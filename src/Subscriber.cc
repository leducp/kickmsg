#include <stdexcept>

#include "kickmsg/Subscriber.h"
#include "kickmsg/os/Futex.h"
#include "kickmsg/os/Process.h"
#include "kickmsg/os/Time.h"

namespace kickmsg
{
    namespace
    {
        /// How long receive() sleeps before re-checking a head that is claimed but not
        /// yet committed: no futex edge fires for the commit itself.
        constexpr nanoseconds RECHECK_NAP = 100us;
    }

    Subscriber::Subscriber(SharedRegion& region)
        : base_{region.base()}
        , header_{region.header()}
        , geom_{region.geometry()}
        , ring_idx_{UINT32_MAX}
        , start_pos_{0}
        , read_pos_{0}
        , lost_{0}
    {
        recv_buf_.resize(geom_.slot_data_size);

        for (uint32_t i = 0; i < geom_.max_subs; ++i)
        {
            auto* ring = sub_ring_at(base_, geom_, i);
            // Require Free with in_flight == 0. Resetting a live count could let
            // a late decrement underflow into the state bits.
            uint32_t expected = ring::make_packed(ring::Free);
            // Read the drain floor before Live allows publishers to advance write_pos.
            uint64_t wp = ring->write_pos.load(std::memory_order_acquire);
            if (ring->state_flight.compare_exchange_strong(expected,
                    ring::make_packed(ring::Live),
                    std::memory_order_acq_rel))
            {
                // Store starttime before releasing owner_pid, so recovery sees a matching
                // identity. Until then, pid == 0 prevents recovery.
                uint64_t pid = current_pid();
                ring->owner_starttime.store(process_starttime(pid),
                                            std::memory_order_relaxed);
                ring->owner_pid.store(pid, std::memory_order_release);
                ring_idx_  = i;
                // Keep the earlier position as the drain floor; consume from the latest
                // position to avoid replaying a previous subscriber's entries.
                uint64_t wp2 = ring->write_pos.load(std::memory_order_acquire);
                start_pos_ = wp;
                read_pos_  = wp;
                if (wp2 > wp)
                {
                    read_pos_ = wp2;
                }
                break;
            }
        }

        if (ring_idx_ == UINT32_MAX)
        {
            throw std::runtime_error("No free subscriber slots");
        }
    }

    void Subscriber::release_ring()
    {
        if (ring_idx_ == UINT32_MAX)
        {
            return;
        }

        auto* ring = sub_ring_at(base_, geom_, ring_idx_);

        // Transition Live -> Draining, preserving in_flight count.
        uint32_t old = ring->state_flight.load(std::memory_order_acquire);
        while (true)
        {
            uint32_t desired = (old & ~ring::STATE_MASK) | ring::Draining;
            if (ring->state_flight.compare_exchange_weak(old, desired,
                    std::memory_order_acq_rel, std::memory_order_acquire))
            {
                break;
            }
        }

        // Wait for all admitted publishers to finish.
        bool quiesced = true;
        microseconds deadline{geom_.commit_timeout_us};
        nanoseconds start = kickmsg::monotonic_ns();
        while (ring::get_in_flight(
                   ring->state_flight.load(std::memory_order_acquire)) > 0)
        {
            if (kickmsg::elapsed_time(start) >= deadline)
            {
                // A live publisher may still be writing. Skip draining on timeout;
                // orphan recovery can release the remaining references after quiescence.
                quiesced = false;
                ++drain_timeouts_;
                break;
            }
            kickmsg::yield();
        }

        // Still Draining, so still ours to retract. A crash inside this window leaves
        // Draining + owner == 0, which reclaim_dead_rings skips: one leaked ring.
        clear_owner(ring);

        if (quiesced)
        {
            drain_unconsumed(ring);
            // in_flight == 0 -- safe to store directly.
            ring->state_flight.store(ring::make_packed(ring::Free),
                                     std::memory_order_release);
        }
        else
        {
            // Timeout: only change state bits, preserve in_flight
            // for the slow/crashed publisher.
            old = ring->state_flight.load(std::memory_order_acquire);
            while (true)
            {
                uint32_t desired = (old & ~ring::STATE_MASK) | ring::Free;
                if (ring->state_flight.compare_exchange_weak(old, desired,
                        std::memory_order_release,
                        std::memory_order_acquire))
                {
                    break;
                }
            }
        }

        ring_idx_ = UINT32_MAX;
    }

    Subscriber::~Subscriber()
    {
        release_ring();
    }

