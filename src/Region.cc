#include <cstring>
#include <stdexcept>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

#include "kickmsg/Region.h"
#include "kickmsg/os/Time.h"

namespace kickmsg
{
    namespace
    {
        void validate_config(channel::Type type, channel::Config const& cfg)
        {
            if (type != channel::PubSub and type != channel::Broadcast)
            {
                throw std::runtime_error("Unsupported channel type");
            }
            if (not is_power_of_two(cfg.sub_ring_capacity))
            {
                throw std::runtime_error("sub_ring_capacity must be a power of 2");
            }
            if (cfg.pool_size == 0)
            {
                throw std::runtime_error("pool_size must be > 0");
            }
            if (cfg.max_subscribers == 0)
            {
                throw std::runtime_error("max_subscribers must be > 0");
            }
            if (cfg.max_payload_size == 0)
            {
                throw std::runtime_error("max_payload_size must be > 0");
            }
        }

        struct RegionLayout
        {
            std::size_t header_size;
            std::size_t ring_stride;
            std::size_t slot_stride;
            std::size_t sub_rings_offset;
            std::size_t pool_offset;
            std::size_t total_size;
            uint16_t    creator_len;
        };

        RegionLayout compute_layout(channel::Config const& cfg, char const* creator_name)
        {
            RegionLayout layout;
            layout.creator_len      = static_cast<uint16_t>(std::strlen(creator_name));
            layout.header_size      = align_up(sizeof(Header) + layout.creator_len, CACHE_LINE);
            layout.ring_stride      = align_up(
                sizeof(SubRingHeader) + cfg.sub_ring_capacity * sizeof(Entry), CACHE_LINE);
            layout.slot_stride      = align_up(sizeof(SlotHeader) + cfg.max_payload_size, CACHE_LINE);
            layout.sub_rings_offset = layout.header_size;
            layout.pool_offset      = layout.sub_rings_offset + cfg.max_subscribers * layout.ring_stride;
            layout.total_size       = layout.pool_offset + cfg.pool_size * layout.slot_stride;
            return layout;
        }
    }

    void SharedRegion::stamp_new_region(channel::Type type, channel::Config const& cfg,
                                        char const* creator_name, std::size_t total_size,
                                        std::size_t sub_rings_offset, std::size_t pool_offset,
                                        std::size_t ring_stride,     std::size_t slot_stride,
                                        uint16_t    creator_len)
    {
        std::memset(base(), 0, total_size);

        auto* h = header();
        h->version           = VERSION;
        h->channel_type      = type;
        h->total_size        = total_size;
        h->sub_rings_offset  = sub_rings_offset;
        h->pool_offset       = pool_offset;
        h->max_subs          = cfg.max_subscribers;
        h->sub_ring_capacity = cfg.sub_ring_capacity;
        h->sub_ring_mask     = cfg.sub_ring_capacity - 1;
        h->pool_size         = cfg.pool_size;
        h->slot_data_size    = cfg.max_payload_size;
        h->slot_stride       = slot_stride;
        h->sub_ring_stride   = ring_stride;
        h->commit_timeout_us = static_cast<uint64_t>(cfg.commit_timeout.count());
        h->config_hash       = compute_config_hash(type, cfg);
        h->creator_pid       = static_cast<uint64_t>(getpid());
        h->created_at_ns     = static_cast<uint64_t>(kickmsg::since_epoch().count());
        h->creator_name_len  = creator_len;
        std::memcpy(header_creator_name(h), creator_name, creator_len);

        // Optional payload schema: publish directly before the magic store.
        // No claim state machine needed at creation because (a) we are the
        // only writer — no concurrent claimant can race — and (b) the
        // release-store of MAGIC below carries all preceding writes,
        // including the memcpy into schema_data and this relaxed store of
        // schema_state, across to any reader that acquire-loads MAGIC.
        // The relaxed is therefore correct; do NOT "fix" it to release in
        // isolation — MAGIC is the sole publication fence for this region.
        if (cfg.schema.has_value())
        {
            std::memcpy(&h->schema_data, &*cfg.schema, sizeof(SchemaInfo));
            h->schema_state.store(schema::Set, std::memory_order_relaxed);
        }

        h->free_top = tagged_pack(0, INVALID_SLOT);

        for (uint32_t i = 0; i < cfg.pool_size; ++i)
        {
            auto* slot = slot_at(base(), h, i);
            slot->refcount = 0;
            treiber_push(h->free_top, slot, i);
        }

        for (uint32_t i = 0; i < cfg.max_subscribers; ++i)
        {
            auto* ring = sub_ring_at(base(), h, i);
            ring->state_flight = ring::make_packed(ring::Free);
            ring->write_pos    = 0;
            ring->has_waiter   = 0;
        }

        // Write magic LAST with release: create_or_open() polls magic with
        // acquire, so all preceding init stores are visible once magic == MAGIC.
        h->magic.store(MAGIC, std::memory_order_release);
    }

