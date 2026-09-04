#include "kickmsg/Publisher.h"
#include "kickmsg/os/Futex.h"
#include "kickmsg/os/Time.h"

namespace kickmsg
{
    Publisher::~Publisher()
    {
        release_pending();
    }

    bool Publisher::wake_ring(SubRingHeader* ring)
    {
        // Relaxed: every call site fences seq_cst between the write_pos commit and this
        // load, which is what orders it against the subscriber's has_waiter store.
        uint32_t const waiter = ring->has_waiter.load(std::memory_order_relaxed);
        if (waiter == ring::WaiterFutex)
        {
            futex_wake_all(ring->write_pos);
        }
        return waiter == ring::WaiterCarrier;
    }

    void Publisher::release_pending()
    {
        if (pending_slot_ != INVALID_SLOT)
        {
            // Return the uncommitted slot to the free-stack.
            auto* slot = slot_at(base_, header_, pending_slot_);
            treiber_push(header_->free_top, slot, pending_slot_);
            pending_slot_ = INVALID_SLOT;
        }
    }

    Allocation Publisher::allocate()
    {
        // Release any previously allocated but unpublished slot.
        release_pending();

        uint32_t slot_idx = treiber_pop(header_->free_top, base_, header_);
        if (slot_idx == INVALID_SLOT)
        {
            return Allocation{nullptr, 0};
        }

        pending_slot_ = slot_idx;

        auto* slot = slot_at(base_, header_, slot_idx);
        return Allocation{slot_data(slot), header_->slot_data_size};
    }

