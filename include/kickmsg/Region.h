#ifndef KICKMSG_REGION_H
#define KICKMSG_REGION_H

#include <vector>

#include "kickmsg/types.h"
#include "kickmsg/os/SharedMemory.h"

namespace kickmsg
{
    /// Runtime snapshot of a single subscriber ring.
    /// Values are relaxed/acquire-loaded, so the snapshot is internally
    /// consistent per-ring but may race mildly across rings — fine for a
    /// diagnostic view; not intended as a strongly-consistent read.
    struct RingStats
    {
        uint32_t state;          ///< ring::State as a raw int (0=Free, 1=Live, 2=Draining)
        uint32_t in_flight;      ///< Publishers currently admitted to this ring
        uint64_t write_pos;      ///< Monotonic claim counter (rough throughput proxy)
        uint64_t dropped_count;  ///< Cumulative publisher drops on this ring
        uint64_t lost_count;     ///< Cumulative subscriber losses on this ring
    };

    /// Aggregate region snapshot returned by SharedRegion::stats().
    /// Safe to call under live traffic: all reads are relaxed/acquire,
    /// no writes.
    struct RegionStats
    {
        std::vector<RingStats> rings;   ///< One entry per subscriber-ring slot (length == max_subs)
        uint64_t total_writes;          ///< Max of write_pos across all rings: publish events observed by the channel, monotonic across subscriber churn
        uint64_t total_drops;           ///< Sum of dropped_count across all rings
        uint64_t total_losses;          ///< Sum of lost_count across all rings
        uint64_t live_rings;            ///< Number of rings currently Live
        uint64_t pool_free;             ///< Approximate free-slot count (walks Treiber stack — racy under churn)
        uint64_t pool_size;             ///< Total pool capacity (static)
    };

    /// Static header metadata returned by SharedRegion::info().
    /// All fields are written once at creation and never mutated, so this
    /// read is a plain copy of stable bytes.
    struct RegionInfo
    {
        std::string   shm_name;
        channel::Type channel_type;
        uint32_t      version;
        uint64_t      config_hash;
        uint64_t      total_size;
        uint64_t      max_subs;
        uint64_t      sub_ring_capacity;
        uint64_t      pool_size;
        uint64_t      max_payload_size;
        uint64_t      commit_timeout_us;
        uint64_t      creator_pid;
        uint64_t      created_at_ns;
        std::string   creator_name;
    };


    class SharedRegion
    {
    public:
        SharedRegion() = default;

        SharedRegion(SharedRegion const&) = delete;
        SharedRegion& operator=(SharedRegion const&) = delete;

        // Hand-written move ops so the moved-from object's base_/size_
        // are reset to a default-constructed state.  A defaulted move
        // would leave them aliasing the destination's live memory —
        // base() on the moved-from object would silently return a
        // dangling-looking-live pointer instead of nullptr.
        SharedRegion(SharedRegion&& other) noexcept
            : shm_{std::move(other.shm_)}
            , name_{std::move(other.name_)}
            , base_{other.base_}
            , size_{other.size_}
        {
            other.base_ = nullptr;
            other.size_ = 0;
        }

        SharedRegion& operator=(SharedRegion&& other) noexcept
        {
            if (this != &other)
            {
                shm_   = std::move(other.shm_);
                name_  = std::move(other.name_);
                base_  = other.base_;
                size_  = other.size_;
                other.base_ = nullptr;
                other.size_ = 0;
            }
            return *this;
        }

        ~SharedRegion() = default;

        static SharedRegion create(char const* name, channel::Type type,
                                   channel::Config const& cfg,
                                   char const* creator_name = "");

        static SharedRegion open(char const* name);

        /// Create the region if it doesn't exist, otherwise open the
        /// existing one.  On the open branch, cfg.schema is IGNORED —
        /// schema is orthogonal to channel geometry and doesn't
        /// participate in the config-hash mismatch check.  Use
        /// try_claim_schema() afterwards to publish a descriptor
        /// regardless of which side ended up creating the region.
        static SharedRegion create_or_open(char const* name, channel::Type type,
                                           channel::Config const& cfg,
                                           char const* creator_name = "");

        /// Number of bytes the caller must provide to back a region with
        /// this config and creator name.  The address passed to
        /// attach_create() must be at least CACHE_LINE aligned and span
        /// at least this many bytes.
        static std::size_t required_size(channel::Config const& cfg,
                                         char const* creator_name = "");