    SharedRegion SharedRegion::create(char const* name, channel::Type type,
                                     channel::Config const& cfg,
                                     char const* creator_name)
    {
        validate_config(type, cfg);
        RegionLayout layout = compute_layout(cfg, creator_name);

        SharedRegion region;
        region.name_ = name;
        region.shm_.create(name, layout.total_size);
        region.stamp_new_region(type, cfg, creator_name,
                                layout.total_size, layout.sub_rings_offset,
                                layout.pool_offset, layout.ring_stride,
                                layout.slot_stride, layout.creator_len);
        return region;
    }

    SharedRegion SharedRegion::open(char const* name)
    {
        SharedRegion region;
        region.name_ = name;
        region.shm_.open(name);

        auto* h = region.header();
        if (h->magic.load(std::memory_order_acquire) != MAGIC)
        {
            throw std::runtime_error("Invalid shared memory (bad magic)");
        }
        if (h->version != VERSION)
        {
            throw std::runtime_error("Version mismatch");
        }

        return region;
    }

    SharedRegion SharedRegion::create_or_open(char const* name, channel::Type type,
                                              channel::Config const& cfg,
                                              char const* creator_name)
    {
        validate_config(type, cfg);
        RegionLayout layout = compute_layout(cfg, creator_name);

        // Try to be the creator.  On success, try_create leaves the
        // SharedMemory fully mapped — we stamp the header directly rather
        // than closing and re-entering SharedMemory::create, which would
        // require either O_TRUNC (rejected on Darwin) or shm_unlink +
        // recreate (introduces a tiny race window where a concurrent
        // caller could see the name missing or point to a different
        // object than the one they initially observed).
        SharedRegion region;
        region.name_ = name;
        if (region.shm_.try_create(name, layout.total_size))
        {
            region.stamp_new_region(type, cfg, creator_name,
                                    layout.total_size, layout.sub_rings_offset,
                                    layout.pool_offset, layout.ring_stride,
                                    layout.slot_stride, layout.creator_len);
            return region;
        }
        region.name_.clear();  // we didn't create; fall through to open loop

        uint64_t expected_hash = compute_config_hash(type, cfg);

        for (int i = 0; i < 200; ++i)
        {
            try
            {
                SharedMemory shm;
                shm.open(name);

                auto* h = static_cast<Header*>(shm.address());
                if (h->magic.load(std::memory_order_acquire) == MAGIC and h->version == VERSION)
                {
                    if (h->config_hash != expected_hash)
                    {
                        throw std::runtime_error(
                            std::string{"Config mismatch on existing region: "} + name);
                    }
                    SharedRegion region;
                    region.name_ = name;
                    region.shm_  = std::move(shm);
                    return region;
                }

                shm.close();
            }
            catch (std::runtime_error const&)
            {
                throw;
            }
            catch (...)
            {
            }
            kickmsg::sleep(10ms);
        }

        throw std::runtime_error(
            std::string{"Timed out waiting for region init: "} + name);
    }

    void SharedRegion::unlink()
    {
        if (not name_.empty())
        {
            SharedMemory::unlink(name_);
        }
    }