    Subscriber::Subscriber(Subscriber&& other) noexcept
        : base_{other.base_}
        , header_{other.header_}
        , geom_{other.geom_}
        , ring_idx_{other.ring_idx_}
        , start_pos_{other.start_pos_}
        , read_pos_{other.read_pos_}
        , lost_{other.lost_}
        , drain_timeouts_{other.drain_timeouts_}
        , recv_buf_{std::move(other.recv_buf_)}
        , waker_{other.waker_}
    {
        other.ring_idx_ = UINT32_MAX;
        other.waker_    = nullptr;
    }

    Subscriber& Subscriber::operator=(Subscriber&& other) noexcept
    {
        if (this != &other)
        {
            release_ring();

            base_            = other.base_;
            header_          = other.header_;
            geom_            = other.geom_;
            ring_idx_        = other.ring_idx_;
            start_pos_       = other.start_pos_;
            read_pos_        = other.read_pos_;
            lost_            = other.lost_;
            drain_timeouts_  = other.drain_timeouts_;
            recv_buf_        = std::move(other.recv_buf_);
            waker_           = other.waker_;

            other.ring_idx_ = UINT32_MAX;
            other.waker_    = nullptr;
        }
        return *this;
    }

    bool Subscriber::attach(Waker& waker)
    {
        if (ring_idx_ == UINT32_MAX or not waker.valid())
        {
            return false;
        }
        waker_ = &waker;
        return true;
    }

    Subscriber::Wait Subscriber::head_state(SubRingHeader* ring) const
    {
        uint64_t wp = ring->write_pos.load(std::memory_order_acquire);
        if (wp <= read_pos_)
        {
            return Wait::Armed;
        }
        if (wp - read_pos_ > geom_.sub_ring_capacity)
        {
            // Overrun: try_receive resynchronises and returns a sample.
            return Wait::Ready;
        }
        auto&    e   = ring_entries(ring)[read_pos_ & geom_.sub_ring_mask];
        uint64_t seq = e.sequence.load(std::memory_order_acquire);
        // Same test try_receive gives up on: a lock at this position, or an
        // entry still holding an older generation. Everything else (commit,
        // skip marker, overwrite) it resolves without blocking.
        if (seq_is_locked(seq) or seq_pos(seq) < read_pos_ + 1)
        {
            return Wait::Poll;
        }
        return Wait::Ready;
    }

    Subscriber::Wait Subscriber::wait_state() const
    {
        if (ring_idx_ == UINT32_MAX)
        {
            return Wait::Armed;
        }
        return head_state(sub_ring_at(base_, geom_, ring_idx_));
    }

    Subscriber::Wait Subscriber::arm_wait()
    {
        if (ring_idx_ == UINT32_MAX)
        {
            return Wait::Armed;
        }
        auto* ring = sub_ring_at(base_, geom_, ring_idx_);

        // Sample before checking the head to detect a publish during waiter setup.
        // Descriptor polling does not recheck write_pos as futex_wait does.
        uint64_t cur = ring->write_pos.load(std::memory_order_relaxed);

        Wait state = head_state(ring);
        if (state != Wait::Armed)
        {
            return state;
        }
        if (waker_ == nullptr)
        {
            // No carrier: the caller's own deadline is the only wake left.
            return Wait::Armed;
        }

        ring->has_waiter.store(ring::WaiterCarrier, std::memory_order_relaxed);
        // Pairs with the publisher's seq_cst fence: orders the store above before the
        // write_pos re-read below, so a concurrent publish either sees the mode and
        // signals, or lands in the re-read.
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (ring->write_pos.load(std::memory_order_relaxed) != cur)
        {
            disarm_wait();
            return head_state(ring);
        }
        return Wait::Armed;
    }

    void Subscriber::disarm_wait()
    {
        if (ring_idx_ == UINT32_MAX)
        {
            return;
        }
        // Relaxed: a publisher reading the mode just before this can still signal,
        // leaving one stale wake for the Waker's owner to drain.
        auto* ring = sub_ring_at(base_, geom_, ring_idx_);
        ring->has_waiter.store(ring::WaiterNone, std::memory_order_relaxed);
    }

    std::optional<Subscriber::SampleRef> Subscriber::try_receive()
    {
        // Copy while the SampleView pins the slot.
        auto view = try_receive_view();
        if (not view)
        {
            return std::nullopt;
        }
        std::memcpy(recv_buf_.data(), view->data(), view->len());
        return SampleRef{recv_buf_.data(), view->len(), view->ring_pos()};
    }

