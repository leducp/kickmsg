#ifndef KICKMSG_PUBLISHER_H
#define KICKMSG_PUBLISHER_H

#include "kickmsg/types.h"
#include "kickmsg/Region.h"

namespace kickmsg
{
    /// Reservation returned by Publisher::allocate(): a writable pointer
    /// into shared memory plus the maximum number of bytes the caller may
    /// write through it.  data == nullptr signals pool exhaustion.
    struct Allocation
    {
        void*       data;
        std::size_t max_size;
    };

    class Publisher
    {
    public:
        Publisher(SharedRegion& region)
            : base_{region.base()}
            , header_{region.header()}
            , commit_timeout_{microseconds{header_->commit_timeout_us}}
            , pending_slot_{INVALID_SLOT}
        {
        }

        ~Publisher();

        Publisher(Publisher const&) = delete;
        Publisher& operator=(Publisher const&) = delete;

        Publisher(Publisher&& other) noexcept
            : base_{other.base_}
            , header_{other.header_}
            , commit_timeout_{other.commit_timeout_}
            , pending_slot_{other.pending_slot_}
            , dropped_{other.dropped_}
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
                commit_timeout_ = other.commit_timeout_;
                pending_slot_   = other.pending_slot_;
                dropped_        = other.dropped_;
                other.pending_slot_ = INVALID_SLOT;
            }
            return *this;
        }

        /// Reserve a slot.  Returns {data, max_size}; data is nullptr if
        /// the pool is exhausted.
        Allocation allocate();

        /// Commit the currently reserved slot, recording `len` as the
        /// payload size.
        ///
        /// Returns the number of rings delivered to.  0 means no pending
        /// allocation, oversized `len` (the pending slot is recycled), or
        /// zero live subscribers -- indistinguishable by design.
        std::size_t publish(std::size_t len);

        /// Allocate, copy, and publish in one call.
        /// Returns bytes written on success (NOT a delivery count: a
        /// successful send may have reached zero subscribers), -EMSGSIZE
        /// if too large, -EAGAIN if pool exhausted.
        int32_t send(void const* data, std::size_t len);

        /// Number of per-ring delivery drops (CAS lock contention or pool exhaustion).
        uint64_t dropped() const { return dropped_; }

    private:
        /// Result of waiting for the previous wrap's occupant to commit.
        /// stable_lock: one lock value spanned the whole timeout window,
        /// proving its (unique) holder stale -- the steal precondition.
        struct CommitWait
        {
            uint64_t last_seq;
            bool     stable_lock;
        };

        static CommitWait wait_for_commit(Entry& e, uint64_t expected_seq,
                                          microseconds timeout);
        void self_repair(Entry& e, uint64_t pos, uint64_t capacity,
                         CommitWait const& wait);
        void abandon_delivery(SubRingHeader* ring);
        void release_slot(uint32_t idx);
        void release_pending();

        void*        base_;
        Header*      header_;
        microseconds commit_timeout_;
        uint32_t     pending_slot_;
        uint64_t     dropped_{0};
    };
}

#endif