    SharedRegion::HealthReport SharedRegion::diagnose()
    {
        auto* b = base();
        auto* h = header();
        HealthReport report{};

        // Schema slot wedged at Claiming: crashed claimant that CAS'd but
        // never reached Set.  Mirrors the operator-surface pattern of
        // retired_rings/locked_entries — reset_schema_claim() recovers it.
        report.schema_stuck =
            (h->schema_state.load(std::memory_order_acquire) == schema::Claiming);

        for (uint64_t i = 0; i < h->max_subs; ++i)
        {
            auto* ring    = sub_ring_at(b, h, static_cast<uint32_t>(i));
            auto* entries = ring_entries(ring);
            uint64_t wp   = ring->write_pos.load(std::memory_order_acquire);
            uint64_t cap  = h->sub_ring_capacity;

            uint64_t start = 0;
            if (wp > cap)
            {
                start = wp - cap;
            }
            for (uint64_t pos = start; pos < wp; ++pos)
            {
                auto& e = entries[pos & h->sub_ring_mask];
                if (e.sequence.load(std::memory_order_acquire) == LOCKED_SEQUENCE)
                {
                    ++report.locked_entries;
                }
            }

            uint32_t    packed    = ring->state_flight.load(std::memory_order_acquire);
            ring::State state     = ring::get_state(packed);
            uint32_t    in_flight = ring::get_in_flight(packed);

            if (state == ring::Live)
            {
                ++report.live_rings;
            }
            else if (state == ring::Free and in_flight > 0)
            {
                ++report.retired_rings;
            }
            else if (state == ring::Draining and in_flight > 0)
            {
                ++report.draining_rings;
            }
        }

        return report;
    }

    std::size_t SharedRegion::repair_locked_entries()
    {
        auto* b   = base();
        auto* h   = header();
        std::size_t repaired = 0;

        for (uint64_t i = 0; i < h->max_subs; ++i)
        {
            auto*    ring    = sub_ring_at(b, h, static_cast<uint32_t>(i));
            auto*    entries = ring_entries(ring);
            uint64_t wp      = ring->write_pos.load(std::memory_order_acquire);
            uint64_t cap     = h->sub_ring_capacity;

            uint64_t start = 0;
            if (wp > cap)
            {
                start = wp - cap;
            }
            for (uint64_t pos = start; pos < wp; ++pos)
            {
                auto&    e   = entries[pos & h->sub_ring_mask];
                uint64_t seq = e.sequence.load(std::memory_order_acquire);

                if (seq == LOCKED_SEQUENCE)
                {
                    // The crashed publisher may have written garbage into
                    // slot_idx/payload_len. Mark the entry as having no
                    // valid slot so subscribers skip it and future evictions
                    // don't release a stale index.
                    e.slot_idx.store(INVALID_SLOT, std::memory_order_relaxed);
                    e.payload_len.store(0, std::memory_order_relaxed);

                    // Commit with the sequence future publishers expect:
                    // pos + 1 (not prev_seq). A publisher wrapping to this
                    // position will CAS(pos + 1 → LOCKED), which now succeeds.
                    e.sequence.store(pos + 1, std::memory_order_release);
                    ++repaired;
                }
            }
        }

        return repaired;
    }

    std::size_t SharedRegion::reset_retired_rings()
    {
        auto* b = base();
        auto* h = header();
        std::size_t reset = 0;

        for (uint64_t i = 0; i < h->max_subs; ++i)
        {
            auto*    ring   = sub_ring_at(b, h, static_cast<uint32_t>(i));
            uint32_t packed = ring->state_flight.load(std::memory_order_acquire);

            if (ring::get_state(packed) == ring::Free
                and ring::get_in_flight(packed) > 0)
            {
                ring->state_flight.store(ring::make_packed(ring::Free),
                                         std::memory_order_release);
                ++reset;
            }
        }

        return reset;
    }

    std::optional<SchemaInfo> SharedRegion::schema() const
    {
        auto const* h     = header();
        uint32_t    state = h->schema_state.load(std::memory_order_acquire);
        if (state != schema::Set)
        {
            // Unset or a claim is mid-write — no stable payload to return.
            return std::nullopt;
        }
        SchemaInfo out;
        std::memcpy(&out, &h->schema_data, sizeof(SchemaInfo));
        return out;
    }

