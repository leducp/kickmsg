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
            auto* slot = slot_at(base_, geom_, pending_slot_);
            treiber_push(header_->free_top, slot, pending_slot_);
            pending_slot_ = INVALID_SLOT;
        }
    }

    Allocation Publisher::allocate()
    {
        // Release any previously allocated but unpublished slot.
        release_pending();

        uint32_t slot_idx = treiber_pop(header_->free_top, base_, geom_);
        if (slot_idx == INVALID_SLOT)
        {
            return Allocation{nullptr, 0};
        }

        pending_slot_ = slot_idx;
        ++reservation_;

        auto* slot = slot_at(base_, geom_, slot_idx);
        return Allocation{slot_data(slot), geom_.slot_data_size};
    }

    std::size_t Publisher::publish(std::size_t len)
    {
        // Reject oversized lengths before narrowing to uint32_t.
        if (len > geom_.slot_data_size)
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

        auto*    slot     = slot_at(base_, geom_, slot_idx);
        uint64_t capacity = geom_.sub_ring_capacity;

        // Relaxed: nobody can reach this slot until a commit below publishes
        // it, and every commit is a release-CAS that carries this store.
        slot->payload_len.store(static_cast<uint32_t>(len), std::memory_order_relaxed);

        // Pre-set refcount to max_subs before publishing to any ring,
        // so a fast eviction on ring[k] cannot free the slot before
        // we finish publishing to ring[k+1].
        slot->refcount.store(static_cast<uint32_t>(geom_.max_subs),
                             std::memory_order_release);

        std::size_t delivered = 0;
        uint32_t    excess    = 0;
        bool        carrier   = false;

        for (uint32_t i = 0; i < geom_.max_subs; ++i)
        {
            auto* ring = sub_ring_at(base_, geom_, i);

            // Relaxed pre-check: a stale Free may miss one delivery;
            // a stale Live is checked by the admission CAS.
            uint32_t snapshot = ring->state_flight.load(std::memory_order_relaxed);
            if (ring::get_state(snapshot) != ring::Live)
            {
                ++excess;
                continue;
            }

            // Check Live and increment in_flight in one acquire-release CAS.
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

            uint64_t pos = ring->write_pos.fetch_add(1, std::memory_order_acq_rel);

            uint64_t idx  = pos & geom_.sub_ring_mask;
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
                // An earlier ring's wake must not wait out this ring's commit timeout.
                uint64_t const cur = e.sequence.load(std::memory_order_acquire);
                bool const blocks  = seq_is_locked(cur) or seq_pos(cur) < prev_seq;
                if (blocks and carrier and wake_backend_ != nullptr)
                {
                    wake_backend_->signal();
                    carrier = false;
                }
                wait = wait_for_commit(e, prev_seq, commit_timeout_);
            }

            // Lock and commit by CAS so a repairer can revoke this position.
            uint64_t const lock_val = seq_lock(pos);
            uint64_t observed = 0;
            if (pos >= capacity)
            {
                observed = wait.last_seq;
            }
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

            // This early check avoids work; the metadata CAS below guards late writes.
            if (e.sequence.load(std::memory_order_acquire) != lock_val)
            {
                carrier |= abandon_delivery(ring);
                ++excess;
                continue;
            }

            // Replace only an older position's claim, so a late writer cannot
            // overwrite a newer entry.
            uint64_t const my_meta  = meta_pack(pos, slot_idx);
            uint64_t       old_meta = e.meta.load(std::memory_order_acquire);
            bool           taken    = false;
            while (meta_precedes(old_meta, pos))
            {
                if (e.meta.compare_exchange_weak(old_meta, my_meta,
                        std::memory_order_acq_rel, std::memory_order_acquire))
                {
                    taken = true;
                    break;
                }
            }
            if (not taken)
            {
                // A newer publisher owns this entry: our slot reference is
                // still ours to drop.
                carrier |= abandon_delivery(ring);
                ++excess;
                continue;
            }

            // The entry now owns our reference. We must release its previous claim.
            uint32_t prev_biased = meta_slot_biased(old_meta);
            if (prev_biased != 0)
            {
                release_slot(prev_biased - 1);
            }

            // CAS-commit.  Release on success publishes the data stores.
            uint64_t expected_lock = lock_val;
            if (not e.sequence.compare_exchange_strong(expected_lock, pos + 1,
                    std::memory_order_release, std::memory_order_relaxed))
            {
                // The entry owns our reference even if commit fails. Do not release it twice.
                carrier |= abandon_delivery(ring);
                continue;
            }

            // Release admission.
            ring->state_flight.fetch_sub(ring::IN_FLIGHT_ONE,
                                         std::memory_order_release);

            // Pair with the subscriber's fence: publish write_pos before checking
            // has_waiter, so either the subscriber sees the position or we send a wake.
            std::atomic_thread_fence(std::memory_order_seq_cst);
            carrier |= wake_ring(ring);
            ++delivered;
        }

        // Release references that were not transferred to ring entries.
        // Any ring admission for these deliveries has already been released.
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
        if (len > geom_.slot_data_size)
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

        // Avoid a clock read when the predecessor is already committed.
        uint64_t first = e.sequence.load(std::memory_order_acquire);
        if (not seq_is_locked(first) and seq_pos(first) >= expected_seq)
        {
            return CommitWait{first, false};
        }

        nanoseconds start = kickmsg::monotonic_ns();
        uint64_t    seq   = first;
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
        if (entry_steal_and_skip(e, pos, seq))
        {
            header_->steal_count.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void Publisher::release_slot(uint32_t idx)
    {
        // idx is read from a ring entry a peer wrote; a crashed or hostile
        // publisher can leave it out of range (this also covers INVALID_SLOT).
        if (idx >= geom_.pool_size)
        {
            return;
        }
        auto*    s    = slot_at(base_, geom_, idx);
        uint32_t prev = s->refcount.fetch_sub(1, std::memory_order_acq_rel);
        if (prev == 1)
        {
            treiber_push(header_->free_top, s, idx);
        }
    }
}
