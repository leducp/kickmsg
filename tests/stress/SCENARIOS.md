# Kickmsg Stress Test Scenarios

All scenarios run as part of `kickmsg_stress_test`. Use `endurance.sh` for
extended runs. TSAN builds scale message counts by 100x to keep runtime
manageable.

Common oracles for the no-crash scenarios (everything except `gc_recovery`
and `live_repair`, which inject damage by design):

- **Readiness barrier**: subscribers signal after constructing their
  `Subscriber`; publishers wait for all of them before the first send. Every
  ring is Live for the whole run, which makes per-subscriber accounting exact.
- **Exact conservation**: `received + lost (+ corrupt/bad/reorder) ==
  total_sent` per subscriber -- messages can neither vanish nor duplicate.
- **GC-zero**: `repair_locked_entries()` must fix 0 entries, and
  `reclaim_orphaned_slots()` must reclaim no more slots than the publishers'
  ring `dropped_count` total. Drops happen when a publisher descheduled past
  `commit_timeout` has its entry lock stolen by a peer (`self_repair`); each
  steal deliberately leaks one slot ref that only the GC recovers, so
  reclaims within the drop budget are stall residue, not a leak. Any repair,
  or any reclaim beyond that budget, is a normal-path bug that the GC would
  otherwise silently mask before the structural verifies.

## Treiber stack (`treiber.cc`)

**What**: 8 threads × 100K pop/push cycles on the lock-free free-stack.

**Why**: Validates ABA safety of the tagged-pointer Treiber stack under high
contention. Every cycle pops a slot, writes to it, and pushes it back.

**Config**: pool=64, 8 threads.

**Failure means**: ABA bug in the Treiber stack, corruption of the free-list
linked structure, or slot duplication.

## Subscriber churn (`churn.cc`)

**What**: 4 subscriber threads repeatedly join and leave (5 rounds each) while
a publisher sends continuously.

**Why**: Exercises the full ring lifecycle: Free -> Live -> Draining -> Free.
Tests drain_unconsumed correctness, in_flight quiescence spin, and ring reuse.

**Config**: max_subs=4, ring=32, pool=128, 10K messages.

**Failure means**: Double-decrement on drain, ring state corruption on reuse,
or refcount leak.

## GC recovery (`gc_recovery.cc`)

**What**: Manually poisons a ring entry with a stale position-tagged lock and orphans a slot
with refcount > 0. Calls `repair_locked_entries()` and `reclaim_orphaned_slots()`
and verifies they fix both issues.

**Why**: Validates the explicit recovery API that operators use after a publisher
crash.

**Failure means**: GC does not repair the poisoned entry, does not reclaim the
orphaned slot, or corrupts the free-stack.

## Fairness (`fairness.cc`)

**What**: 1 publisher × 16 subscribers, 100K messages. Measures the receive
distribution spread (min vs max across subscribers).

**Why**: Verifies that per-subscriber rings provide equal service -- a slow
subscriber should not starve fast ones.

**Config**: ring=256, pool=512 (large enough for no eviction pressure).

**Failure means**: Extreme receive imbalance, zero-receive subscriber, or
data corruption.

## MPMC (`mpmc.cc`)

**What**: Parameterized multi-publisher multi-subscriber stress with 7 configs:

| Pubs | Subs | Msgs/Pub | Pool | Ring | Mode |
|------|------|----------|------|------|------|
| 2 | 4 | 100K | 256 | 64 | copy |
| 8 | 8 | 50K | 128 | 32 | copy |
| 1 | 1 | 500K | 64 | 16 | copy |
| 16 | 16 | 20K | 32 | 8 | copy |
| 2 | 4 | 100K | 256 | 64 | zerocopy |
| 8 | 8 | 50K | 128 | 32 | zerocopy |
| 16 | 16 | 20K | 32 | 8 | zerocopy |

**Why**: Core correctness test. Validates payload integrity (magic + checksum),
per-publisher sequence ordering, refcount lifecycle, and pool integrity across
a range of contention levels and both receive modes.

**Failure means**: Data corruption, per-publisher reorder, refcount leak, or
Treiber stack corruption.

## Pool exhaustion (`pool_exhaustion.cc`)

**What**: 8 publishers fight over 8 slots while 4 subscribers consume slowly
(1us sleep between receives).

**Why**: Maximizes the -EAGAIN / retry rate. Tests Treiber stack under extreme
pop/push frequency and refcount correctness when most sends fail.

**Config**: pool=8, ring=4, max_subs=4, 10K msgs per publisher.

**Failure means**: Treiber ABA under extreme cycling, refcount underflow from
batch excess, or double-push corrupting the free-stack.

## Live repair (`live_repair.cc`)

**What**: 4 publishers + 4 subscribers running for 2 seconds. A background
injector periodically poisons ring entries with stale position-tagged locks. A background
healer calls `diagnose()` + `repair_locked_entries()`.

**Why**: Validates the claim that `repair_locked_entries()` is safe under live
traffic. The repair does a plain store to `sequence` while publishers may be
CAS-ing the same entry -- this test verifies the "benign double-store" argument.

**Failure means**: Data corruption caused by repair racing with a live publisher,
or repair failing to unblock a poisoned entry.

## Single-slot ring (`edge_cases.cc`)

**What**: ring=2 (smallest valid power-of-2), pool=32, 4 publishers × 10K
messages.

**Why**: Every publish wraps and evicts the previous entry. Hammers
`wait_for_commit` on every message. Tests the wrap + two-phase commit
hot path with zero buffering.

**Failure means**: Eviction race, wait_for_commit timeout under normal
load, or refcount corruption from immediate reuse.

## Big payload (`big_payload.cc`)

**What**: 4 publishers × 50K messages of 8 KB through a small pool (16) and
tiny rings (8), so eviction constantly races readers. One subscriber consumes
by copy (`try_receive`), one zero-copy (`try_receive_view`). Each payload is a
header (magic, pub_id, seq, byte_count, FNV-1a checksum) followed by a
deterministic byte pattern over the full 8 KB; readers re-derive the pattern
and checksum for every byte of every sample. The zero-copy reader validates
through the `SampleView` twice -- a second pass failing after a clean first
pass proves the slot was overwritten while pinned.

**Why**: The small `Payload` used elsewhere fits in one cache line, so a torn
read there is nearly impossible to observe. 8 KB spans many lines and takes
long enough to copy/validate that an eviction racing the pin/seqlock window
has a real chance of being caught.

**Config**: pool=16, ring=8, max_subs=2, payload=8192 B, 4 pubs × 50K msgs.

**Failure means**: Torn read (pin or seqlock violation), pin not held for the
view's lifetime, checksum/pattern corruption, conservation miss, or refcount
leak.

## Subscriber saturation (`edge_cases.cc`)

**What**: max_subs=4, attempt to create 6 subscribers. Verify 5th and 6th
throw `std::runtime_error`. Destroy one, verify a new subscriber can join.
Publisher runs throughout.

**Why**: Tests the subscriber slot allocation boundary and ring reuse after
a subscriber disconnects.

**Failure means**: Subscriber joins a non-Free ring, ring leak after disconnect,
or data corruption during the join/leave cycle.