    bool SharedRegion::try_claim_schema(SchemaInfo const& info)
    {
        auto*    h        = header();
        uint32_t expected = schema::Unset;

        // Acq_rel on success: acquire so any prior claim's Set is visible on
        // retry paths; release so our pre-CAS zeroing (none here) is ordered
        // before subsequent writes to schema_data (still fine: Claiming is
        // only visible once CAS wins, and the payload write happens-before
        // the Set release-store below).
        if (h->schema_state.compare_exchange_strong(
                expected, schema::Claiming,
                std::memory_order_acq_rel,
                std::memory_order_acquire))
        {
            std::memcpy(&h->schema_data, &info, sizeof(SchemaInfo));
            // Release: pairs with the acquire in schema() so a reader that
            // observes Set sees the fully-written schema_data payload.
            h->schema_state.store(schema::Set, std::memory_order_release);
            return true;
        }

        // Someone else won the claim.  If they're mid-write, wait briefly
        // for the state to settle at Set so a follow-up schema() read is
        // meaningful — but bound the wait: a claimant that crashed between
        // CAS→Claiming and store→Set leaves the slot wedged.  Operators
        // recover such a wedge with reset_schema_claim(), and diagnose()
        // surfaces it via HealthReport::schema_stuck.
        //
        // MAX_YIELDS is chosen empirically: a memcpy of SchemaInfo (512 B)
        // plus a release-store completes in well under a microsecond on
        // any target platform, so 1024 yields gives the legitimate winner
        // several orders of magnitude more than it needs while keeping the
        // worst-case wait on a crashed claimant imperceptible to callers.
        constexpr int MAX_YIELDS = 1024;
        for (int i = 0; i < MAX_YIELDS and expected == schema::Claiming; ++i)
        {
            std::this_thread::yield();
            expected = h->schema_state.load(std::memory_order_acquire);
        }
        return false;
    }

    bool SharedRegion::reset_schema_claim()
    {
        // Force a wedged Claiming state back to Unset so a new claim can
        // proceed.  Analogous to reset_retired_rings(): a deliberate
        // post-crash action, NOT safe under live traffic.  Only call after
        // confirming the original claimant is gone; otherwise a slow-but-
        // alive writer could finish its memcpy into schema_data and then
        // release-store Set, while a new claimant is concurrently using
        // the slot — producing torn bytes.
        uint32_t expected = schema::Claiming;
        return header()->schema_state.compare_exchange_strong(
            expected, schema::Unset,
            std::memory_order_acq_rel,
            std::memory_order_relaxed);
    }

    std::size_t SharedRegion::reclaim_orphaned_slots()
    {
        auto* b = base();
        auto* h = header();

        // Build a set of all slot indices referenced by committed ring entries.
        std::vector<bool> referenced(h->pool_size, false);

        for (uint64_t i = 0; i < h->max_subs; ++i)
        {
            auto*    ring    = sub_ring_at(b, h, static_cast<uint32_t>(i));
            auto*    entries = ring_entries(ring);
            uint64_t wp      = ring->write_pos.load(std::memory_order_acquire);
            uint64_t cap     = h->sub_ring_capacity;

            uint64_t start = 0;
            if (wp > cap)
            {
                start = wp - cap;
            }
            for (uint64_t pos = start; pos < wp; ++pos)
            {
                auto&    e   = entries[pos & h->sub_ring_mask];
                uint64_t seq = e.sequence.load(std::memory_order_acquire);

                // Skip uncommitted and locked entries.
                if (seq >= pos + 1 and seq != LOCKED_SEQUENCE)
                {
                    uint32_t idx = e.slot_idx.load(std::memory_order_relaxed);
                    if (idx < h->pool_size)
                    {
                        referenced[idx] = true;
                    }
                }
            }
        }

        // Reclaim unreferenced slots with refcount > 0.
        std::size_t reclaimed = 0;
        for (uint64_t idx = 0; idx < h->pool_size; ++idx)
        {
            if (referenced[idx])
            {
                continue;
            }

            auto*    slot = slot_at(b, h, static_cast<uint32_t>(idx));
            uint32_t rc   = slot->refcount.load(std::memory_order_acquire);
            if (rc > 0)
            {
                slot->refcount.store(0, std::memory_order_release);
                treiber_push(h->free_top, slot, static_cast<uint32_t>(idx));
                ++reclaimed;
            }
        }

        return reclaimed;
    }
}