    std::optional<Subscriber::SampleRef> Subscriber::receive(nanoseconds timeout)
    {
        // Moved-from Subscriber: ring_idx_ is the UINT32_MAX sentinel, so
        // sub_ring_at would compute a wild pointer.
        if (ring_idx_ == UINT32_MAX)
        {
            return std::nullopt;
        }
        auto*       ring  = sub_ring_at(base_, geom_, ring_idx_);
        nanoseconds start = kickmsg::monotonic_ns();

        int idle_spins = 0;
        while (true)
        {
            auto sample = try_receive();
            if (sample)
            {
                return sample;
            }

            nanoseconds elapsed = kickmsg::elapsed_time(start);
            if (elapsed >= timeout)
            {
                return std::nullopt;
            }
            nanoseconds remaining = timeout - elapsed;

            uint64_t cur = ring->write_pos.load(std::memory_order_relaxed);
            if (cur <= read_pos_)
            {
                idle_spins = 0;
                ring->has_waiter.store(ring::WaiterFutex, std::memory_order_relaxed);
                // Pairs with the publisher's seq_cst fence: orders this store
                // before futex_wait's kernel read of write_pos so a concurrent
                // publish can't be missed on a weakly-ordered CPU.
                std::atomic_thread_fence(std::memory_order_seq_cst);
                futex_wait(ring->write_pos, cur, remaining);
                ring->has_waiter.store(ring::WaiterNone, std::memory_order_relaxed);
            }
            else
            {
                // Head claimed but uncommitted: no futex edge fires for the
                // commit itself, so poll -- bounded, or a crashed publisher
                // turns this into a hot spin for the whole timeout.
                ++idle_spins;
                if (idle_spins <= 64)
                {
                    kickmsg::yield();
                }
                else
                {
                    nanoseconds nap = RECHECK_NAP;
                    if (remaining < nap)
                    {
                        nap = remaining;
                    }
                    kickmsg::sleep(nap);
                }
            }
        }
    }

    std::optional<Subscriber::SampleView> Subscriber::try_receive_view()
    {
        // Moved-from Subscriber: ring_idx_ is the UINT32_MAX sentinel, so
        // sub_ring_at would compute a wild pointer.
        if (ring_idx_ == UINT32_MAX)
        {
            return std::nullopt;
        }
        auto* ring = sub_ring_at(base_, geom_, ring_idx_);

        for (int retries = 0; retries < 64; ++retries)
        {
            uint64_t wp = ring->write_pos.load(std::memory_order_acquire);
            if (wp <= read_pos_)
            {
                return std::nullopt;
            }

            uint64_t capacity = geom_.sub_ring_capacity;
            if (wp - read_pos_ > capacity)
            {
                uint64_t skipped = (wp - read_pos_) - capacity;
                lost_ += skipped;
                ring->lost_count.fetch_add(skipped, std::memory_order_relaxed);
                read_pos_ = wp - capacity;
            }

            uint64_t idx  = read_pos_ & geom_.sub_ring_mask;
            auto* entries = ring_entries(ring);
            auto& e       = entries[idx];

            uint64_t seq1 = e.sequence.load(std::memory_order_acquire);
            if (seq1 != read_pos_ + 1)
            {
                if (seq_is_skip(seq1) and seq_pos(seq1) == read_pos_ + 1)
                {
                    ++lost_;
                    ring->lost_count.fetch_add(1, std::memory_order_relaxed);
                    ++read_pos_;
                    continue;
                }
                if (seq_is_locked(seq1) or seq_pos(seq1) < read_pos_ + 1)
                {
                    return std::nullopt;
                }
                ++lost_;
                ring->lost_count.fetch_add(1, std::memory_order_relaxed);
                ++read_pos_;
                continue;
            }

            uint32_t biased = meta_slot_biased(e.meta.load(std::memory_order_relaxed));
            if (biased == 0 or biased - 1 >= geom_.pool_size)
            {
                ++lost_;
                ring->lost_count.fetch_add(1, std::memory_order_relaxed);
                ++read_pos_;
                continue;
            }
            uint32_t slot_idx = biased - 1;

            // Pin the slot so it survives until ~SampleView().
            auto* slot = slot_at(base_, geom_, slot_idx);
            uint32_t rc = slot->refcount.load(std::memory_order_acquire);
            bool pinned = false;
            // rc == UINT32_MAX is corrupt residue; skip so rc + 1 can't wrap.
            while (rc > 0 and rc != UINT32_MAX)
            {
                if (slot->refcount.compare_exchange_weak(rc, rc + 1,
                        std::memory_order_acq_rel, std::memory_order_acquire))
                {
                    pinned = true;
                    break;
                }
            }

            if (not pinned)
            {
                ++lost_;
                ring->lost_count.fetch_add(1, std::memory_order_relaxed);
                ++read_pos_;
                continue;
            }

            // Seqlock validation after pinning.
            uint64_t seq2 = e.sequence.load(std::memory_order_acquire);
            if (seq2 != seq1)
            {
                uint32_t prev = slot->refcount.fetch_sub(1,
                                    std::memory_order_acq_rel);
                if (prev == 1)
                {
                    treiber_push(header_->free_top, slot, slot_idx);
                }
                ++lost_;
                ring->lost_count.fetch_add(1, std::memory_order_relaxed);
                ++read_pos_;
                continue;
            }

            // Length read under the pin, after the seqlock: see try_receive().
            uint32_t payload_len = slot->payload_len.load(std::memory_order_relaxed);
            if (payload_len > geom_.slot_data_size)
            {
                uint32_t bad = slot->refcount.fetch_sub(1, std::memory_order_acq_rel);
                if (bad == 1)
                {
                    treiber_push(header_->free_top, slot, slot_idx);
                }
                ++lost_;
                ring->lost_count.fetch_add(1, std::memory_order_relaxed);
                ++read_pos_;
                continue;
            }

            ++read_pos_;
            return SampleView{header_, slot, slot_idx, payload_len, read_pos_ - 1};
        }
        return std::nullopt;
    }

