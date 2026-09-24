#ifndef KICKMSG_REGION_H
#define KICKMSG_REGION_H

#include <vector>

#include "kickmsg/types.h"
#include "kickmsg/os/SharedMemory.h"

namespace kickmsg
{
    /// Ring diagnostics. Fields may be read at different times during live traffic.
    struct RingStats
    {
        uint32_t state;          ///< ring::State as a raw int (0=Free, 1=Live, 2=Draining, 3=Reclaiming)
        uint32_t in_flight;      ///< Publishers currently admitted to this ring
        uint64_t write_pos;      ///< Monotonic claim counter (rough throughput proxy)
        uint64_t dropped_count;  ///< Cumulative publisher drops on this ring
        uint64_t lost_count;     ///< Cumulative subscriber losses on this ring
    };

    /// Read-only region diagnostics, safe during live traffic.
    struct RegionStats
    {
        std::vector<RingStats> rings;   ///< One entry per subscriber-ring slot (length == max_subs)
        uint64_t total_writes;          ///< Max of write_pos across all rings: publish events observed by the channel, monotonic across subscriber churn
        uint64_t total_drops;           ///< Sum of dropped_count across all rings
        uint64_t total_losses;          ///< Sum of lost_count across all rings
        uint64_t total_steals;          ///< Stale entries stolen (self-repair + repair_locked_entries)
        uint64_t live_rings;            ///< Number of rings currently Live
        uint64_t pool_free;             ///< Approximate free-slot count (walks Treiber stack -- racy under churn)
        uint64_t pool_size;             ///< Total pool capacity (static)
    };

    /// Header metadata copied from fields set at creation.
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

        // Clear base_ and size_ in the moved-from object.
        SharedRegion(SharedRegion&& other) noexcept
            : shm_{std::move(other.shm_)}
            , name_{std::move(other.name_)}
            , base_{other.base_}
            , size_{other.size_}
            , geometry_{other.geometry_}
        {
            other.base_ = nullptr;
            other.size_ = 0;
            other.geometry_ = Geometry{};
        }

        SharedRegion& operator=(SharedRegion&& other) noexcept
        {
            if (this != &other)
            {
                shm_   = std::move(other.shm_);
                name_  = std::move(other.name_);
                base_  = other.base_;
                size_  = other.size_;
                geometry_  = other.geometry_;
                other.base_ = nullptr;
                other.size_ = 0;
                other.geometry_ = Geometry{};
            }
            return *this;
        }

        ~SharedRegion() = default;

        /// Create a fresh region under `name`, replacing any existing
        /// object: peers keep their old (orphaned) mapping, never a
        /// truncated one.  Single-creator only; concurrent creators must
        /// use create_or_open().
        static SharedRegion create(char const* name, channel::Type type,
                                   channel::Config const& cfg,
                                   char const* creator_name = "");

        /// Open an existing region. If expected_identity and the stored identity are
        /// both nonzero, they must match or this throws. Throws VersionMismatch on a
        /// region stamped by another kickmsg build.
        static SharedRegion open(char const* name, uint64_t expected_identity = 0);

        /// Create or open a region. Opening ignores cfg.schema; use try_claim_schema()
        /// to publish a descriptor. Schema is separate from geometry validation.
        /// Throws VersionMismatch at once on a region stamped by another kickmsg build.
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

        /// Validated local copy of the geometry; pointer arithmetic uses only this.
        Geometry const& geometry() const { return geometry_; }

        channel::Type channel_type() const { return header()->channel_type; }

        /// The shared-memory name this region was created or opened with.
        /// Empty for a default-constructed SharedRegion (before create/open).
        std::string const& name() const { return name_; }

        /// Read the published payload schema, or nullopt while Unset or Claiming.
        /// The caller decides whether the descriptor is compatible.
        std::optional<SchemaInfo> schema() const;

        /// Publish a schema with Unset -> Claiming -> Set. Returns true on success.
        /// If another claim is in progress, wait for a bounded number of yields,
        /// then return false. Read schema() to check the published descriptor.
        /// Safe during live traffic. A dead claimant requires reset_schema_claim().
        bool try_claim_schema(SchemaInfo const& info);

        /// Reset Claiming to Unset after confirming the claimant has stopped.
        /// Returns true if reset, false if the state was not Claiming.
        /// Unsafe during an active claim: its writer could overwrite a new claim.
        bool reset_schema_claim();

        /// Read-only health check, safe during live traffic. Probes each occupied
        /// ring's owner; intended for periodic monitoring, not the message path.
        ///
        /// Locked entries can be repaired under live traffic. Retired rings and a
        /// stuck schema claim require confirmation that their writers have stopped.
        /// Draining and schema_stuck can be transient; dead_rings identifies owners
        /// that reclaim_dead_rings() can recover.
        struct HealthReport
        {
            uint32_t locked_entries;   ///< Entries holding a position-tagged lock, or committed >1 wrap stale
            uint32_t retired_rings;    ///< Free rings with stale in_flight > 0
            uint32_t draining_rings;   ///< Draining rings with in_flight > 0
            uint32_t dead_rings;       ///< Live/Draining/Reclaiming rings whose owner process is gone
            uint32_t live_rings;       ///< Active subscriber rings
            bool     schema_stuck;     ///< schema_state at Claiming (advisory; may be a live claim)
        };
        HealthReport diagnose();

        /// Replace stalled commits with skip markers. Safe during live traffic:
        /// recheck locks after one commit_timeout and steal by CAS. A resumed
        /// publisher detects the stolen lock and drops its commit.
        /// Waits one commit_timeout if locks exist; returns entries repaired.
        std::size_t repair_locked_entries();

        /// Reset Free rings with in_flight > 0 so subscribers can reuse them.
        /// Only call after confirming the admitted publishers have stopped.
        /// Returns the number reset.
        std::size_t reset_retired_rings();

        /// Reclaim rings with a dead owner, checked by PID and start time.
        /// CAS to Reclaiming, recheck death, then free or restore the ring.
        /// Concurrent changes defer a ring to the next call. Preserve in_flight
        /// for late publisher decrements; remaining counts require reset_retired_rings().
        /// Undrained entry claims keep their slots until the next publisher at each entry.
        /// Returns the number reclaimed.
        ///
        /// A crash before the owner PID is recorded can strand a ring. A later
        /// call or owner teardown can recover a ring left at Reclaiming.
        std::size_t reclaim_dead_rings();

        /// Read ring counters and an approximate free-slot count under live traffic.
        /// Uses no locks or syscalls. Fields may be sampled at different times;
        /// the free-stack walk is bounded by pool_size.
        RegionStats stats() const;

        /// Static header snapshot -- geometry + creator metadata.  All
        /// fields are written once at creation, so this is a plain copy.
        RegionInfo info() const;

        /// Reclaim off-stack slots with no ring claim, and reset every other off-stack
        /// slot's refcount to its claim count.
        /// Only call with all publishers stopped and no outstanding SampleView: both
        /// can hold slots without ring entries. Returns the number reclaimed.
        std::size_t reclaim_orphaned_slots();

    private:
        /// Initialize the mapped region and publish MAGIC last.
        void stamp_new_region(channel::Type type, channel::Config const& cfg,
                              char const* creator_name, std::size_t total_size,
                              std::size_t sub_rings_offset, std::size_t pool_offset,
                              std::size_t ring_stride,     std::size_t slot_stride,
                              uint16_t    creator_len);

        SharedMemory shm_;
        std::string  name_;
        void*        base_{nullptr};
        std::size_t  size_{0};
        Geometry     geometry_{};
    };
}

#endif