        /// Stamp a fresh region into caller-provided memory.  The library
        /// does not take ownership: the caller's buffer must outlive the
        /// returned SharedRegion and any Publisher/Subscriber attached to
        /// it.  unlink() is a no-op on the returned region.  `label`, if
        /// non-empty, is surfaced via info().shm_name for logging.
        ///
        /// Throws if address is not CACHE_LINE aligned or size is less
        /// than required_size(cfg, creator_name).
        static SharedRegion attach_create(void* address, std::size_t size,
                                          channel::Type type,
                                          channel::Config const& cfg,
                                          char const* creator_name = "",
                                          char const* label = "");

        /// Attach to caller-provided memory that already contains a valid
        /// region (validates MAGIC + VERSION, and that size is at least
        /// the embedded total_size).  No ownership taken; unlink() is a
        /// no-op.  `label` is surfaced via info().shm_name for logging.
        ///
        /// Throws if address is not CACHE_LINE aligned, magic/version do
        /// not match, or size is smaller than the embedded total_size.
        static SharedRegion attach_open(void* address, std::size_t size,
                                        char const* label = "");

        void unlink();

        void*       base()       { return base_; }
        void const* base() const { return base_; }

        Header*       header()       { return static_cast<Header*>(base_); }
        Header const* header() const { return static_cast<Header const*>(base_); }

        channel::Type channel_type() const { return header()->channel_type; }

        /// The shared-memory name this region was created or opened with.
        /// Empty for a default-constructed SharedRegion (before create/open).
        std::string const& name() const { return name_; }

        /// Read the payload schema descriptor if one has been published.
        ///
        /// Returns nullopt when the schema slot is still Unset, or while a
        /// concurrent claim is mid-write (Claiming).  The library never
        /// interprets the bytes: callers apply their own mismatch policy
        /// against the returned SchemaInfo (identity / layout / version /
        /// name / algo tags).
        std::optional<SchemaInfo> schema() const;

        /// Atomically publish a schema descriptor to the region.
        ///
        /// Returns true if this call claimed the slot (Unset → Claiming →
        /// Set), false if some other claimant got there first — in which
        /// case the caller should read back with schema() and apply its
        /// own mismatch policy.  When another claim is mid-write, this
        /// call briefly yields until the state settles or a small bounded
        /// budget is exhausted; if the state is still Claiming at that
        /// point (likely a crashed claimant), this call still returns
        /// false and the operator should use reset_schema_claim() to
        /// recover the wedged slot.
        ///
        /// Safe under live traffic and across processes; only reachable
        /// at connect-time scale (not on the hot path).
        bool try_claim_schema(SchemaInfo const& info);

        /// Recover a schema slot wedged in the Claiming state by a
        /// crashed claimant (CAS'd Unset → Claiming then died before the
        /// release-store of Set).  Atomically CASes Claiming → Unset so a
        /// new claim can proceed; returns true if the reset actually
        /// happened, false if the state was not Claiming.
        ///
        /// NOT safe under live traffic.  Only call after confirming the
        /// crashed claimant is gone: a slow-but-alive writer could still
        /// be mid-memcpy into schema_data and would then release-store
        /// Set, racing a new claim into torn bytes.  Mirrors the safety
        /// contract of reset_retired_rings() — a deliberate post-crash
        /// action, not a routine maintenance call.
        bool reset_schema_claim();

        /// Read-only health check. Safe under live traffic; does NOT mutate
        /// the region. Counts locked entries and ring states, and probes
        /// per-ring owner liveness (a bounded number of cheap OS calls, one
        /// per occupied ring -- intended for a periodic health timer, not a
        /// hot path).
        ///
        /// Supervisor policy:
        ///  - locked_entries > 0: crash residue, call repair_locked_entries()
        ///  - retired_rings > 0: safe for reset_retired_rings() after
        ///    confirming the crashed publisher is gone
        ///  - draining_rings > 0: usually transient (subscriber tearing down),
        ///    persistent counts may indicate a stuck teardown
        ///  - dead_rings > 0: a subscriber holding a Live/Draining ring died;
        ///    call reclaim_dead_rings() to recover the ring slot
        ///  - live_rings: normal occupancy
        ///  - schema_stuck: a claimant is in the Claiming state. This is a
        ///    point-in-time read, so a healthy in-progress try_claim_schema()
        ///    can transiently set it -- treat it as advisory and act
        ///    (reset_schema_claim()) only if it persists AND the claimant is
        ///    confirmed gone.
        struct HealthReport
        {
            uint32_t locked_entries;   ///< Entries stuck at LOCKED_SEQUENCE
            uint32_t retired_rings;    ///< Free rings with stale in_flight > 0
            uint32_t draining_rings;   ///< Draining rings with in_flight > 0
            uint32_t dead_rings;       ///< Live/Draining rings whose owner process is gone
            uint32_t live_rings;       ///< Active subscriber rings
            bool     schema_stuck;     ///< schema_state at Claiming (advisory; may be a live claim)
        };
        HealthReport diagnose();

