#ifndef KICKMSG_REGION_H
#define KICKMSG_REGION_H

#include <optional>
#include <string>

#include "kickmsg/types.h"
#include "kickmsg/os/SharedMemory.h"

namespace kickmsg
{
    class SharedRegion
    {
    public:
        SharedRegion() = default;

        SharedRegion(SharedRegion const&) = delete;
        SharedRegion& operator=(SharedRegion const&) = delete;
        SharedRegion(SharedRegion&&) noexcept = default;
        SharedRegion& operator=(SharedRegion&&) noexcept = default;
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

        void unlink();

        void*       base()       { return shm_.address(); }
        void const* base() const { return shm_.address(); }

        Header*       header()       { return static_cast<Header*>(shm_.address()); }
        Header const* header() const { return static_cast<Header const*>(shm_.address()); }

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

        /// Lightweight read-only health check. Safe under live traffic.
        /// Counts locked entries and ring states without any writes.
        ///
        /// Supervisor policy:
        ///  - locked_entries > 0: crash residue, call repair_locked_entries()
        ///  - retired_rings > 0: safe for reset_retired_rings() after
        ///    confirming the crashed publisher is gone
        ///  - draining_rings > 0: usually transient (subscriber tearing down),
        ///    persistent counts may indicate a stuck teardown
        ///  - live_rings: normal occupancy
        ///  - schema_stuck: a claimant crashed between Unset → Claiming and
        ///    the Set release-store.  Every future try_claim_schema() will
        ///    return false after its bounded wait until an operator calls
        ///    reset_schema_claim() (only safe after confirming the original
        ///    claimant is gone).
        struct HealthReport
        {
            uint32_t locked_entries;   ///< Entries stuck at LOCKED_SEQUENCE
            uint32_t retired_rings;    ///< Free rings with stale in_flight > 0
            uint32_t draining_rings;   ///< Draining rings with in_flight > 0
            uint32_t live_rings;       ///< Active subscriber rings
            bool     schema_stuck;     ///< schema_state wedged at Claiming (crashed claimant)
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
        SharedMemory shm_;
        std::string  name_;
    };
}

#endif
