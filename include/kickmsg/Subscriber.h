#ifndef KICKMSG_SUBSCRIBER_H
#define KICKMSG_SUBSCRIBER_H


#include <memory>

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
            Parked,  ///< A publisher will make wait_fd() readable
            Poll,    ///< Head claimed but uncommitted: no wake fires, block at most poll_budget()
        };

        /// Descriptor to poll for readability. The first call opens a private Waker on
        /// `backend`, which must be the one this channel's publishers were given; later
        /// calls must name the same one. -1 when it cannot open here, leaving the caller
        /// on receive().
        int  wait_fd(WakeBackend& backend);

        /// Share `waker` rather than opening a private one. Returns waker.fd(), or -1
        /// when it has none. It must outlive this Subscriber, and draining it is the
        /// caller's job: disarm_wait() will not drain a shared waker.
        int  attach(Waker& waker);

        /// Declare the intent to block on wait_fd(). Must be paired with
        /// disarm_wait(), and the sample is still taken with try_receive().
        Wait arm_wait();

        /// Stop asking for wakes. Drains the waker only when this Subscriber owns it; a
        /// shared one is drained by its owner, once per wait.
        void disarm_wait() { disarm_wait(true); }

        /// Opt in to a caller's wait set without handing out a descriptor: found by ADL,
        /// so a generic waiter calls `wait_descriptor(sub)` and never names kickmsg. -1
        /// until wait_fd() or attach() has opened one.
        ///
        /// Prefer this to wait_fd() wherever the waiter is generic. wait_fd() stays for a
        /// caller feeding a foreign event loop, which needs the integer itself.
        friend int wait_descriptor(Subscriber const& sub)
        {
            if (sub.waker_ == nullptr)
            {
                return -1;
            }
            return sub.waker_->fd();
        }

        /// Cap for a Wait::Poll block: the publisher's commit fires no wake.
        static constexpr nanoseconds poll_budget() { return 100us; }

        /// Non-consuming: what a caller about to block should do about this Subscriber.
        Wait peek() const;

        uint64_t lost() const { return lost_; }
        uint64_t drain_timeouts() const { return drain_timeouts_; }
        uint32_t ring_index() const { return ring_idx_; }

    private:
        // arm_wait/disarm_wait/peek/poll_budget stay public: a caller with its own event
        // loop needs them, since a publisher signals only while somebody is armed.
        friend bool wait_any(Subscriber* const* subscribers, std::size_t count,
                             nanoseconds timeout);

        /// The Waker this Subscriber is attached to, nullptr when it never asked for one.
        Waker* waker() const { return waker_; }

        /// wait_any drains every distinct Waker itself; draining here too would cost a
        /// second EAGAIN recv per private Waker per iteration.
        void disarm_wait(bool drain_owned);

        void release_ring();
        void drain_unconsumed(SubRingHeader* ring);

        /// Non-consuming peek at the next position, mirroring try_receive's
        /// "come back later" test. Parked means nothing is claimed yet.
        Wait head_state(SubRingHeader* ring) const;

        void*                base_;
        Header*              header_;
        uint32_t             ring_idx_;
        uint64_t             start_pos_;
        uint64_t             read_pos_;
        uint64_t             lost_;
        uint64_t             drain_timeouts_{0};
        std::vector<uint8_t> recv_buf_;

        // Exactly one is set: owned_waker_ also fills waker_.
        std::unique_ptr<Waker> owned_waker_;
        Waker*                 waker_{nullptr};
    };

    /// Block until at least one of `subscribers` has a sample, or `timeout` elapses.
    /// Returns true when the caller should drain, false on timeout. Like poll(), this
    /// reports readiness and consumes nothing: take the samples with try_receive().
    ///
    /// Each Subscriber must already have a descriptor (wait_fd() or attach()) and belong
    /// to the calling thread; there is no limit on how many. Give each channel its own
    /// backend, so a set spanning N channels polls N descriptors; several Subscribers on
    /// one channel may share a Waker. One without a descriptor is still waited on, but
    /// nothing can wake the set on its behalf, so the wait degrades to a re-peek.
    bool wait_any(Subscriber* const* subscribers, std::size_t count, nanoseconds timeout);
}

#endif
