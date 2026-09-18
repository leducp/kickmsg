#ifndef KICKMSG_PUBLISHER_H
#define KICKMSG_PUBLISHER_H

#include "kickmsg/types.h"
#include "kickmsg/Region.h"
#include "kickmsg/Waker.h"

namespace kickmsg
{
    /// Writable shared-memory reservation. data == nullptr means pool exhaustion.
    struct Allocation
    {
        void*       data;
        std::size_t max_size;
    };

    class Publisher
    {
    public:
        Publisher(SharedRegion& region, WakeBackend* backend = nullptr)
            : base_{region.base()}
            , header_{region.header()}
            , geom_{region.geometry()}
            , commit_timeout_{microseconds{geom_.commit_timeout_us}}
            , pending_slot_{INVALID_SLOT}
            , wake_backend_{backend}
        {
        }

        ~Publisher();

        Publisher(Publisher const&) = delete;
        Publisher& operator=(Publisher const&) = delete;

        Publisher(Publisher&& other) noexcept
            : base_{other.base_}
            , header_{other.header_}
            , geom_{other.geom_}
            , commit_timeout_{other.commit_timeout_}
            , pending_slot_{other.pending_slot_}
            , reservation_{other.reservation_}
            , dropped_{other.dropped_}
            , wake_backend_{other.wake_backend_}
        {
            other.pending_slot_ = INVALID_SLOT;
        }

        Publisher& operator=(Publisher&& other) noexcept
        {
            if (this != &other)
            {
                release_pending();
                base_           = other.base_;
                header_         = other.header_;
                geom_           = other.geom_;
                commit_timeout_ = other.commit_timeout_;
                pending_slot_   = other.pending_slot_;
                reservation_    = other.reservation_;
                dropped_        = other.dropped_;
                wake_backend_   = other.wake_backend_;
                other.pending_slot_ = INVALID_SLOT;
            }
            return *this;
        }

        /// Reserve a slot; data is nullptr if the pool is exhausted.
        /// Invalidates any previous reservation and returns its slot to the pool.
        Allocation allocate();

        /// Current reservation token, or 0 if none. Changes on allocate() and publish().
        uint64_t reservation_id() const
        {
            if (pending_slot_ == INVALID_SLOT)
            {
                return 0;
            }
            return reservation_;
        }

        /// Publish len bytes from the current reservation. Returns rings delivered to.
        /// Returns 0 for no reservation, oversized len, or no live subscribers.
        /// An oversized len releases the reservation.
        std::size_t publish(std::size_t len);

        /// Allocate, copy, and publish. Returns bytes written, even with no subscribers,
        /// -EMSGSIZE if too large, or -EAGAIN if the pool is exhausted.
        int32_t send(void const* data, std::size_t len);

        /// Number of per-ring delivery drops (CAS lock contention or pool exhaustion).
        uint64_t dropped() const { return dropped_; }

    private:
        /// stable_lock means one lock value persisted for the full timeout,
        /// allowing recovery to steal that position.
        struct CommitWait
        {
            uint64_t last_seq;
            bool     stable_lock;
        };

        static CommitWait wait_for_commit(Entry& e, uint64_t expected_seq,
                                          microseconds timeout);
        void self_repair(Entry& e, uint64_t pos, uint64_t capacity,
                         CommitWait const& wait);
        /// True when the ring wants a backend wake for the skip marker.
        bool abandon_delivery(SubRingHeader* ring);
        void release_slot(uint32_t idx);
        void release_pending();

        /// futex-wakes `ring`; true when it wants the backend instead, ORed across the
        /// ring loop so a publish signals it once.
        bool wake_ring(SubRingHeader* ring);

        void*        base_;
        /// Shared mutable state only (free_top, counters).  Anything that
        /// drives pointer math comes from geom_; see Geometry.
        Header*      header_;
        Geometry     geom_;
        microseconds commit_timeout_;
        uint32_t     pending_slot_;
        /// Monotonic reservation counter; see reservation_id().  Never reset,
        /// so a stale token can never alias a later reservation.
        uint64_t     reservation_{0};
        uint64_t     dropped_{0};
        WakeBackend* wake_backend_{nullptr};
    };
}

#endif
