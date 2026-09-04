#include <stdexcept>

#include "kickmsg/Subscriber.h"
#include "kickmsg/os/Futex.h"
#include "kickmsg/os/Process.h"
#include "kickmsg/os/Time.h"

namespace kickmsg
{
    Subscriber::Subscriber(SharedRegion& region)
        : base_{region.base()}
        , header_{region.header()}
        , ring_idx_{UINT32_MAX}
        , start_pos_{0}
        , read_pos_{0}
        , lost_{0}
    {
        recv_buf_.resize(header_->slot_data_size);

        for (uint32_t i = 0; i < header_->max_subs; ++i)
        {
            auto* ring = sub_ring_at(base_, header_, i);
            // Requires Free | in_flight=0. A ring stuck at Free | in_flight>0
            // (from a crashed publisher) stays retired until the operator
            // calls reset_retired_rings(). We do NOT force-reset stale
            // in_flight: the packed layout means a late fetch_sub from a
            // slow publisher would underflow into the state bits.
            uint32_t expected = ring::make_packed(ring::Free);
            // Capture write_pos BEFORE setting Live. Once Live, publishers
            // can immediately commit via fetch_add, racing with our read.
            // Reading first ensures start_pos_ <= any position a publisher
            // can claim after seeing Live.
            uint64_t wp = ring->write_pos.load(std::memory_order_acquire);
            if (ring->state_flight.compare_exchange_strong(expected,
                    ring::make_packed(ring::Live),
                    std::memory_order_acq_rel))
            {
                // Record owner liveness so reclaim_dead_rings() can recover
                // this ring if we crash without releasing it. starttime
                // first; owner_pid (release) last, so a sweeper that reads a
                // non-zero pid also sees the matching starttime. owner_pid
                // stays 0 until here, so a sweep racing the claim sees 0 and
                // skips (treats it as a claim in progress).
                uint64_t pid = current_pid();
                ring->owner_starttime.store(process_starttime(pid),
                                            std::memory_order_relaxed);
                ring->owner_pid.store(pid, std::memory_order_release);
                ring_idx_  = i;
                // Pre-CAS wp can be stale (no HB edge to the previous
                // tenant): keep it as the drain floor, but consume from the
                // freshest value or we'd replay the previous tenancy.
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

        auto* ring = sub_ring_at(base_, header_, ring_idx_);

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
        microseconds deadline{header_->commit_timeout_us};
        nanoseconds start = kickmsg::monotonic_ns();
        while (ring::get_in_flight(
                   ring->state_flight.load(std::memory_order_acquire)) > 0)
        {
            if (kickmsg::elapsed_time(start) >= deadline)
            {
                // Publisher likely crashed. Do NOT force in_flight to 0:
                // a slow-but-alive publisher may still be mid-commit.
                // Skip drain to avoid racing with it. Leaked slot refs
                // are recoverable by GC (reclaim_orphaned_slots).
                quiesced = false;
                ++drain_timeouts_;
                break;
            }
            kickmsg::yield();
        }

        // Still Draining, so still ours to retract. A crash inside this window leaves
        // Draining + owner == 0, which reclaim_dead_rings skips: one leaked ring.
        retract_tenant(ring);

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
        , ring_idx_{other.ring_idx_}
        , start_pos_{other.start_pos_}
        , read_pos_{other.read_pos_}
        , lost_{other.lost_}
        , drain_timeouts_{other.drain_timeouts_}
        , recv_buf_{std::move(other.recv_buf_)}
        , owned_waker_{std::move(other.owned_waker_)}
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
            ring_idx_        = other.ring_idx_;
            start_pos_       = other.start_pos_;
            read_pos_        = other.read_pos_;
            lost_            = other.lost_;
            drain_timeouts_  = other.drain_timeouts_;
            recv_buf_        = std::move(other.recv_buf_);
            owned_waker_     = std::move(other.owned_waker_);
            waker_           = other.waker_;

            other.ring_idx_ = UINT32_MAX;
            other.waker_    = nullptr;
        }
        return *this;
    }

    int Subscriber::wait_fd(WakeBackend& backend)
    {
        if (waker_ != nullptr)
        {
            if (&waker_->backend() != &backend)
            {
                return -1;
            }
            return waker_->fd();
        }
        if (ring_idx_ == UINT32_MAX)
        {
            return -1;
        }

        auto owned = std::make_unique<Waker>(backend);
        if (not owned->valid())
        {
            return -1;
        }
        owned_waker_ = std::move(owned);
        waker_       = owned_waker_.get();
        return waker_->fd();
    }

    int Subscriber::attach(Waker& waker)
    {
        if (ring_idx_ == UINT32_MAX or not waker.valid())
        {
            return -1;
        }
        // Drop a private Waker this call supersedes; its socket has no other owner.
        if (owned_waker_.get() != &waker)
        {
            owned_waker_.reset();
        }
        waker_ = &waker;
        return waker.fd();
    }

    Subscriber::Wait Subscriber::head_state(SubRingHeader* ring) const
    {
        uint64_t wp = ring->write_pos.load(std::memory_order_acquire);
        if (wp <= read_pos_)
        {
            return Wait::Parked;
        }
        if (wp - read_pos_ > header_->sub_ring_capacity)
        {
            // Overrun: try_receive resynchronises and returns a sample.
            return Wait::Ready;
        }
        auto&    e   = ring_entries(ring)[read_pos_ & header_->sub_ring_mask];
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

    Subscriber::Wait Subscriber::peek() const
    {
        if (ring_idx_ == UINT32_MAX)
        {
            return Wait::Parked;
        }
        return head_state(sub_ring_at(base_, header_, ring_idx_));
    }

    Subscriber::Wait Subscriber::arm_wait()
    {
        if (ring_idx_ == UINT32_MAX)
        {
            return Wait::Parked;
        }
        auto* ring = sub_ring_at(base_, header_, ring_idx_);

        // Sampled BEFORE head_state decides the ring is empty. A publish landing between
        // that decision and this load would otherwise already be in cur, the re-read below
        // would match, and the ring would wait on a wake the publisher never sent: it read
        // has_waiter before the store below and saw WaiterNone. receive() survives the same
        // ordering only because futex_wait re-checks the word inside the kernel; poll on a
        // descriptor has no such re-check, so this is the only guard.
        uint64_t cur = ring->write_pos.load(std::memory_order_relaxed);

        Wait state = head_state(ring);
        if (state != Wait::Parked)
        {
            return state;
        }
        if (waker_ == nullptr)
        {
            // No carrier: the caller's own deadline is the only wake left.
            return Wait::Parked;
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
        return Wait::Parked;
    }

    void Subscriber::disarm_wait(bool drain_owned)
    {
        if (ring_idx_ == UINT32_MAX)
        {
            return;
        }
        // Exchange: the old value says whether this Subscriber armed, and only then is
        // a wake owed to it. Relaxed -- a publisher reading the mode just before this can
        // still signal, leaving one stale wake for the next disarm to consume.
        auto*          ring = sub_ring_at(base_, header_, ring_idx_);
        uint32_t const was  = ring->has_waiter.exchange(ring::WaiterNone,
                                                        std::memory_order_relaxed);
        if (drain_owned and was == ring::WaiterCarrier and owned_waker_ != nullptr)
        {
            owned_waker_->drain();
        }
    }

    std::optional<Subscriber::SampleRef> Subscriber::try_receive()
    {
        // Moved-from Subscriber: ring_idx_ is the UINT32_MAX sentinel, so
        // sub_ring_at would compute a wild pointer.
        if (ring_idx_ == UINT32_MAX)
        {
            return std::nullopt;
        }
        auto* ring = sub_ring_at(base_, header_, ring_idx_);

        for (int retries = 0; retries < 64; ++retries)
        {
            uint64_t wp = ring->write_pos.load(std::memory_order_acquire);
            if (wp <= read_pos_)
            {
                return std::nullopt;
            }

            uint64_t capacity = header_->sub_ring_capacity;
            if (wp - read_pos_ > capacity)
            {
                uint64_t skipped = (wp - read_pos_) - capacity;
                lost_ += skipped;
                ring->lost_count.fetch_add(skipped, std::memory_order_relaxed);
                read_pos_ = wp - capacity;
            }

            uint64_t idx  = read_pos_ & header_->sub_ring_mask;
            auto* entries = ring_entries(ring);
            auto& e       = entries[idx];

            // Acquire: ensures we see the slot_idx/payload_len written
            // by the publisher before the sequence commit.
            uint64_t seq1 = e.sequence.load(std::memory_order_acquire);
            if (seq1 != read_pos_ + 1)
            {
                if (seq_is_skip(seq1) and seq_pos(seq1) == read_pos_ + 1)
                {
                    // Skip marker: metadata untrustworthy by design.
                    ++lost_;
                    ring->lost_count.fetch_add(1, std::memory_order_relaxed);
                    ++read_pos_;
                    continue;
                }
                if (seq_is_locked(seq1) or seq_pos(seq1) < read_pos_ + 1)
                {
                    // Publisher is mid-commit (position-tagged lock) or has
                    // not committed yet. Come back later.
                    return std::nullopt;
                }
                // Entry was overwritten (seq > expected): advance and retry.
                ++lost_;
                ring->lost_count.fetch_add(1, std::memory_order_relaxed);
                ++read_pos_;
                continue;
            }

            uint32_t slot_idx    = e.slot_idx.load(std::memory_order_relaxed);
            uint32_t payload_len = e.payload_len.load(std::memory_order_relaxed);

            if (slot_idx >= header_->pool_size or payload_len > header_->slot_data_size)
            {
                ++lost_;
                ring->lost_count.fetch_add(1, std::memory_order_relaxed);
                ++read_pos_;
                continue;
            }

            // Pin the slot via refcount increment to prevent it from being
            // freed while we memcpy. Without the pin, a publisher could evict
            // the ring entry and push the slot back to the free stack, letting
            // another publisher overwrite the data mid-copy.
            auto* slot = slot_at(base_, header_, slot_idx);
            uint32_t rc = slot->refcount.load(std::memory_order_acquire);
            bool pinned = false;
            // rc == UINT32_MAX is unreachable for a healthy slot (refcount is
            // bounded by max_subs + live views); treat it as corrupt residue
            // so rc + 1 can't wrap to 0 and make a pinned slot look freeable.
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
                // refcount == 0 (or corrupt): slot not pinnable, count as lost.
                ++lost_;
                ring->lost_count.fetch_add(1, std::memory_order_relaxed);
                ++read_pos_;
                continue;
            }

            // Seqlock validation: re-read the sequence after pinning. If it
            // changed, the entry was overwritten between our first read and
            // the pin, so the slot_idx we pinned may be stale.
            uint64_t seq2 = e.sequence.load(std::memory_order_acquire);
            if (seq2 != seq1)
            {
                uint32_t prev = slot->refcount.fetch_sub(1, std::memory_order_acq_rel);
                if (prev == 1)
                {
                    treiber_push(header_->free_top, slot, slot_idx);
                }
                ++lost_;
                ring->lost_count.fetch_add(1, std::memory_order_relaxed);
                ++read_pos_;
                continue;
            }

            std::memcpy(recv_buf_.data(), slot_data(slot), payload_len);

            // Unpin: we have our copy, release the slot reference.
            uint32_t prev = slot->refcount.fetch_sub(1, std::memory_order_acq_rel);
            if (prev == 1)
            {
                treiber_push(header_->free_top, slot, slot_idx);
            }

            ++read_pos_;
            return SampleRef{recv_buf_.data(), payload_len, read_pos_ - 1};
        }
        return std::nullopt;
    }

    std::optional<Subscriber::SampleRef> Subscriber::receive(nanoseconds timeout)
    {
        // Moved-from Subscriber: ring_idx_ is the UINT32_MAX sentinel, so
        // sub_ring_at would compute a wild pointer.
        if (ring_idx_ == UINT32_MAX)
        {
            return std::nullopt;
        }
        auto*       ring  = sub_ring_at(base_, header_, ring_idx_);
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
                    nanoseconds nap = poll_budget();
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
        auto* ring = sub_ring_at(base_, header_, ring_idx_);

        for (int retries = 0; retries < 64; ++retries)
        {
            uint64_t wp = ring->write_pos.load(std::memory_order_acquire);
            if (wp <= read_pos_)
            {
                return std::nullopt;
            }

            uint64_t capacity = header_->sub_ring_capacity;
            if (wp - read_pos_ > capacity)
            {
                uint64_t skipped = (wp - read_pos_) - capacity;
                lost_ += skipped;
                ring->lost_count.fetch_add(skipped, std::memory_order_relaxed);
                read_pos_ = wp - capacity;
            }

            uint64_t idx  = read_pos_ & header_->sub_ring_mask;
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

            uint32_t slot_idx    = e.slot_idx.load(std::memory_order_relaxed);
            uint32_t payload_len = e.payload_len.load(std::memory_order_relaxed);

            if (slot_idx >= header_->pool_size or payload_len > header_->slot_data_size)
            {
                ++lost_;
                ring->lost_count.fetch_add(1, std::memory_order_relaxed);
                ++read_pos_;
                continue;
            }

            // Pin the slot so it survives until ~SampleView().
            auto* slot = slot_at(base_, header_, slot_idx);
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

            ++read_pos_;
            return SampleView{base_, header_, slot_idx, payload_len, read_pos_ - 1};
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
        auto*       ring  = sub_ring_at(base_, header_, ring_idx_);
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
                    nanoseconds nap = poll_budget();
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
        uint64_t capacity = header_->sub_ring_capacity;

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

        // Release this ring's reference for ALL committed entries in the live window:
        // - [oldest, read_pos_): consumed by try_receive (pin/unpin is net-zero,
        //   so the ring's original rc=1 reference still needs releasing).
        //   For try_receive_view, rc=2 (ring ref + SampleView pin); we release
        //   the ring ref here, ~SampleView releases the pin later.
        // - [read_pos_, wp): unconsumed entries, also need their ring ref released.
        // Evicted entries have seq != pos+1, so the check safely skips them.
        for (uint64_t pos = oldest; pos < wp; ++pos)
        {
            auto&    e   = entries[pos & header_->sub_ring_mask];
            uint64_t seq = e.sequence.load(std::memory_order_acquire);

            if (seq != pos + 1)
            {
                continue;
            }

            uint32_t slot_idx = e.slot_idx.load(std::memory_order_relaxed);
            if (slot_idx < header_->pool_size)
            {
                auto*    slot = slot_at(base_, header_, slot_idx);
                uint32_t prev = slot->refcount.fetch_sub(1,
                                    std::memory_order_acq_rel);
                if (prev == 1)
                {
                    treiber_push(header_->free_top, slot, slot_idx);
                }
                e.slot_idx.store(INVALID_SLOT, std::memory_order_seq_cst);
            }
        }

    }
}
