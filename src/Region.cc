#include <stdexcept>
#include <thread>

#include "kickmsg/Region.h"
#include "kickmsg/os/Process.h"
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
            // Slot indices ride in Entry::meta's 24-bit field; see types.h.
            if (cfg.pool_size > MAX_POOL_SIZE)
            {
                throw std::runtime_error("pool_size exceeds MAX_POOL_SIZE");
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
            if (creator_name == nullptr)
            {
                throw std::runtime_error("creator_name must not be null");
            }
            std::size_t name_len = std::strlen(creator_name);
            // creator_name_len is a uint16_t header field; a silent cast
            // would truncate the name and desync required_size/attach_create.
            if (name_len > UINT16_MAX)
            {
                throw std::runtime_error("creator_name exceeds 65535 bytes");
            }

            // Check additions and alignment before computing strides, which must not wrap.
            if (cfg.sub_ring_capacity > (SIZE_MAX - sizeof(SubRingHeader) - CACHE_LINE) / sizeof(Entry))
            {
                throw std::runtime_error("Config too large: ring stride overflows");
            }
            if (cfg.max_payload_size > SIZE_MAX - sizeof(SlotHeader) - CACHE_LINE)
            {
                throw std::runtime_error("Config too large: slot stride overflows");
            }

            RegionLayout layout;
            layout.creator_len      = static_cast<uint16_t>(name_len);
            layout.header_size      = align_up(sizeof(Header) + layout.creator_len, CACHE_LINE);
            layout.ring_stride      = align_up(sizeof(SubRingHeader) + cfg.sub_ring_capacity * sizeof(Entry), CACHE_LINE);
            layout.slot_stride      = align_up(sizeof(SlotHeader) + cfg.max_payload_size, CACHE_LINE);
            layout.sub_rings_offset = layout.header_size;

            // Reject sizes that overflow the mapped region length.
            if (cfg.max_subscribers > (SIZE_MAX - layout.sub_rings_offset) / layout.ring_stride)
            {
                throw std::runtime_error("Config too large: subscriber rings overflow");
            }
            layout.pool_offset = layout.sub_rings_offset + cfg.max_subscribers * layout.ring_stride;
            if (cfg.pool_size > (SIZE_MAX - layout.pool_offset) / layout.slot_stride)
            {
                throw std::runtime_error("Config too large: slot pool overflow");
            }
            layout.total_size = layout.pool_offset + cfg.pool_size * layout.slot_stride;
            return layout;
        }
    }

    void SharedRegion::capture_geometry()
    {
        auto const* h = header();
        geom_.sub_rings_offset   = h->sub_rings_offset;
        geom_.sub_ring_stride    = h->sub_ring_stride;
        geom_.sub_ring_capacity  = h->sub_ring_capacity;
        geom_.sub_ring_mask      = h->sub_ring_mask;
        geom_.pool_offset        = h->pool_offset;
        geom_.slot_stride        = h->slot_stride;
        geom_.pool_size          = h->pool_size;
        geom_.slot_data_size     = h->slot_data_size;
        geom_.max_subs           = h->max_subs;
        geom_.commit_timeout_us  = h->commit_timeout_us;
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
        h->creator_pid       = kickmsg::current_pid();
        h->created_at_ns     = static_cast<uint64_t>(kickmsg::since_epoch().count());
        h->creator_name_len  = creator_len;
        std::memcpy(header_creator_name(h), creator_name, creator_len);

        // The creator is the only writer. The release-store of MAGIC publishes
        // the schema bytes and relaxed schema_state store.
        if (cfg.schema.has_value())
        {
            std::memcpy(&h->schema_data, &*cfg.schema, sizeof(SchemaInfo));
            h->schema_state.store(schema::Set, std::memory_order_relaxed);
        }

        h->free_top      = tagged_pack(0, INVALID_SLOT);
        h->steal_count   = 0;
        h->identity_hash = cfg.identity;

        for (uint32_t i = 0; i < cfg.pool_size; ++i)
        {
            auto* slot = slot_at(base(), h, i);
            slot->refcount = 0;
            treiber_push(h->free_top, slot, i);
        }

        for (uint32_t i = 0; i < cfg.max_subscribers; ++i)
        {
            auto* ring = sub_ring_at(base(), h, i);
            ring->state_flight  = ring::make_packed(ring::Free);
            ring->write_pos     = 0;
            ring->dropped_count = 0;
            ring->lost_count    = 0;
            // attach_create's buffer need not arrive zeroed the way a mapping does.
            clear_owner(ring);
        }

        // Write magic LAST with release: create_or_open() polls magic with
        // acquire, so all preceding init stores are visible once magic == MAGIC.
        h->magic.store(MAGIC, std::memory_order_release);

        // Capture the initialized geometry for later pointer arithmetic.
        capture_geometry();
    }

    namespace
    {
        /// Validate all geometry used for pointer arithmetic.
        /// The caller has checked magic, version, and size >= total_size.
        void validate_header_geometry(Header const* h)
        {
            // Only PubSub and Broadcast have ring geometry.
            if (h->channel_type != channel::PubSub and
                h->channel_type != channel::Broadcast)
            {
                throw std::runtime_error("Header geometry: unsupported channel type");
            }
            if (h->total_size < sizeof(Header))
            {
                throw std::runtime_error(
                    "Header geometry: total_size smaller than Header");
            }

            // Nonzero counts and strides are required by the divisions below.
            if (h->max_subs == 0 or
                h->pool_size == 0 or
                h->slot_data_size == 0 or
                h->sub_ring_capacity == 0 or
                h->slot_stride == 0 or
                h->sub_ring_stride == 0)
            {
                throw std::runtime_error(
                    "Header geometry: zero-cardinality field");
            }

            // Slot indices must stay representable in Entry::meta, or a peer
            // could name a slot the claim word cannot round-trip.
            if (h->pool_size > MAX_POOL_SIZE)
            {
                throw std::runtime_error("Header geometry: pool_size exceeds MAX_POOL_SIZE");
            }
            if (not is_power_of_two(h->sub_ring_capacity))
            {
                throw std::runtime_error("Header geometry: sub_ring_capacity not a power of 2");
            }
            if (h->sub_ring_mask != h->sub_ring_capacity - 1)
            {
                throw std::runtime_error("Header geometry: sub_ring_mask inconsistent with capacity");
            }

            // Sub-rings span [sub_rings_offset, pool_offset); pool spans
            // [pool_offset, total_size).
            if (h->sub_rings_offset < sizeof(Header) or
                h->sub_rings_offset >= h->pool_offset or
                h->pool_offset >= h->total_size)
            {
                throw std::runtime_error("Header geometry: ring/pool offsets out of range");
            }

            // creator_name tail lives in [sizeof(Header), sub_rings_offset);
            // bound it there so info() can't read into the ring/pool area.
            if (h->creator_name_len > h->sub_rings_offset - sizeof(Header))
            {
                throw std::runtime_error("Header geometry: creator_name_len exceeds tail");
            }

            // Shared atomics require cache-line-aligned offsets and strides.
            if ((h->sub_rings_offset % CACHE_LINE) != 0 or
                (h->pool_offset      % CACHE_LINE) != 0 or
                (h->sub_ring_stride  % CACHE_LINE) != 0 or
                (h->slot_stride      % CACHE_LINE) != 0)
            {
                throw std::runtime_error("Header geometry: offset or stride not cache-line aligned");
            }

            // Bound sub_ring_capacity by total_size before multiplying so
            // the min_ring_stride product can't overflow on a junk value.
            if (h->sub_ring_capacity > h->total_size / sizeof(Entry))
            {
                throw std::runtime_error("Header geometry: sub_ring_capacity exceeds region");
            }
            std::size_t const min_ring_stride = sizeof(SubRingHeader) + h->sub_ring_capacity * sizeof(Entry);
            if (h->sub_ring_stride < min_ring_stride)
            {
                throw std::runtime_error("Header geometry: sub_ring_stride too small");
            }
            // Bound the payload size before adding to it, to prevent overflow.
            if (h->slot_data_size > h->total_size - sizeof(SlotHeader))
            {
                throw std::runtime_error("Header geometry: slot_data_size exceeds region");
            }
            std::size_t const min_slot_stride = sizeof(SlotHeader) + h->slot_data_size;
            if (h->slot_stride < min_slot_stride)
            {
                throw std::runtime_error("Header geometry: slot_stride too small");
            }

            // max_subs * sub_ring_stride must fit in the rings region.
            // Division-based bound avoids mul-overflow on a junk max_subs.
            std::size_t const rings_space = h->pool_offset - h->sub_rings_offset;
            if (h->max_subs > rings_space / h->sub_ring_stride)
            {
                throw std::runtime_error("Header geometry: subscriber rings overflow pool_offset");
            }

            // pool_size * slot_stride must fit in the pool region.
            std::size_t const pool_space = h->total_size - h->pool_offset;
            if (h->pool_size > pool_space / h->slot_stride)
            {
                throw std::runtime_error("Header geometry: slot pool overflow total_size");
            }
        }

        // Validate the header and geometry before using offsets into the mapping.
        void validate_opened(void* address, std::size_t size)
        {
            if (size < sizeof(Header))
            {
                throw std::runtime_error("Buffer smaller than region Header");
            }
            auto* h = static_cast<Header*>(address);
            if (h->magic.load(std::memory_order_acquire) != MAGIC)
            {
                throw std::runtime_error("Invalid shared memory (bad magic)");
            }
            if (h->version != VERSION)
            {
                throw std::runtime_error("Version mismatch");
            }
            if (size < h->total_size)
            {
                throw std::runtime_error("Buffer smaller than embedded region total_size");
            }
            validate_header_geometry(h);
        }

        // True if the ring's recorded owner is provably gone. owner_pid == 0
        // means unowned or a claim in progress -> not dead. Mirrors
        bool ring_owner_dead(SubRingHeader const* ring)
        {
            // Acquire syncs with the subscriber's release-store of owner_pid,
            // so a nonzero pid comes with a matching starttime.
            uint64_t pid = ring->owner_pid.load(std::memory_order_acquire);
            uint64_t stored = ring->owner_starttime.load(std::memory_order_relaxed);
            return owner_is_dead(pid, stored);
        }
    }

    std::size_t SharedRegion::required_size(channel::Config const& cfg,
                                            char const* creator_name)
    {
        validate_config(channel::PubSub, cfg);
        return compute_layout(cfg, creator_name).total_size;
    }

    SharedRegion SharedRegion::attach_create(void* address, std::size_t size,
                                             channel::Type type,
                                             channel::Config const& cfg,
                                             char const* creator_name,
                                             char const* label)
    {
        validate_config(type, cfg);
        if (reinterpret_cast<std::uintptr_t>(address) % CACHE_LINE != 0)
        {
            throw std::runtime_error("attach_create: address not CACHE_LINE aligned");
        }
        RegionLayout layout = compute_layout(cfg, creator_name);
        if (size < layout.total_size)
        {
            throw std::runtime_error("attach_create: buffer smaller than required_size");
        }

        SharedRegion region;
        region.base_ = address;
        region.size_ = size;
        region.name_ = label;
        region.stamp_new_region(type, cfg, creator_name,
                                layout.total_size, layout.sub_rings_offset,
                                layout.pool_offset, layout.ring_stride,
                                layout.slot_stride, layout.creator_len);
        return region;
    }

    SharedRegion SharedRegion::attach_open(void* address, std::size_t size,
                                           char const* label)
    {
        if (reinterpret_cast<std::uintptr_t>(address) % CACHE_LINE != 0)
        {
            throw std::runtime_error("attach_open: address not CACHE_LINE aligned");
        }

        SharedRegion region;
        region.base_ = address;
        region.size_ = size;
        region.name_ = label;
        validate_opened(region.base_, region.size_);
        region.capture_geometry();
        return region;
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
        region.base_ = region.shm_.address();
        region.size_ = layout.total_size;
        region.stamp_new_region(type, cfg, creator_name,
                                layout.total_size, layout.sub_rings_offset,
                                layout.pool_offset, layout.ring_stride,
                                layout.slot_stride, layout.creator_len);
        return region;
    }

    SharedRegion SharedRegion::open(char const* name, uint64_t expected_identity)
    {
        SharedRegion region;
        region.name_ = name;
        region.shm_.open(name);
        region.base_ = region.shm_.address();
        region.size_ = region.shm_.size();
        validate_opened(region.base_, region.size_);
        region.capture_geometry();
        uint64_t stamped = region.header()->identity_hash;
        if (expected_identity != 0 and stamped != 0 and stamped != expected_identity)
        {
            throw std::runtime_error(std::string{"Identity mismatch on existing region (shm name collision): "} + name);
        }
        return region;
    }

    SharedRegion SharedRegion::create_or_open(char const* name, channel::Type type,
                                              channel::Config const& cfg,
                                              char const* creator_name)
    {
        validate_config(type, cfg);
        RegionLayout layout = compute_layout(cfg, creator_name);

        // Initialize the mapping returned by try_create; reopening could race
        // with another creator or require truncating the shared object.
        SharedRegion region;
        region.name_ = name;
        if (region.shm_.try_create(name, layout.total_size))
        {
            region.base_ = region.shm_.address();
            region.size_ = layout.total_size;
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
            SharedMemory shm;
            if (shm.try_open(name))
            {
                auto* h = static_cast<Header*>(shm.address());
                if (h->magic.load(std::memory_order_acquire) == MAGIC and
                    h->version == VERSION)
                {
                    if (h->config_hash != expected_hash)
                    {
                        throw std::runtime_error(std::string{"Config mismatch on existing region: "} + name);
                    }
                    if (cfg.identity != 0 and h->identity_hash != 0
                        and h->identity_hash != cfg.identity)
                    {
                        throw std::runtime_error(std::string{"Identity mismatch on existing region (shm name collision): "} + name);
                    }
                    SharedRegion region;
                    region.name_ = name;
                    region.shm_  = std::move(shm);
                    region.base_ = region.shm_.address();
                    region.size_ = region.shm_.size();
                    // The config hash does not cover offsets, strides, or total_size.
                    validate_opened(region.base_, region.size_);
                    region.capture_geometry();
                    return region;
                }
                // SHM exists but magic/version not ready yet -- creator
                // is still mid-init.  Close and retry.
            }
            // try_open returned false (ENOENT) or magic not ready -> retry.
            kickmsg::sleep(10ms);
        }

        throw std::runtime_error(std::string{"Timed out waiting for region init: "} + name);
    }

    void SharedRegion::unlink()
    {
        // Unlinking removes the name; existing mappings remain valid.
        // Injected regions have no OS name to remove.
        if (shm_.is_open() and not name_.empty())
        {
            SharedMemory::unlink(name_);
        }
    }

    SharedRegion::HealthReport SharedRegion::diagnose()
    {
        auto* b = base();
        auto* h = header();
        HealthReport report{};

        // Claiming may be transient or left by a crashed schema writer.
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
                auto&    e   = entries[pos & h->sub_ring_mask];
                uint64_t seq = e.sequence.load(std::memory_order_acquire);

                // Case A: explicitly locked, never committed.
                // Case B: more than one full wrap behind -- stale from a
                //         publisher that crashed before the CAS lock.
                if (seq_is_locked(seq) or seq_pos(seq) + cap < pos + 1)
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

            // Count dead ring owners separately from ring state.
            if ((state == ring::Live or state == ring::Draining
                 or state == ring::Reclaiming)
                and ring_owner_dead(ring))
            {
                ++report.dead_rings;
            }
        }

        return report;
    }

    std::size_t SharedRegion::repair_locked_entries()
    {
        auto* b   = base();
        auto* h   = header();
        std::size_t repaired = 0;

        struct LockedCandidate
        {
            Entry*   entry;
            uint64_t pos;
            uint64_t seq;
        };
        std::vector<LockedCandidate> candidates;

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
                auto&    e        = entries[pos & h->sub_ring_mask];
                uint64_t seq      = e.sequence.load(std::memory_order_acquire);
                uint64_t expected = pos + 1;

                if (seq_is_locked(seq))
                {
                    // Case A: may be a healthy in-flight commit -- never
                    // steal on first sight, defer to the grace pass.
                    candidates.push_back({&e, pos, seq});
                }
                else if (seq_pos(seq) + cap < expected)
                {
                    // Case B: committed >1 wrap behind (claimant crashed
                    // before its lock CAS).
                    if (entry_steal_and_skip(e, pos, seq))
                    {
                        h->steal_count.fetch_add(1, std::memory_order_relaxed);
                        ++repaired;
                    }
                }
            }
        }

        if (candidates.empty())
        {
            return repaired;
        }

        // Grace pass: an unchanged lock value across a full commit_timeout
        // proves its (unique) holder exceeded the commit budget.
        kickmsg::sleep(microseconds{h->commit_timeout_us});

        for (auto const& c : candidates)
        {
            if (c.entry->sequence.load(std::memory_order_acquire) != c.seq)
            {
                continue;
            }
            if (entry_steal_and_skip(*c.entry, c.pos, c.seq))
            {
                h->steal_count.fetch_add(1, std::memory_order_relaxed);
                ++repaired;
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

            if (ring::get_state(packed) == ring::Free and
                ring::get_in_flight(packed) > 0)
            {
                // Already Free but unclaimable while in_flight > 0: the store below is
                // the hand-off, so the retraction still goes first.
                clear_owner(ring);
                ring->state_flight.store(ring::make_packed(ring::Free),
                                         std::memory_order_release);
                ++reset;
            }
        }

        return reset;
    }

    std::size_t SharedRegion::reclaim_dead_rings()
    {
        auto* b = base();
        auto* h = header();
        std::size_t reclaimed = 0;

        for (uint64_t i = 0; i < h->max_subs; ++i)
        {
            auto*       ring  = sub_ring_at(b, h, static_cast<uint32_t>(i));
            uint32_t    packed = ring->state_flight.load(std::memory_order_acquire);
            ring::State state  = ring::get_state(packed);

            // Reclaiming residue (reclaimer crashed mid-pass) is only
            // recoverable here.
            if (state != ring::Live and
                state != ring::Draining and
                state != ring::Reclaiming)
            {
                continue;
            }
            if (not ring_owner_dead(ring))
            {
                continue;
            }

            // Use one CAS attempt, then recheck owner death. A retry could acquire
            // a replacement owner's ring; in_flight changes defer this pass.
            uint32_t fresh = ring->state_flight.load(std::memory_order_acquire);
            if (ring::get_state(fresh) != state)
            {
                continue;
            }
            uint32_t claim = (fresh & ~ring::STATE_MASK) | ring::Reclaiming;
            if (not ring->state_flight.compare_exchange_strong(fresh, claim,
                    std::memory_order_acq_rel, std::memory_order_relaxed))
            {
                continue;
            }

            if (ring_owner_dead(ring))
            {
                // Preserve in_flight so a late publisher decrement cannot underflow state.
                clear_owner(ring);

                uint32_t old = claim;
                while (ring::get_state(old) == ring::Reclaiming)
                {
                    uint32_t desired = (old & ~ring::STATE_MASK) | ring::Free;
                    if (ring->state_flight.compare_exchange_weak(old, desired,
                            std::memory_order_release, std::memory_order_acquire))
                    {
                        ++reclaimed;
                        break;
                    }
                }
            }
            else
            {
                // Displaced a live owner (freed + re-claimed between checks):
                // restore it, but only while still Reclaiming -- the owner's
                // own teardown may have moved the state.
                uint32_t old = claim;
                while (ring::get_state(old) == ring::Reclaiming)
                {
                    uint32_t desired = (old & ~ring::STATE_MASK) | state;
                    if (ring->state_flight.compare_exchange_weak(old, desired,
                            std::memory_order_release, std::memory_order_acquire))
                    {
                        break;
                    }
                }
            }
        }

        return reclaimed;
    }

    std::optional<SchemaInfo> SharedRegion::schema() const
    {
        auto const* h     = header();
        uint32_t    state = h->schema_state.load(std::memory_order_acquire);
        if (state != schema::Set)
        {
            // Unset or a claim is mid-write -- no stable payload to return.
            return std::nullopt;
        }
        SchemaInfo out;
        std::memcpy(&out, &h->schema_data, sizeof(SchemaInfo));
        // Ensure the copied schema name is terminated before consumers read it.
        out.name[sizeof(out.name) - 1] = '\0';
        return out;
    }

    bool SharedRegion::try_claim_schema(SchemaInfo const& info)
    {
        auto*    h        = header();
        uint32_t expected = schema::Unset;

        // Acquire observes prior claims; the later Set release-store publishes data.
        if (h->schema_state.compare_exchange_strong(expected, schema::Claiming,
                std::memory_order_acq_rel, std::memory_order_acquire))
        {
            std::memcpy(&h->schema_data, &info, sizeof(SchemaInfo));
            // Release: pairs with the acquire in schema() so a reader that
            // observes Set sees the fully-written schema_data payload.
            h->schema_state.store(schema::Set, std::memory_order_release);
            return true;
        }

        // Wait briefly for an active schema writer, but bound the wait if it died.
        // A confirmed dead claimant can be cleared with reset_schema_claim().
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
        // Only reset after the original claimant has stopped; a live writer could
        // otherwise publish data while a new claimant overwrites it.
        uint32_t expected = schema::Claiming;
        return header()->schema_state.compare_exchange_strong(expected, schema::Unset,
            std::memory_order_acq_rel, std::memory_order_relaxed);
    }

    RegionStats SharedRegion::stats() const
    {
        auto const* b = base();
        auto const* h = header();

        RegionStats out{};
        out.pool_size    = h->pool_size;
        out.total_steals = h->steal_count.load(std::memory_order_relaxed);
        out.rings.reserve(h->max_subs);

        for (uint64_t i = 0; i < h->max_subs; ++i)
        {
            // sub_ring_at needs a non-const base*/header*, but the operation
            // is read-only -- const_cast is safe here.
            auto* ring = sub_ring_at(const_cast<void*>(b),
                                     h, static_cast<uint32_t>(i));
            uint32_t packed = ring->state_flight.load(std::memory_order_acquire);

            RingStats rs{};
            rs.state         = static_cast<uint32_t>(ring::get_state(packed));
            rs.in_flight     = ring::get_in_flight(packed);
            rs.write_pos     = ring->write_pos.load(std::memory_order_acquire);
            rs.dropped_count = ring->dropped_count.load(std::memory_order_relaxed);
            rs.lost_count    = ring->lost_count.load(std::memory_order_relaxed);

            if (rs.state == ring::Live)
            {
                ++out.live_rings;
            }
            // Include Free rings: their frozen write_pos is a valid past observation.
            // The maximum stays monotonic across subscriber changes.
            if (rs.write_pos > out.total_writes)
            {
                out.total_writes = rs.write_pos;
            }
            out.total_drops  += rs.dropped_count;
            out.total_losses += rs.lost_count;

            out.rings.push_back(rs);
        }

        // Bound the free-stack walk by pool_size. Concurrent changes can cause
        // an overcount or undercount.
        uint64_t top = h->free_top.load(std::memory_order_acquire);
        uint32_t idx = tagged_idx(top);
        uint64_t count = 0;
        uint64_t const limit = h->pool_size;
        while (idx != INVALID_SLOT and count < limit)
        {
            if (idx >= h->pool_size) break;
            auto* slot = slot_at(const_cast<void*>(b), h, idx);
            idx = slot->next_free.load(std::memory_order_relaxed);
            ++count;
        }
        out.pool_free = count;

        return out;
    }

    RegionInfo SharedRegion::info() const
    {
        auto const* h = header();
        RegionInfo out{};
        out.shm_name          = name_;
        out.channel_type      = h->channel_type;
        out.version           = h->version;
        out.config_hash       = h->config_hash;
        out.total_size        = h->total_size;
        out.max_subs          = h->max_subs;
        out.sub_ring_capacity = h->sub_ring_capacity;
        out.pool_size         = h->pool_size;
        out.max_payload_size  = h->slot_data_size;
        out.commit_timeout_us = h->commit_timeout_us;
        out.creator_pid       = h->creator_pid;
        out.created_at_ns     = h->created_at_ns;

        // Creator name tail: bytes written at offset sizeof(Header).
        auto const* tail = static_cast<char const*>(base()) + sizeof(Header);
        out.creator_name.assign(tail, h->creator_name_len);
        return out;
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
                auto& e = entries[pos & h->sub_ring_mask];

                // Locked and skip-marked entries still own the slots named by their claims.
                uint32_t biased = meta_slot_biased(e.meta.load(std::memory_order_acquire));
                if (biased != 0 and biased - 1 < h->pool_size)
                {
                    referenced[biased - 1] = true;
                }
            }
        }

        // Under quiescence, free-stack membership also identifies rc == 0 orphans.
        // Bound the walk to handle corrupt cycles.
        std::vector<bool> on_stack(h->pool_size, false);
        uint64_t walked = 0;
        uint32_t idx32  = tagged_idx(h->free_top.load(std::memory_order_acquire));
        while (idx32 != INVALID_SLOT and idx32 < h->pool_size
               and walked < h->pool_size)
        {
            on_stack[idx32] = true;
            idx32 = slot_at(b, h, idx32)->next_free.load(std::memory_order_relaxed);
            ++walked;
        }

        // Reclaim slots that are neither ring-referenced nor on the free
        // stack, regardless of refcount.
        std::size_t reclaimed = 0;
        for (uint64_t idx = 0; idx < h->pool_size; ++idx)
        {
            if (referenced[idx] or on_stack[idx])
            {
                continue;
            }

            auto* slot = slot_at(b, h, static_cast<uint32_t>(idx));
            slot->refcount.store(0, std::memory_order_release);
            treiber_push(h->free_top, slot, static_cast<uint32_t>(idx));
            ++reclaimed;
        }

        return reclaimed;
    }
}