    std::optional<Subscriber::SampleView> Subscriber::receive_view(nanoseconds timeout)
    {
        // Moved-from Subscriber: ring_idx_ is the UINT32_MAX sentinel, so
        // sub_ring_at would compute a wild pointer.
        if (ring_idx_ == UINT32_MAX)
        {
            return std::nullopt;
        }
        auto*       ring  = sub_ring_at(base_, geom_, ring_idx_);
        nanoseconds start = kickmsg::monotonic_ns();

        int idle_spins = 0;
        while (true)
        {
            auto sample = try_receive_view();
            if (sample)
            {
                return sample;
            }

            nanoseconds elapsed = kickmsg::elapsed_time(start);
            if (elapsed >= timeout)
            {
                return std::nullopt;
            }
            nanoseconds remaining = timeout - elapsed;

            uint64_t cur = ring->write_pos.load(std::memory_order_relaxed);
            if (cur <= read_pos_)
            {
                idle_spins = 0;
                ring->has_waiter.store(ring::WaiterFutex, std::memory_order_relaxed);
                // Pairs with the publisher's seq_cst fence: orders this store
                // before futex_wait's kernel read of write_pos so a concurrent
                // publish can't be missed on a weakly-ordered CPU.
                std::atomic_thread_fence(std::memory_order_seq_cst);
                futex_wait(ring->write_pos, cur, remaining);
                ring->has_waiter.store(ring::WaiterNone, std::memory_order_relaxed);
            }
            else
            {
                // Head claimed but uncommitted: no futex edge fires for the
                // commit itself, so poll -- bounded, or a crashed publisher
                // turns this into a hot spin for the whole timeout.
                ++idle_spins;
                if (idle_spins <= 64)
                {
                    kickmsg::yield();
                }
                else
                {
                    nanoseconds nap = RECHECK_NAP;
                    if (remaining < nap)
                    {
                        nap = remaining;
                    }
                    kickmsg::sleep(nap);
                }
            }
        }
    }

    void Subscriber::drain_unconsumed(SubRingHeader* ring)
    {
        auto*    entries  = ring_entries(ring);
        uint64_t capacity = geom_.sub_ring_capacity;

        // write_pos is final: the in_flight spin in the destructor guarantees
        // no publisher is mid-commit on this ring.
        uint64_t wp = ring->write_pos.load(std::memory_order_acquire);

        if (wp == 0)
        {
            return;
        }

        // Only release entries this subscriber is responsible for:
        // [max(oldest, start_pos_), wp). Entries before start_pos_ belong
        // to a previous subscriber on this ring slot and were already released.
        uint64_t oldest = 0;
        if (wp > capacity)
        {
            oldest = wp - capacity;
        }
        if (oldest < start_pos_)
        {
            oldest = start_pos_;
        }

        // Release each remaining claim, including consumed and skip-marked entries.
        // SampleView pins are separate references and survive this drain.
        for (uint64_t pos = oldest; pos < wp; ++pos)
        {
            auto&    e    = entries[pos & geom_.sub_ring_mask];
            uint64_t meta = e.meta.load(std::memory_order_acquire);

            uint32_t biased = meta_slot_biased(meta);
            if (biased == 0 or biased - 1 >= geom_.pool_size)
            {
                continue;
            }
            uint32_t slot_idx = biased - 1;

            // Clear the claim before releasing its reference to prevent a second release.
            if (not e.meta.compare_exchange_strong(meta, meta & ~META_SLOT_MASK,
                    std::memory_order_acq_rel, std::memory_order_relaxed))
            {
                continue;
            }

            auto*    slot = slot_at(base_, geom_, slot_idx);
            uint32_t prev = slot->refcount.fetch_sub(1,
                                std::memory_order_acq_rel);
            if (prev == 1)
            {
                treiber_push(header_->free_top, slot, slot_idx);
            }
        }

    }
}
