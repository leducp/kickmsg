#ifndef KICKMSG_PUBLISHER_H
#define KICKMSG_PUBLISHER_H

#include <algorithm>

#include "kickmsg/types.h"
#include "kickmsg/Region.h"
#include "kickmsg/Waker.h"

namespace kickmsg
{
    class Publisher;

    /// A reserved pool slot.  Destroying an unpublished one returns the slot
    /// to the pool.  Must not outlive its Publisher: nothing tracks that lifetime.
    class AllocatedSlot
    {
    public:
        AllocatedSlot() = default;
        ~AllocatedSlot();

        AllocatedSlot(AllocatedSlot&& other) noexcept;
        AllocatedSlot& operator=(AllocatedSlot&& other) noexcept;
        AllocatedSlot(AllocatedSlot const&) = delete;
        AllocatedSlot& operator=(AllocatedSlot const&) = delete;

        /// False once published, or once a later allocate() took the slot back.
        bool valid() const;
        bool published() const { return published_; }

        /// Writable payload area of max_size() bytes, for filling the slot in place.
        /// The pointer dies with the reservation and nothing can revoke a copy of it.
        void*       data()     const { return data_; }
        std::size_t max_size() const { return max_size_; }

        /// Copy `len` bytes in.  Returns bytes written, 0 if the reservation is gone
        /// or `len` exceeds max_size().
        std::size_t write(void const* src, std::size_t len);

        /// Commit the first `len` bytes.  Returns rings delivered to; 0 also means a
        /// gone reservation, an oversized length, or no subscribers.
        std::size_t publish(std::size_t len);

    private:
        friend class Publisher;
        AllocatedSlot(Publisher& publisher, void* data, std::size_t max_size, uint64_t id)
            : publisher_{&publisher}, data_{data}, max_size_{max_size}, id_{id}
        {
        }

        Publisher*  publisher_{nullptr};
        void*       data_{nullptr};
        std::size_t max_size_{0};
        uint64_t   id_{0};
        bool       published_{false};
    };

    class Publisher
    {
    public:
        Publisher(SharedRegion& region, WakeBackend* backend = nullptr)
            : base_{region.base()}
            , header_{region.header()}
            , geometry_{region.geometry()}
            , commit_timeout_{microseconds{geometry_.commit_timeout_us}}
            , pending_slot_{INVALID_SLOT}
            , wake_backend_{backend}
        {
        }

        ~Publisher();

        Publisher(Publisher const&) = delete;
        Publisher& operator=(Publisher const&) = delete;

        /// Moving invalidates every outstanding AllocatedSlot of the source and
        /// returns its reserved slot to the pool.
        Publisher(Publisher&& other) noexcept
            : base_{other.base_}
            , header_{other.header_}
            , geometry_{other.geometry_}
            , commit_timeout_{other.commit_timeout_}
            , pending_slot_{INVALID_SLOT}
            , reservation_{other.reservation_}
            , dropped_{other.dropped_}
            , wake_backend_{other.wake_backend_}
        {
            // No handle can reach the slot once its id is invalidated, so free it.
            other.release_pending();
            // Advance, never reset: a reused moved-from publisher must not reissue old ids.
            ++other.reservation_;
        }

        Publisher& operator=(Publisher&& other) noexcept
        {
            if (this != &other)
            {
                release_pending();
                base_           = other.base_;
                header_         = other.header_;
                geometry_       = other.geometry_;
                commit_timeout_ = other.commit_timeout_;
                pending_slot_   = INVALID_SLOT;
                // Past both counters, so neither object's older handles can match.
                reservation_    = std::max(reservation_, other.reservation_) + 1;
                dropped_        = other.dropped_;
                wake_backend_   = other.wake_backend_;
                other.release_pending();
                ++other.reservation_;
            }
            return *this;
        }

        /// Reserve a slot.  The result is invalid if the pool is exhausted.
        /// Supersedes any previous reservation and returns its slot to the pool.
        AllocatedSlot allocate();

        /// Allocate, copy, and publish. Returns bytes written, even with no subscribers,
        /// -EMSGSIZE if too large, or -EAGAIN if the pool is exhausted.
        int32_t send(void const* data, std::size_t len);

        /// Number of per-ring delivery drops (CAS lock contention or pool exhaustion).
        uint64_t dropped() const { return dropped_; }

    private:
        friend class AllocatedSlot;

        /// Commit `len` bytes of the pending reservation.
        std::size_t publish(std::size_t len);

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
        /// Shared mutable state only; pointer math uses geometry_.
        Header*      header_;
        Geometry     geometry_;
        microseconds commit_timeout_;
        uint32_t     pending_slot_;
        /// Id of the current reservation; only ever increases, so a stale
        /// AllocatedSlot id never matches again.
        uint64_t     reservation_{0};
        uint64_t     dropped_{0};
        WakeBackend* wake_backend_{nullptr};
    };
}

#endif