    std::size_t Publisher::publish(std::size_t len)
    {
        // Oversized len would otherwise be truncated by the uint32_t store
        // into payload_len -- possibly to a small VALID length, bypassing
        // the subscriber's bound check and delivering a silently wrong
        // length.  Recycle the pending slot and report zero deliveries.
        if (len > header_->slot_data_size)
        {
            release_pending();
            return 0;
        }
        if (pending_slot_ == INVALID_SLOT)
        {
            return 0;
        }

        uint32_t slot_idx = pending_slot_;
        pending_slot_ = INVALID_SLOT;

        auto*    slot     = slot_at(base_, header_, slot_idx);
        uint64_t capacity = header_->sub_ring_capacity;

        // Pre-set refcount to max_subs before publishing to any ring,
        // so a fast eviction on ring[k] cannot free the slot before
        // we finish publishing to ring[k+1].
        slot->refcount.store(static_cast<uint32_t>(header_->max_subs),
                             std::memory_order_release);

        std::size_t delivered = 0;
        uint32_t    excess    = 0;
        bool        carrier   = false;

        for (uint32_t i = 0; i < header_->max_subs; ++i)
        {
            auto* ring = sub_ring_at(base_, header_, i);

            // Relaxed pre-check: skip obviously non-Live rings without
            // any RMW atomic. Stale reads are safe:
            //  - Sees Free, actually Live: miss one delivery (acceptable).
            //  - Sees Live, actually Draining: CAS catches it below.
            uint32_t snapshot = ring->state_flight.load(std::memory_order_relaxed);
            if (ring::get_state(snapshot) != ring::Live)
            {
                ++excess;
                continue;
            }

            // CAS admission: atomically verify state==Live and increment
            // in_flight. All ordering is on a single variable, so
            // acquire/release is sufficient (no seq_cst needed).
            uint32_t old = snapshot;
            bool admitted = false;
            while (true)
            {
                if (ring::get_state(old) != ring::Live)
                {
                    ++excess;
                    break;
                }
                if (ring->state_flight.compare_exchange_weak(old,
                        old + ring::IN_FLIGHT_ONE,
                        std::memory_order_acq_rel,
                        std::memory_order_acquire))
                {
                    admitted = true;
                    break;
                }
                // CAS failed -- old was updated. Re-check state.
            }

            if (not admitted)
            {
                continue;
            }

            // Admitted: in_flight incremented, state is Live.

            // Claim a position in this ring. fetch_add is unconditional:
            // no CAS retry loop, O(1) under contention, and compiles to
            // a single LDADDAL on AArch64 with LSE atomics.
            uint64_t pos = ring->write_pos.fetch_add(1, std::memory_order_acq_rel);

            uint64_t idx  = pos & header_->sub_ring_mask;
            auto* entries = ring_entries(ring);
            auto& e       = entries[idx];

            uint64_t prev_seq = 0;
            if (pos >= capacity)
            {
                prev_seq = pos - capacity + 1;
            }

            // Wait for the previous wrap's occupant; also records whether one
            // lock value spanned the whole timeout (self_repair's steal proof).
            CommitWait wait{0, false};
            if (pos >= capacity)
            {
                wait = wait_for_commit(e, prev_seq, commit_timeout_);
            }

            // Two-phase commit: CAS to our lock, write data, CAS-commit.
            // A repairer's theft makes both CASes fail instead of being
            // blind-stored over.
            uint64_t const lock_val = seq_lock(pos);
            uint64_t observed = 0;
            if (pos >= capacity)
            {
                observed = wait.last_seq;
            }
            bool prev_was_skip = false;
            bool locked = false;
            for (int attempt = 0; attempt < 64; ++attempt)
            {
                if (not seq_is_locked(observed) and seq_pos(observed) == prev_seq)
                {
                    // CAS from the exact observed value (plain or skip-tagged).
                    uint64_t expected = observed;
                    // Acquire on success: we need to see the previous writer's stores.
                    if (e.sequence.compare_exchange_weak(expected, lock_val,
                            std::memory_order_acquire, std::memory_order_relaxed))
                    {
                        prev_was_skip = seq_is_skip(observed);
                        locked = true;
                        break;
                    }
                    observed = expected;
                    continue;
                }
                if (not seq_is_locked(observed))
                {
                    break;  // committed elsewhere: stale residue, can't lock
                }
                observed = e.sequence.load(std::memory_order_relaxed);
            }
            if (not locked)
            {
                // Heal a provably-stale entry so the next publisher here
                // does not pay the timeout again.
                self_repair(e, pos, capacity, wait);
                carrier |= abandon_delivery(ring);
                ++excess;
                continue;
            }

            // Release the previous occupant's slot from the post-lock read
            // (sees even a commit that landed after our wait timed out; a
            // drain's INVALID marker fails the bound check).  Never for a
            // skip predecessor (untrustworthy metadata), never below one
            // wrap (zero-init slot_idx would read as valid slot 0).
            if (pos >= capacity and not prev_was_skip)
            {
                uint32_t prev_slot = e.slot_idx.load(std::memory_order_acquire);
                if (prev_slot < header_->pool_size)
                {
                    release_slot(prev_slot);
                }
            }

            // Theft guard: a repairer may have stolen our lock during a
            // stall; storing data now would tear the repaired entry.
            if (e.sequence.load(std::memory_order_acquire) != lock_val)
            {
                carrier |= abandon_delivery(ring);
                ++excess;
                continue;
            }

            e.slot_idx.store(slot_idx, std::memory_order_relaxed);
            e.payload_len.store(static_cast<uint32_t>(len), std::memory_order_relaxed);

            // CAS-commit: fails only on theft after the guard above.
            // Release on success publishes the data stores.
            uint64_t expected_lock = lock_val;
            if (not e.sequence.compare_exchange_strong(expected_lock, pos + 1,
                    std::memory_order_release, std::memory_order_relaxed))
            {
                carrier |= abandon_delivery(ring);
                ++excess;
                continue;
            }

            // Release admission.
            ring->state_flight.fetch_sub(ring::IN_FLIGHT_ONE,
                                         std::memory_order_release);

            // seq_cst fence orders the write_pos fetch_add before the
            // has_waiter load: without it a weakly-ordered CPU can read
            // has_waiter == 0 stale and skip the wake to a subscriber already
            // parked in futex_wait (a lost wakeup until its timeout). Pairs
            // with the subscriber's fence. x86's locked RMW already fences,
            // which is why this never surfaced on x86.
            std::atomic_thread_fence(std::memory_order_seq_cst);
            carrier |= wake_ring(ring);
            ++delivered;
        }

        // Batch release excess refs for all non-delivered rings.
        // Safe because: Free rings have no drain to race with, and
        // Draining rings where CAS failed never admitted us (in_flight
        // was never incremented), so their drain doesn't depend on us.
        if (excess > 0)
        {
            uint32_t prev = slot->refcount.fetch_sub(excess,
                                std::memory_order_acq_rel);
            if (prev == excess)
            {
                treiber_push(header_->free_top, slot, slot_idx);
            }
        }

        // Skip markers advance write_pos too, so they owe a wake like a delivery does.
        if (carrier and wake_backend_ != nullptr)
        {
            wake_backend_->signal();
        }

        return delivered;
    }

