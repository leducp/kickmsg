#ifndef KICKMSG_SUBSCRIBER_H
#define KICKMSG_SUBSCRIBER_H


#include "kickmsg/types.h"
#include "kickmsg/Region.h"
#include "kickmsg/Waker.h"

namespace kickmsg
{
    class Subscriber
    {
    public:
        // Copy-based sample: data is copied into subscriber-local memory.
        // Move-only: the internal buffer is reused across try_receive()
        // calls, so copies would alias the same memory.
        class SampleRef
        {
        public:
            SampleRef(void const* data, std::size_t len, uint64_t ring_pos)
                : data_{data}
                , len_{len}
                , ring_pos_{ring_pos}
            {
            }

            ~SampleRef() = default;

            SampleRef(SampleRef const&) = delete;
            SampleRef& operator=(SampleRef const&) = delete;

            SampleRef(SampleRef&& other) noexcept
                : data_{other.data_}
                , len_{other.len_}
                , ring_pos_{other.ring_pos_}
            {
                other.data_ = nullptr;
                other.len_  = 0;
            }

            SampleRef& operator=(SampleRef&& other) noexcept
            {
                if (this != &other)
                {
                    data_     = other.data_;
                    len_      = other.len_;
                    ring_pos_ = other.ring_pos_;
                    other.data_ = nullptr;
                    other.len_  = 0;
                }
                return *this;
            }

            void const* data()     const { return data_; }
            std::size_t len()      const { return len_; }
            uint64_t    ring_pos() const { return ring_pos_; }

        private:
            void const* data_;
            std::size_t len_;
            uint64_t    ring_pos_;
        };

        // Zero-copy sample: data points directly into shared memory.
        // Holds a refcount pin on the slot, released on destruction.
        // Must not outlive the SharedRegion.
        class SampleView
        {
        public:
            SampleView()
                : base_{nullptr}
                , header_{nullptr}
                , slot_idx_{INVALID_SLOT}
                , len_{0}
                , ring_pos_{0}
            {
            }

            ~SampleView() { release(); }

            SampleView(SampleView const&) = delete;
            SampleView& operator=(SampleView const&) = delete;

            SampleView(SampleView&& other) noexcept
                : base_{other.base_}
                , header_{other.header_}
                , slot_idx_{other.slot_idx_}
                , len_{other.len_}
                , ring_pos_{other.ring_pos_}
            {
                other.slot_idx_ = INVALID_SLOT;
            }

            SampleView& operator=(SampleView&& other) noexcept
            {
                if (this != &other)
                {
                    release();
                    base_     = other.base_;
                    header_   = other.header_;
                    slot_idx_ = other.slot_idx_;
                    len_      = other.len_;
                    ring_pos_ = other.ring_pos_;
                    other.slot_idx_ = INVALID_SLOT;
                }
                return *this;
            }

            void const* data() const
            {
                if (slot_idx_ == INVALID_SLOT)
                {
                    return nullptr;
                }
                return slot_data(slot_at(base_, header_, slot_idx_));
            }

            std::size_t len()      const { return len_; }
            uint64_t    ring_pos() const { return ring_pos_; }
            bool valid()           const { return slot_idx_ != INVALID_SLOT; }

        private:
            friend class Subscriber;

            SampleView(void* base, Header* hdr, uint32_t slot_idx, uint32_t len, uint64_t ring_pos)
                : base_{base}
                , header_{hdr}
                , slot_idx_{slot_idx}
                , len_{len}
                , ring_pos_{ring_pos}
            {
            }

            void release()
            {
                if (slot_idx_ != INVALID_SLOT)
                {
                    auto* slot = slot_at(base_, header_, slot_idx_);
                    auto  prev = slot->refcount.fetch_sub(1,
                                     std::memory_order_acq_rel);
                    if (prev == 1)
                    {
                        treiber_push(header_->free_top, slot, slot_idx_);
                    }
                    slot_idx_ = INVALID_SLOT;
                }
            }

            void*    base_;
            Header*  header_;
            uint32_t slot_idx_;
            uint32_t len_;
            uint64_t ring_pos_;
        };

        Subscriber(SharedRegion& region);
        ~Subscriber();

        Subscriber(Subscriber const&) = delete;
        Subscriber& operator=(Subscriber const&) = delete;

        Subscriber(Subscriber&& other) noexcept;
        Subscriber& operator=(Subscriber&& other) noexcept;

        std::optional<SampleRef> try_receive();
        std::optional<SampleRef> receive(nanoseconds timeout);
        std::optional<SampleView> try_receive_view();
        std::optional<SampleView> receive_view(nanoseconds timeout);

        /// What the caller may do after arm_wait().
        enum class Wait
        {
            Ready,   ///< A sample is waiting: drain instead of blocking
            Armed,   ///< A publisher will make the descriptor readable
            Poll,    ///< Head claimed but uncommitted: no wake fires for it, so re-check soon rather than block
        };

        /// Wait on `waker`, built on the backend this channel's publishers were given.
        /// It must outlive this Subscriber, and draining it is the caller's job: several
        /// Subscribers may share one, so disarm_wait() cannot drain it without swallowing
        /// another's wake. False when it has no descriptor.
        bool attach(Waker& waker);

        /// Declare the intent to block on the descriptor. Must be paired with
        /// disarm_wait(), and the sample is still taken with try_receive().
        Wait arm_wait();

        /// Stop asking for wakes. Never drains: the waker's owner does that, once per
        /// wait, or a Subscriber sharing it would swallow another's wake.
        void disarm_wait();

        /// Opt in to a caller's wait set without handing out a descriptor: found by ADL,
        /// so a generic waiter calls `wait_descriptor(sub)` and never names kickmsg. -1
        /// until attach() has opened one. A caller feeding its own event loop takes the
        /// descriptor from here too, which is the only way out.
        friend int wait_descriptor(Subscriber const& sub)
        {
            if (sub.waker_ == nullptr)
            {
                return -1;
            }
            return wait_descriptor(*sub.waker_);
        }

        /// What arm_wait() would answer, without arming. Consumes nothing.
        Wait wait_state() const;

        uint64_t lost() const { return lost_; }
        uint64_t drain_timeouts() const { return drain_timeouts_; }
        uint32_t ring_index() const { return ring_idx_; }

    private:
        // arm_wait/disarm_wait/wait_state stay public: a caller with its own event
        // loop needs them, since a publisher signals only while somebody is armed.
        friend bool wait_any(Subscriber* const* subscribers, std::size_t count,
                             nanoseconds timeout, nanoseconds poll_cap);

        void release_ring();
        void drain_unconsumed(SubRingHeader* ring);

        /// Non-consuming peek at the next position, mirroring try_receive's
        /// "come back later" test. Armed means nothing is claimed yet.
        Wait head_state(SubRingHeader* ring) const;

        void*                base_;
        Header*              header_;
        uint32_t             ring_idx_;
        uint64_t             start_pos_;
        uint64_t             read_pos_;
        uint64_t             lost_;
        uint64_t             drain_timeouts_{0};
        std::vector<uint8_t> recv_buf_;

        Waker* waker_{nullptr};
    };

    /// Wait until one of `subscribers` has a sample, or `timeout` runs out. Returns true
    /// when there is something to read; it reads nothing itself, so drain with
    /// try_receive().
    ///
    /// All the subscribers must belong to the calling thread. Some cycles cannot be woken
    /// at all: `poll_cap` is how long those wait before looking again.
    bool wait_any(Subscriber* const* subscribers, std::size_t count, nanoseconds timeout,
                  nanoseconds poll_cap = 100us);
}

#endif
