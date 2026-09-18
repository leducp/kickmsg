#include "kickmsg/types.h"
#include "kickmsg/Hash.h"

namespace kickmsg
{
    void clear_owner(SubRingHeader* ring)
    {
        // Relaxed: the following Free release-store publishes these fields.
        // Clear has_waiter so a replacement does not inherit a stale wake mode.
        ring->has_waiter.store(ring::WaiterNone, std::memory_order_relaxed);
        ring->owner_pid.store(0, std::memory_order_relaxed);
        ring->owner_starttime.store(0, std::memory_order_relaxed);
    }

    SubRingHeader* sub_ring_at(void* base, Header const* h, uint32_t idx)
    {
        auto* p = static_cast<uint8_t*>(base) + h->sub_rings_offset;
        return reinterpret_cast<SubRingHeader*>(p + idx * h->sub_ring_stride);
    }

    SubRingHeader* sub_ring_at(void* base, Geometry const& g, uint32_t idx)
    {
        auto* p = static_cast<uint8_t*>(base) + g.sub_rings_offset;
        return reinterpret_cast<SubRingHeader*>(p + idx * g.sub_ring_stride);
    }

    Entry* ring_entries(SubRingHeader* ring)
    {
        return reinterpret_cast<Entry*>(
            reinterpret_cast<uint8_t*>(ring) + sizeof(SubRingHeader));
    }

    SlotHeader* slot_at(void* base, Header const* h, uint32_t idx)
    {
        auto* p = static_cast<uint8_t*>(base) + h->pool_offset;
        return reinterpret_cast<SlotHeader*>(p + idx * h->slot_stride);
    }

    SlotHeader* slot_at(void* base, Geometry const& g, uint32_t idx)
    {
        auto* p = static_cast<uint8_t*>(base) + g.pool_offset;
        return reinterpret_cast<SlotHeader*>(p + idx * g.slot_stride);
    }

    SlotHeader* slot_at(void* pool_base, std::size_t slot_stride, uint32_t idx)
    {
        auto* p = static_cast<uint8_t*>(pool_base);
        return reinterpret_cast<SlotHeader*>(p + idx * slot_stride);
    }

    uint8_t* slot_data(SlotHeader* slot)
    {
        return reinterpret_cast<uint8_t*>(slot + 1);
    }

    char* header_creator_name(Header* h)
    {
        return reinterpret_cast<char*>(h) + sizeof(Header);
    }

    // Config hash field order is part of the ABI; changes require a VERSION bump.
    uint64_t compute_config_hash(channel::Type type, channel::Config const& cfg)
    {
        uint64_t h = hash::fnv1a_64(type);
        h = hash::fnv1a_64(cfg.max_subscribers,        h);
        h = hash::fnv1a_64(cfg.sub_ring_capacity,      h);
        h = hash::fnv1a_64(cfg.pool_size,              h);
        h = hash::fnv1a_64(cfg.max_payload_size,       h);
        h = hash::fnv1a_64(cfg.commit_timeout.count(), h);
        return h;
    }

    namespace schema
    {
        uint32_t diff(SchemaInfo const& a, SchemaInfo const& b)
        {
            uint32_t d = Equal;
            if (a.identity      != b.identity)      d |= Identity;
            if (a.layout        != b.layout)        d |= Layout;
            if (a.version       != b.version)       d |= Version;
            // Compare through NUL or the fixed field size.
            if (std::strncmp(a.name, b.name, sizeof(a.name)) != 0) d |= Name;
            if (a.identity_algo != b.identity_algo) d |= IdentityAlgo;
            if (a.layout_algo   != b.layout_algo)   d |= LayoutAlgo;
            // Ignore reserved fields for forward compatibility.
            return d;
        }
    }

    bool entry_steal_and_skip(Entry& e, uint64_t pos, uint64_t observed)
    {
        // CAS-own the sequence; a live writer that moves first wins.
        uint64_t expected = observed;
        if (not e.sequence.compare_exchange_strong(expected, seq_repair(pos),
                std::memory_order_acquire, std::memory_order_relaxed))
        {
            return false;
        }

        // Keep the slot claim; the next publisher or drainer releases its reference.
        e.sequence.store(seq_skip(pos), std::memory_order_release);
        return true;
    }

    void treiber_push(std::atomic<uint64_t>& top, SlotHeader* slot, uint32_t slot_idx)
    {
        uint64_t old_top = top.load(std::memory_order_relaxed);
        uint64_t new_top;
        do
        {
            slot->next_free.store(tagged_idx(old_top), std::memory_order_relaxed);
            new_top = tagged_pack(tagged_gen(old_top) + 1, slot_idx);
        }
        // Release: ensures next_free write is visible to any thread that later pops this slot.
        while (not top.compare_exchange_weak(old_top, new_top, std::memory_order_release, std::memory_order_relaxed));
    }

    uint32_t treiber_pop(std::atomic<uint64_t>& top, void* base, Header const* h)
    {
        return treiber_pop(top, static_cast<uint8_t*>(base) + h->pool_offset, h->slot_stride,
                           h->pool_size);
    }

    uint32_t treiber_pop(std::atomic<uint64_t>& top, void* base, Geometry const& g)
    {
        return treiber_pop(top, static_cast<uint8_t*>(base) + g.pool_offset, g.slot_stride,
                           g.pool_size);
    }

    void treiber_push(std::atomic<uint64_t>& top, void* pool_base, std::size_t slot_stride, uint32_t slot_idx)
    {
        treiber_push(top, slot_at(pool_base, slot_stride, slot_idx), slot_idx);
    }

    uint32_t treiber_pop(std::atomic<uint64_t>& top, void* pool_base, std::size_t slot_stride,
                         uint64_t pool_size)
    {
        // Acquire: pairs with the release in push to see the pushed slot's next_free.
        uint64_t old_top = top.load(std::memory_order_acquire);
        while (true)
        {
            uint32_t idx = tagged_idx(old_top);
            if (idx == INVALID_SLOT)
            {
                return INVALID_SLOT;
            }
            // Bounds-check each shared free-list index before dereferencing it.
            if (idx >= pool_size)
            {
                return INVALID_SLOT;
            }
            auto*    slot = slot_at(pool_base, slot_stride, idx);
            uint32_t next = slot->next_free.load(std::memory_order_relaxed);
            uint64_t new_top = tagged_pack(tagged_gen(old_top) + 1, next);
            // Acq_rel: release publishes the new top, acquire synchronizes with the last push.
            // Failure is acquire: on retry we need to see the next_free written by whoever changed top.
            if (top.compare_exchange_weak(old_top, new_top, std::memory_order_acq_rel, std::memory_order_acquire))
            {
                return idx;
            }
        }
    }
}