    int32_t Publisher::send(void const* data, std::size_t len)
    {
        if (len > header_->slot_data_size)
        {
            return -EMSGSIZE;
        }

        auto a = allocate();
        if (a.data == nullptr)
        {
            return -EAGAIN;
        }

        std::memcpy(a.data, data, len);
        publish(len);
        return static_cast<int32_t>(len);
    }

    Publisher::CommitWait Publisher::wait_for_commit(Entry& e, uint64_t expected_seq,
                                                     microseconds timeout)
    {
        constexpr int CHECK_INTERVAL = 1024;
        nanoseconds start = kickmsg::monotonic_ns();

        uint64_t first = e.sequence.load(std::memory_order_acquire);
        uint64_t seq   = first;
        int i = 0;
        while (true)
        {
            if (not seq_is_locked(seq) and seq_pos(seq) >= expected_seq)
            {
                return CommitWait{seq, false};
            }
            ++i;
            if ((i & (CHECK_INTERVAL - 1)) == 0)
            {
                if (kickmsg::elapsed_time(start) >= timeout)
                {
                    // Same lock value at both ends proves one holder spanned
                    // the window (steal precondition).
                    bool stable = seq_is_locked(first) and seq == first;
                    return CommitWait{seq, stable};
                }
            }
            seq = e.sequence.load(std::memory_order_acquire);
        }
    }

    bool Publisher::abandon_delivery(SubRingHeader* ring)
    {
        ++dropped_;
        ring->dropped_count.fetch_add(1, std::memory_order_relaxed);
        ring->state_flight.fetch_sub(ring::IN_FLIGHT_ONE,
                                     std::memory_order_release);
        // write_pos already advanced and the position may now carry a skip
        // marker: without the wake a parked subscriber sleeps its timeout.
        std::atomic_thread_fence(std::memory_order_seq_cst);
        return wake_ring(ring);
    }

    void Publisher::self_repair(Entry& e, uint64_t pos, uint64_t capacity,
                                CommitWait const& wait)
    {
        uint64_t seq  = e.sequence.load(std::memory_order_acquire);
        uint64_t done = pos + 1;

        if (seq_is_locked(seq))
        {
            // A lock that appeared mid-wait may be a healthy commit -- only
            // steal one proven stable across the full wait.
            if (not wait.stable_lock or seq != wait.last_seq)
            {
                return;
            }
        }
        else if (seq_pos(seq) + capacity >= done)
        {
            return;  // at most one wrap behind: normal contention residue
        }
        if (entry_steal_and_clear(e, pos, seq))
        {
            header_->steal_count.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void Publisher::release_slot(uint32_t idx)
    {
        // idx is read from a ring entry a peer wrote; a crashed or hostile
        // publisher can leave it out of range (this also covers INVALID_SLOT).
        if (idx >= header_->pool_size)
        {
            return;
        }
        auto*    s    = slot_at(base_, header_, idx);
        uint32_t prev = s->refcount.fetch_sub(1, std::memory_order_acq_rel);
        if (prev == 1)
        {
            treiber_push(header_->free_top, s, idx);
        }
    }
}