        /// Repair ring entries stuck at LOCKED_SEQUENCE (publisher crashed
        /// mid-commit). Commits the entry with INVALID_SLOT so future
        /// publishers can wrap past it.
        ///
        /// Safe to call under live traffic: the worst outcome is a benign
        /// double-store if a slow (but alive) publisher commits at the same
        /// time. Can be called freely on a health-check timer.
        /// Returns the number of entries repaired.
        std::size_t repair_locked_entries();

        /// Reset retired rings (Free | in_flight>0) so new subscribers can
        /// claim them. These rings were left stuck by a subscriber teardown
        /// that timed out on a crashed publisher's in_flight.
        ///
        /// Only safe after confirming the crashed publisher is gone.
        /// Unlike repair_locked_entries(), this is a deliberate post-crash
        /// action, not a routine maintenance call.
        /// Returns the number of rings reset.
        std::size_t reset_retired_rings();

        /// Reclaim rings whose owning subscriber process has died (a Live or
        /// Draining ring left behind by a crash). The owner pid + start time
        /// recorded at claim time are checked against the OS; only rings with
        /// a provably-dead owner are reclaimed, so this is safe under live
        /// traffic -- a slow-but-alive subscriber is never touched. in_flight
        /// is preserved (a mid-commit publisher must still fetch_sub), so a
        /// reclaimed ring may land in the retired state for reset_retired_rings()
        /// to finish; committed slot refs are recovered by
        /// reclaim_orphaned_slots(). Returns the number of rings reclaimed.
        ///
        /// Residual: a subscriber that crashes in the few instructions between
        /// winning the claim CAS and recording its pid leaves owner_pid == 0,
        /// which this cannot attribute and so will not reclaim.
        std::size_t reclaim_dead_rings();

        /// Runtime counter snapshot — safe under live traffic.
        ///
        /// Reads the cross-process per-ring counters (`write_pos`,
        /// `dropped_count`, `lost_count`) plus ring state and an approximate
        /// pool-free count.  Intended for external monitoring and the CLI's
        /// `stats` / `watch` subcommands.
        ///
        /// Cheap (no syscalls, no locks, a handful of atomic loads) but not a
        /// strongly-consistent view: individual per-ring values are consistent
        /// with themselves (sequential loads on one variable), but different
        /// rings may be read at slightly different instants.  The free-stack
        /// walk for `pool_free` is bounded by `pool_size` so it can't loop
        /// forever under racing pushes/pops.
        RegionStats stats() const;

        /// Static header snapshot — geometry + creator metadata.  All
        /// fields are written once at creation, so this is a plain copy.
        RegionInfo info() const;

        /// Reclaim orphaned slots (refcount > 0 but not referenced by any ring entry).
        /// These are caused by publisher crashes between allocate and publish, or by
        /// skipped drain on subscriber teardown timeout.
        ///
        /// NOT safe under live traffic. Call only when:
        ///  - all publishers are quiesced (a publisher between refcount pre-set
        ///    and ring push has rc > 0 with no ring entry yet), AND
        ///  - no outstanding SampleView exists (a view holds a refcount pin on
        ///    its slot without any ring entry reference; reclaiming it would free
        ///    memory still being read).
        /// Returns the number of slots reclaimed.
        std::size_t reclaim_orphaned_slots();

    private:
        /// Stamp channel geometry, creator metadata, optional schema, and
        /// finally MAGIC into an already-mapped region.  Shared between
        /// create() and create_or_open()'s creator branch so the two paths
        /// never diverge on layout or ordering.
        void stamp_new_region(channel::Type type, channel::Config const& cfg,
                              char const* creator_name, std::size_t total_size,
                              std::size_t sub_rings_offset, std::size_t pool_offset,
                              std::size_t ring_stride,     std::size_t slot_stride,
                              uint16_t    creator_len);

        SharedMemory shm_;
        std::string  name_;
        void*        base_{nullptr};
        std::size_t  size_{0};
    };
}

#endif
