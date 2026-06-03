# Kickmsg Architecture

## Shared-memory Layout

Every Kickmsg channel is a single POSIX shared-memory region containing
three contiguous areas:

```
Shared Memory Region (/dev/shm/prefix_topic)
┌─────────────────────────────────────────────────────────────────┐
│  Header              │  Subscriber Rings (N)  │  Slot Pool      │
│  (offsets, config,   │  (one MPSC ring per    │  (shared data   │
│   Treiber stack top) │   subscriber)          │   slots)        │
└─────────────────────────────────────────────────────────────────┘
```

The header stores offsets to the rings and pool sections, making the
layout self-describing and forward-compatible.


## Concurrency Model

At the channel level, Kickmsg is **MPMC** (N publishers, M subscribers).
Internally this is decomposed into **M independent MPSC rings**: each
subscriber owns exactly one ring, and all publishers write to all active
rings. No ring ever has two readers.

```
                      Treiber Free Stack
                      (lock-free, ABA-safe)
                      ┌───────────────────────┐
                 ┌───►│ free_top [gen:17 | 3] │◄────────────────────────┐
                 │    └─────────┬─────────────┘                         │
           1.pop │              │ [3]→[8]→[14]→NIL                      │
                 │              │ (linked via next_free)                │ 5.push
                 │              │                                       │ (rc→0)
            ┌────┴────┐         │   Slot Pool                           │
            │ Pub A   │         │   ┌─────────────────┐                 │
            │         │         │   │ Slot[0]  rc:2   │ ◄── published   │
            │ 2.write │         │   │ Slot[1]  rc:0   │ ◄── free ───────┤
            │ payload │         │   │ Slot[2]  rc:1   │ ◄── published   │
            │  into   │         │   │ Slot[3]  rc:0   │ ◄── free        │
            │  slot   │         │   │ ...             │                 │
            │         │         │   │ Slot[15] rc:0   │ ◄── free ───────┘
            │ 3.set   │         │   └─────────────────┘
            │ refcount│         │
            │   = N   │         │
            └────┬────┘         │
                 │              │
  4.push slot    │        ┌─────┴─────┐
  index to each  │        │           │
  active ring    │        ▼           ▼
                 ├──►┌─Ring[0]──────┐ ┌─Ring[1]──────┐ ┌─Ring[2]──────┐
                 │   │ write_pos: 42│ │ write_pos: 42│ │ (state=Free) │
                 │   │ MPSC via CAS │ │ MPSC via CAS │ │  unused      │
                 │   └──────┬───────┘ └──────┬───────┘ └──────────────┘
                 │          │                │
                 └──►...    │                │
                            │                │
           read + rc--      │                │  read + rc--
          (or evict+rc--)   │                │ (or evict+rc--)
                            ▼                ▼
                       Subscriber X     Subscriber Y
                       (read_pos=41)    (read_pos=39)
                        process-local    process-local
```

**Full cycle**: a publisher (1) pops a free slot from the Treiber stack,
(2) writes its payload, (3) pre-sets the refcount to `max_subs` (the
maximum number of subscriber rings), then (4) pushes the slot index into
each Live ring via CAS, releasing the reference inline for non-Live rings.
On the other end, each subscriber reads entries from its own ring and
decrements the slot's refcount. When the ring wraps, the publisher evicts
the oldest entry and decrements its slot's refcount too. In both cases,
whoever drives the refcount to 0 (5) pushes the slot back to the free
stack, completing the cycle.

The rings are independent: each subscriber consumes at its own pace.
A slow subscriber only overflows **its own ring** -- fast subscribers
are unaffected and keep receiving everything without loss.

```
Example: mixed-speed subscribers, all seeing the same write_pos

Ring[0] → fast sub     write_pos=1000, read_pos=998   (2 pending, plenty of room)
Ring[1] → slow sub     write_pos=1000, read_pos=936   (64 pending, ring full!)
Ring[2] → medium sub   write_pos=1000, read_pos=980   (20 pending, fine)
```

This is the entire point of per-subscriber rings vs. a single shared
MPMC ring: **no cross-subscriber impact**.


## Payload Contract

Kickmsg slots carry raw bytes -- there is no serialization or
deserialization step on the hot path. The publisher `memcpy`s (or
directly writes via `allocate()`) into a shared-memory slot, and the
subscriber reads the bytes as-is. This eliminates encoding overhead
entirely, but it requires that **payloads are self-contained**:

- **No pointers or references.** A pointer is only valid in the
  address space of the process that created it. Shared memory is mapped
  at different virtual addresses in each process, so any pointer stored
  in a slot will be meaningless (or dangerous) to the reader.
- **No heap-owning types.** `std::string`, `std::vector`, smart
  pointers, etc. contain internal pointers to heap allocations. They
  must not be placed directly in a slot.
- **POD structs are fine.** Fixed-size structs of scalars, enums, and
  arrays (e.g. `struct Imu { float ax, ay, az; uint64_t timestamp; }`)
  work out of the box.
- **Variable-length data** is supported by writing the raw bytes and
  passing the length via `send(ptr, len)`. The receiver gets the length
  from `SampleRef::len()` or `SampleView::len()`.

If you need to send complex or dynamically-sized types, serialize them
into the slot yourself (e.g. FlatBuffers, Protocol Buffers, or a
custom wire format). Kickmsg handles the transport; serialization is
the user's responsibility.


## Shared-memory Header

The region header is self-describing and forward-compatible:

```
Header (at offset 0)
┌───────────────────────────────────────────────────────────┐
│  magic (atomic)     0x4B49434B4D534721 ("KICKMSG!")       │
│  version            7                                     │
│  channel_type       PubSub | Broadcast                    │
│  total_size         total mmap size in bytes              │
│  sub_rings_offset   byte offset to first subscriber ring  │
│  pool_offset        byte offset to slot pool              │
│  max_subs           max subscriber slots                  │
│  sub_ring_capacity  entries per ring (power of 2)         │
│  sub_ring_mask      sub_ring_capacity - 1                 │
│  pool_size          number of slots in the pool           │
│  slot_data_size     max payload bytes per slot            │
│  slot_stride        slot_data_size + metadata, aligned    │
│  sub_ring_stride    ring header + entries, aligned        │
│  commit_timeout_us  crash detection timeout (microseconds)│
│  config_hash        FNV-1a hash of config (mismatch guard)│
│  creator_pid        PID of the creating process           │
│  created_at_ns      creation timestamp (nanoseconds)      │
│  creator_name_len   length of creator name string         │
│  creator_name[]     variable-length name (debugging)      │
│  schema_state       Unset | Claiming | Set (atomic u32)   │
│  schema_data        SchemaInfo — 512 B, 8 cache lines     │
│  free_top           Treiber stack head (atomic u64)       │
│  steal_count        stale entries stolen by repair (u64)  │
│  identity_hash      logical-identity stamp (u64, 0=unset) │
└───────────────────────────────────────────────────────────┘
```

Offsets (rather than fixed struct sizes) allow extending the header
without breaking existing readers. The `magic` field is an
`atomic<uint64_t>` written last with `release` during init and polled
with `acquire` by `create_or_open()` to spin-wait until the creator
has finished initialization.


## Payload Schema Descriptor

Kickmsg carries opaque byte buffers on the data path — the library
never interprets a payload. For an IPC system that is exactly what
the hot path needs, but it leaves a real problem at the edges:
**two processes attached to the same region can disagree on what the
bytes mean**, because they were built at different times, from
different sources, or against different message definitions.

The descriptor is an **opt-in, off-hot-path** slot in the header
that lets users detect this disagreement. The library provides the
mechanism (a 512-byte fixed-layout blob with a publish protocol);
the user provides the policy (what fingerprints to compute, what
counts as a mismatch, how to react).

```
SchemaInfo (512 B, 8 cache lines)
┌──────────────────────────────────────────────────────────────┐
│  identity[64]       logical fingerprint (user-defined bytes) │
│  layout[64]         structural fingerprint (user-defined)    │
│  name[128]          null-terminated, for diagnostics         │
│  version            uint32 — user-defined version number     │
│  identity_algo      uint32 — user tag (0 = unspecified)      │
│  layout_algo        uint32 — user tag (0 = unspecified)      │
│  flags              uint32 — reserved bit flags              │
│  reserved[240]      future fields — zero on write            │
└──────────────────────────────────────────────────────────────┘
```

**Two-slot design.** Splitting identity (logical type) from layout
(this binary's actual memory layout) lets users distinguish *wrong
type* from *same type, different ABI*: two binaries may agree on
`identity` (same logical Pose) but diverge on `layout` (one added a
field, bumping `sizeof`). The library reports both back via
`SchemaInfo`; callers decide whether ABI skew is tolerable.

**Generous reserve.** 240 bytes of `reserved[]` are zeroed on every
write so future additions (e.g. a creator host string, a descriptor
URL, a signature) don't require another version bump.

### Publish protocol

The descriptor is published via a three-state atomic (`schema_state`):

```
       try_claim_schema()              memcpy schema_data
    ┌───── CAS ─────────►┌─────────────────────────►┌─────────┐
    │ Unset              │   Claiming               │   Set   │
    │ (state = 0)        │   (state = 1)            │(state=2)│
    └────────────────────┘   payload bytes written  └─────────┘
                             between Claiming and Set
```

- Writer CAS `Unset → Claiming` (acq_rel). Winner memcpys the
  payload into `schema_data`, then release-stores `Set`.
- Reader acquire-loads `schema_state`. If `Set`, the payload is
  stable to read. If `Unset` or `Claiming`, `schema()` returns
  `std::nullopt`.
- Losers of the CAS briefly yield if the observed state is
  `Claiming` (bounded by a small iteration budget so a crashed
  claimant can't wedge callers forever), then return `false` —
  callers read back with `schema()` and apply their own mismatch
  policy.

### Crash-recovery primitive

If a claimant is killed between the `Unset → Claiming` CAS and the
release-store of `Set`, the slot stays wedged at `Claiming` and
every future `try_claim_schema()` returns `false` after its bounded
wait. `SharedRegion::reset_schema_claim()` is the operator-driven
recovery: it atomically CASes `Claiming → Unset` so a new claim can
proceed. This mirrors the safety contract of `reset_retired_rings()`
— **not safe under live traffic**; only call after confirming the
original claimant is gone, otherwise a slow-but-alive writer could
still finish its memcpy and release-store `Set` while a new claim
is concurrently reusing the slot, producing torn bytes.

At `SharedRegion::create()` time the creator is the single writer
and no concurrent reader can observe the region yet (the `magic`
sentinel is published last), so `cfg.schema` is written directly
and `schema_state` is stamped `Set` with a relaxed store. The
state machine only matters for late claims against an already-live
region.

### Scope

- **Not on the hot path.** Readers consult the descriptor at
  connect time (a handful of times per process lifetime), never on
  `send` / `receive`. The 512 B blob lives on its own cache lines
  and never shares a line with `free_top` or any ring header.
- **Orthogonal to `config_hash`.** Schema presence/absence does not
  participate in the geometry hash, so a typed publisher can share
  a region with an untyped subscriber as long as the channel
  geometry matches. Users opt in to schema enforcement on their
  own terms.
- **Library is policy-agnostic.** `try_claim_schema()` returning
  `false` is not an error — it just says "someone else got there
  first, here's what they wrote, you decide." The library never
  throws on schema grounds.

Layout version history since the schema descriptor (v4): v5 added
the cross-process runtime counters to `SubRingHeader`
(`dropped_count`, `lost_count` -- see the Subscriber Ring section);
v6 added the ring owner identity (`owner_pid`, `owner_starttime`)
that `reclaim_dead_rings()` keys on; v7 changed the entry commit
protocol (position-tagged locks, skip markers -- see the
sequence-word encoding below) and added `steal_count` and
`identity_hash` to the header. Each bump changes the in-memory
meaning of shared bytes, so mixed-version binaries cannot share a
region; mismatches are rejected at `open()` by the version check.


## Treiber Free Stack

The slot pool is managed as a lock-free Treiber stack. Each free slot's
`next_free` field points to the next free slot, forming a singly-linked
list. The stack head (`free_top` in the Header) is a 64-bit atomic
packing a 32-bit generation counter and a 32-bit slot index to prevent
ABA.

```
free_top [gen:17 | idx:3]
     │
     ▼
  Slot[3]  ──next──▶  Slot[8]  ──next──▶  Slot[14]  ──next──▶  INVALID
  rc:0                rc:0                 rc:0
```

- **Pop** (allocate a slot): CAS `free_top` from `[gen|head]` to
  `[gen+1|head.next]`. The generation increment prevents ABA.
- **Push** (release a slot): CAS `free_top` from `[gen|head]` to
  `[gen+1|slot]` after setting `slot.next = head`.

**Accepted ABA bound.** The generation is 32-bit, so the ABA window is not
formally closed: it requires a thread to read `free_top`, stall, and then
have exactly 2^32 push/pop operations complete (returning the head to the
same `[gen|idx]`) before it resumes its CAS. At realistic pool churn that is
on the order of tens of seconds of uninterrupted contention landing inside a
single preemption of one thread -- astronomically unlikely, and no real
workload approaches it. We accept this rather than pay for a 128-bit DWCAS
(`cmpxchg16b` / `LDXP`-`STXP`), which is not lock-free in the `std::atomic`
sense on every target and costs on the hot alloc/free path. If a future
target makes the window reachable, widen `free_top` to a 128-bit tagged
pointer (a `VERSION` bump).


## Subscriber Ring

Each ring is a fixed-size circular buffer of `Entry` records. An entry
contains a sequence number, slot index, and payload length -- all atomic.

```
Ring[0]
┌──────────────────────────────────────────────────────────┐
│  state: Live   in_flight: 0   write_pos: AtomicU64 = 42  │
│                                                          │
│  entries[0..7]:                                          │
│  ┌─────┬───────────┬──────────┬─────────────┐            │
│  │ idx │ sequence  │ slot_idx │ payload_len │            │
│  ├─────┼───────────┼──────────┼─────────────┤            │
│  │  0  │    37     │    5     │    128      │            │
│  │  1  │    38     │   12     │    256      │            │
│  │  2  │    39     │    0     │     64      │            │
│  │  3  │    40     │    7     │    512      │            │
│  │  4  │    41     │    2     │   1024      │ ◄── latest │
│  │  5  │    42     │   11     │    128      │ ◄── newest │
│  │  6  │    35     │    9     │    256      │ ◄── stale  │
│  │  7  │    36     │    1     │     64      │ ◄── stale  │
│  └─────┴───────────┴──────────┴─────────────┘            │
└──────────────────────────────────────────────────────────┘
```

- **Capacity** must be a power of 2 (index masking: `pos & (cap - 1)`).
- **state_flight** (atomic uint32): packed `[in_flight:30 | state:2]`.
  State bits: `Free(0)`, `Live(1)`, `Draining(2)`, `Reclaiming(3)`.
  In_flight bits: number of publishers currently admitted to this ring.
  Packing into a single variable eliminates cross-variable ordering
  concerns: the publisher's CAS atomically checks state and increments
  in_flight, so acquire/release is sufficient (no seq_cst needed).
- **write_pos** (atomic uint64): monotonically increasing position counter.
  Publishers claim positions via `fetch_add` (unconditional, O(1)).
- **has_waiter** (atomic uint32): set by the subscriber before blocking
  on `futex_wait`, cleared after. Publishers skip the `futex_wake_all`
  syscall when no subscriber is sleeping.
- **dropped_count** (atomic uint64): cumulative count of publisher
  delivery drops on this ring — incremented whenever the two-phase
  commit CAS lock fails and the publisher falls through to the self-
  repair / drop path. Cross-process visible; the per-Publisher
  `dropped()` accessor reports the same events but counted per
  instance.
- **lost_count** (atomic uint64): cumulative count of subscriber
  losses on this ring — bumped on every `++lost_` site (drain-ahead
  skip, stale-overwrite, invalid-slot, unpinnable, seqlock miss).
  Cross-process visible; `Subscriber::lost()` reports the same events
  per instance.
- **Sequence number** is monotonically increasing (`pos + 1`), used as a
  seqlock for data consistency validation and as a commit barrier between
  publishers. The word is tag-encoded (see the encoding table below).
- Stale entries (sequence < subscriber's expected) are detected and
  reported as lost messages.

`write_pos`, `has_waiter`, `dropped_count`, `lost_count` all share one
cache line: the hot path already owns this line when incrementing
`write_pos`, so bumping the counters on a rare drop/loss adds no new
coherency traffic.  Different rings target different lines (128 B
stride), so publishers/subscribers on distinct rings never contend.

### Entry sequence-word encoding

Each entry's 64-bit `sequence` packs a 2-bit tag (bit 63 = lock bit,
bit 62 = repair bit) and a 62-bit value:

```
[tag:2 | value:62]
  00  committed       word is pos + 1; slot_idx / payload_len valid
  01  skip marker     word carries pos + 1; committed but EMPTY --
                      metadata untrustworthy by design
  10  locked          a publisher is mid-commit at pos
                      (position-tagged lock, unique per position)
  11  repair-locked   a repairer owns the entry mid-steal
```

Lock values are unique -- a position is claimed by exactly one
publisher, which locks it at most once -- so an unchanged lock value
across a full timeout window proves a single holder spanned it: the
staleness proof every steal relies on.

Stolen entries are committed as **skip markers**, not plain
sequences. The stolen-from publisher's late metadata stores can land
at any time after the steal, so nothing may ever trust `slot_idx` /
`payload_len` under tag 01: subscribers count a skip-marked position
as one lost message without reading its metadata, and the eviction
path never releases a slot through a skip-tagged entry. The victim's
stores are harmless by construction, not by timing.

### Subscriber join and visibility window

A subscriber joins by CAS-ing a Free ring to Live. The CAS expects
exactly `Free | in_flight=0` (packed value 0). A ring stuck at
`Free | in_flight>0` (from a crashed publisher whose subscriber
teardown timed out) stays retired until the operator calls
`reset_retired_rings()` to recover it.

Direct acceptance of non-zero in_flight would be unsafe: the packed
`state_flight` layout means a late `fetch_sub(IN_FLIGHT_ONE)` from
a slow publisher would underflow into the state bits, corrupting
the ring for the new subscriber. Force-resetting in_flight to 0
would also be unsafe: `commit_timeout` is a heuristic, not proof
of death. A slow-but-alive publisher could still execute its
pending `fetch_sub`, causing the same underflow.

**Publisher self-repair**: when a publisher times out on a stuck
entry, it heals the entry in place (a CAS steal, then a skip-marker
commit) so the next publisher at that position succeeds without
timeout. This handles both Case A (stale position-tagged lock, only
when one lock value provably spanned the whole timeout) and Case B
(stale entry >1 wrap behind).
The operator primitives `repair_locked_entries()` and
`reset_retired_rings()` remain available for defense-in-depth and
health monitoring, but most crash residue is now self-healed by
publishers on the hot path.

**Ordering invariant**: the subscriber reads `write_pos` BEFORE the
CAS to Live (`Subscriber.cc`: `write_pos.load(acquire)` then
`state_flight.compare_exchange_strong(Free, Live, acq_rel)`), and
reads it AGAIN after winning the CAS. The two reads serve different
purposes:

- The pre-CAS value becomes `start_pos_`, the drain floor. Once the
  ring is Live, publishers can immediately `fetch_add(write_pos)`,
  racing with the read; capturing first guarantees `start_pos_ <= any
  position a publisher can claim after seeing Live`. Without that,
  `drain_unconsumed`'s window `[start_pos_, wp)` could miss entries
  committed between the CAS and the read -- a refcount leak.
- The post-CAS value seeds `read_pos_` (the larger of the two reads
  wins). The pre-CAS value can be stale -- there is no happens-before
  edge to the previous tenant's final position -- and consuming from
  it would replay messages left over from the previous tenancy.

**Anyone editing the Subscriber constructor must preserve this order.**

A newly joined subscriber may miss a small number of in-flight
publishes during the visibility window right after attachment:
publishers using a relaxed pre-check may still see the ring as
non-Live (stale read). Steady-state delivery begins once all
publishers observe the ring as Live.


## Publish Flow

Any publisher can call `send()` or `allocate()` + `publish()`. Multiple
publishers may race concurrently on the same channel.

```
Publisher
   |
   v
1. treiber_pop(free_top)           Allocate a slot from the free stack.
   -> Slot[3]                      CAS on free_top (ABA-safe).
   |
   v
2. memcpy payload -> Slot[3].data  Write payload into the slot.
   |
   v
3. Slot[3].refcount = max_subs     Pre-set to max subscriber count.
   |                               Done BEFORE publishing to any ring, so
   |                               the slot cannot be freed prematurely.
   v
4. For each Ring[i]:
   |
   |-- Relaxed pre-check:           Skip obviously non-Live rings with a
   |     state_flight (relaxed)     single relaxed load (no RMW atomic).
   |     if state != Live:          A stale read may miss a just-joined
   |       excess++, continue       subscriber (acceptable — lossy).
   |                                The CAS below catches false positives.
   |
   |-- CAS admission on             Atomically verify state==Live and
   |   state_flight (acq_rel):      increment in_flight in one CAS.
   |     old + IN_FLIGHT_ONE        Packed state_flight: single-variable
   |                                CAS atomically checks state AND bumps
   |     if state changed to        in_flight. acquire/release is sufficient
   |     non-Live during CAS:       (no seq_cst, no cross-variable ordering).
   |       excess++, continue
   |
   |-- fetch_add(write_pos, 1)     Claim position 42. Unconditional:
   |                                O(1) under contention, compiles to
   |                                a single LDADDAL on AArch64 (LSE).
   |
   |-- If ring full (wrap):
   |     wait_for_commit()         Spin-wait (check clock every 1024
   |     |                         iterations) up to commit_timeout
   |     |                         (default 10 ms).
   |     |-- Committed:            Proceed to the lock CAS. The previous
   |     |                         occupant's slot is released AFTER the
   |     |                         lock succeeds, from a post-lock read
   |     |                         of slot_idx (see below).
   |     '-- Timeout (crash or     Records whether ONE position-tagged
   |          stall):               lock value spanned the whole wait —
   |                                the proof self-repair needs before
   |                                stealing the lock.
   |
   |-- Two-phase commit:
   |   |
   |   | Phase 1 - CAS lock:
   |   |   CAS entry.sequence       CAS from the OBSERVED previous value
   |   |     observed ->             -- a plain commit at prev_seq OR a
   |   |     seq_lock(pos)           skip marker carrying prev_seq -- to
   |   |                             seq_lock(pos), the position-tagged
   |   |                             lock [tag:2|pos:62]. This exclusively
   |   |                             owns the entry: no other publisher
   |   |                             can CAS from a lock value since they
   |   |                             expect an unlocked word at prev_seq,
   |   |                             and every locker's value is unique
   |   |                             (one publisher per position, locks
   |   |                             at most once). Whether the
   |   |                             predecessor was a skip marker is
   |   |                             remembered for the slot release
   |   |                             below.
   |   |   Retry up to 64 times     If another publisher holds the lock,
   |   |     if locked               reload and retry. The holder will
   |   |                             release quickly (two relaxed stores
   |   |                             + one CAS commit).
   |   |   If unlocked but not       Entry was committed by another
   |   |     at prev_seq             writer. Lock failure path below.
   |   |
   |   | Lock failure:
   |   |   Self-repair:             Steal a provably-stale entry (lock
   |   |     CAS to seq_repair(pos)  value stable across the whole wait,
   |   |     store INVALID_SLOT      or committed >1 wrap behind) and
   |   |     commit seq_skip(pos)    commit it as an EMPTY SKIP MARKER so
   |   |                             the NEXT publisher at this position
   |   |                             succeeds without timeout. The CAS
   |   |                             backs off if a live writer wins.
   |   |                             Do NOT release any slot -- the entry
   |   |                             was never ours. The stale occupant's
   |   |                             unreleased ref is a bounded leak
   |   |                             (1 per steal), recoverable by GC.
   |   |   abandon_delivery():      Count the drop, then release
   |   |     dropped_count++         admission -- this ring's in_flight
   |   |     state_flight.fetch_sub  was incremented at CAS admission and
   |   |       (IN_FLIGHT_ONE, rel.) must be decremented on every exit
   |   |     Dekker wake             path, otherwise the subscriber
   |   |       (fence + has_waiter   destructor spin-waits forever.
   |   |        + futex_wake_all)    Then the SAME wake as the success
   |   |                             path: write_pos already advanced
   |   |                             (the position may now carry a skip
   |   |                             marker), so without the wake a
   |   |                             parked subscriber sleeps out its
   |   |                             full timeout.
   |   |   excess++, continue
   |   |
   |   | Lock success -- release previous occupant:
   |   |   If NOT prev_was_skip     After locking, we own the entry; the
   |   |     and pos >= capacity:   lock-CAS acquire pairs with the
   |   |       read e.slot_idx       previous committer's release, so even
   |   |       if in bounds:         a commit that landed after our wait
   |   |         release_slot(it)    timed out is seen and released here.
   |   |                             INVALID (drain marker) fails the
   |   |                             bound check: drain_unconsumed already
   |   |                             released this ring's reference --
   |   |                             releasing again would double-
   |   |                             decrement. A SKIP predecessor is
   |   |                             never released: its metadata is
   |   |                             untrustworthy by design; the stolen
   |   |                             entry's ref is left for GC. Below
   |   |                             one wrap there is no predecessor
   |   |                             (a zero-initialized slot_idx would
   |   |                             read as valid slot 0).
   |   |
   |   | Theft guard:               If sequence != seq_lock(pos), a
   |   |   reload entry.sequence    repairer proved our lock stale (we
   |   |   if not ours: drop        stalled past commit_timeout) and owns
   |   |                             the entry. Storing data now would
   |   |                             tear the repaired entry.
   |   |
   |   | Write entry fields (relaxed, safe because we hold the lock):
   |   |   entry.slot_idx    = 3
   |   |   entry.payload_len = 128
   |   |
   |   | Phase 2 - CAS commit:
   |   '   CAS entry.sequence       CAS, not a blind store: fails only if
   |         seq_lock(pos) -> 43     a repairer stole the lock after the
   |                                 theft guard -- the entry is then a
   |                                 committed skip marker, and our late
   |                                 slot_idx/payload_len stores landed
   |                                 under tag 01, which nothing ever
   |                                 trusts: permanently harmless. We
   |                                 record a drop (abandon_delivery).
   |                                 Release on success: subscribers and
   |                                 future publishers at this position
   |                                 see all preceding stores.
   |
   |-- state_flight.fetch_sub       Release admission — subscriber
   |     (IN_FLIGHT_ONE, release)   destructor can now observe
   |                                in_flight == 0.
   |
   '-- if has_waiter:               Conditional wake: skip the syscall
         futex_wake_all(write_pos)  when no subscriber is blocking. A
                                    seq_cst fence precedes the has_waiter
                                    load and pairs with one before the
                                    subscriber's futex_wait, ordering the
                                    write_pos fetch_add ahead of the load.
                                    This is a Dekker StoreLoad pair: with
                                    relaxed ordering alone a weakly-ordered
                                    CPU could read has_waiter == 0 stale
                                    and drop the wake to an already-parked
                                    subscriber (lost wakeup until timeout).
                                    x86's locked RMW fences implicitly;
                                    ARM does not, hence the explicit fence.

5. Batch excess: fetch_sub(excess) on slot refcount.
   One atomic RMW for all non-delivered rings, instead of N
   individual decrements. Safe because Free rings have no drain
   to race with, and Draining rings where CAS failed never
   admitted us (in_flight was never incremented).
```

### Why a two-phase commit?

Without the lock, two publishers that CAS `write_pos` to adjacent
positions could interleave their `slot_idx` and `sequence` stores on
overlapping entries (after a ring wrap). The position-tagged lock
prevents this: only one publisher at a time can write an entry's data
fields, and the final CAS commit of the real sequence makes the entry
visible atomically — or fails, detectably, if the lock was stolen.

Subscribers treat any locked value the same as "not yet committed"
and return `nullopt`, so the lock is invisible to them except as a
brief delay.

### Why pre-set refcount before publishing?

If we incremented refcount one ring at a time, a fast eviction on
Ring[1] could drop the slot's refcount to 0 and free it before we've
even published to Ring[2]. Pre-setting to `max_subs` ensures
the slot stays alive for the entire publish loop. Skipped rings
(non-Live) release their reference inline inside the loop.

### What does "evict" mean for refcount?

Eviction decrements by 1, not sets to 0. Each ring holds **one
reference** to the slot. When a ring entry is overwritten, only that
ring's reference is released:

```
Slot[5] refcount = 2   (Ring[0] and Ring[1] both reference it)

Ring[0] wraps -> evicts Slot[5]:
  refcount.fetch_sub(1) -> was 2, now 1
  1 != 0 -> slot stays alive (Ring[1] still references it)

Ring[1] subscriber reads Slot[5]:
  refcount.fetch_sub(1) -> was 1, now 0
  0 -> treiber_push(Slot[5]) back to free stack
```


## Subscribe Flow

Each subscriber reads from its own ring. The read position is
process-local (not in shared memory), so there is no
reader-reader or reader-writer contention on it.

```
Subscriber X (read_pos_ = 41, local)
   |
   v
1. write_pos(42) > read_pos_(41)?   Check for new data.
   Yes -> data available.           No -> return nullopt or futex_wait.
   |
   v
2. entry = entries[41 & mask]        Read the ring entry.
   seq1 = entry.sequence (acquire)
   |
   |  Five outcomes:
   |
   |-- seq1 == expected (42)         Data ready -> proceed to read.
   |
   |-- seq1 is a skip marker         A repair stole this position from a
   |     carrying expected (42)      stalled publisher. Committed but
   |     lost_++                      EMPTY: the metadata is untrustworthy
   |     read_pos_++                  by design and never read. Count one
   |     continue                     lost, retry next entry.
   |
   |-- seq1 > expected (42)          Subscriber fell behind. The entry
   |     lost_++                      was overwritten while we weren't
   |     read_pos_++                  looking. Count one lost, retry next
   |     continue                     entry. (Falling more than a full
   |                                  ring behind is detected earlier,
   |                                  against write_pos, and skipped in
   |                                  bulk.)
   |
   |-- seq1 is a lock value          A publisher is mid-commit on this
   |     return nullopt               entry. Come back later.
   |
   '-- seq1 < expected (42)          Entry not yet committed. A publisher
         return nullopt               claimed this position (write_pos was
                                      incremented) but hasn't stored the
                                      sequence yet. Come back later.
                                      Not a deadlock: if the publisher
                                      crashed, the next publisher at this
                                      position eventually steals the entry
                                      (after commit_timeout) and commits a
                                      skip marker, which the skip path
                                      above consumes.
   |
   v
3. Read slot_idx and payload_len from the entry.
   |
   |---- Both modes: refcount pin --------------------------------|
   |                                                              |
   |  Both try_receive() and try_receive_view() pin the slot      |
   |  via CAS before reading data. This prevents the publisher    |
   |  from freeing the slot while the subscriber reads it.        |
   |                                                              |
   |  CAS Slot.refcount: rc -> rc+1   Pin the slot (only if       |
   |    (retry while rc > 0)          rc > 0, i.e. slot alive)    |
   |    (if rc == 0: slot freed       between seq1 read and       |
   |     between seq1 and now,        now. Count as lost.)        |
   |     skip as lost message)                                    |
   |                                                              |
   |  seq2 = entry.sequence (acquire)  Seqlock validation: if     |
   |  seq2 == seq1?                    the entry was overwritten  |
   |    -> yes: pin valid              after we pinned, the       |
   |    -> no:  undo pin, count lost   slot_idx may be stale.     |
   |                                                              |
   |---- Copy mode: try_receive() --------------------------------|
   |                                                              |
   |  memcpy Slot[slot_idx].data -> local recv_buf_               |
   |  Unpin: refcount.fetch_sub(1)                                |
   |    If refcount -> 0: treiber_push(slot)                      |
   |  read_pos_++                                                 |
   |  return SampleRef { recv_buf_, payload_len }                 |
   |                                                              |
   |  Note: SampleRef points into recv_buf_ (subscriber-local     |
   |  buffer). Calling try_receive() again overwrites it.         |
   |  Copy data from SampleRef before the next call.              |
   |                                                              |
   |---- Zero-copy mode: try_receive_view() ----------------------|
   |                                                              |
   |  read_pos_++                                                 |
   |  return SampleView { Slot, payload_len }                     |
   |    |                                                         |
   |    '--> ~SampleView():                                       |
   |         refcount.fetch_sub(1)                                |
   |         if refcount -> 0: treiber_push(slot)                 |
   |                                                              |
   |  SampleView holds a direct pointer into shared memory.       |
   |  The refcount pin keeps the slot alive until the view        |
   |  is destroyed. Best for large payloads where memcpy          |
   |  would dominate latency.                                     |
   '--------------------------------------------------------------'
```


## Slot Lifecycle

A slot goes through the following states:

```
  FREE (in Treiber stack, refcount = 0)
    │
    │  treiber_pop() by publisher
    ▼
  WRITING (publisher owns it, refcount = 0)
    │
    │  publish(): refcount = max_subs, push index to Live rings
    ▼
  PUBLISHED (referenced by N ring entries and/or SampleViews)
    │
    │  Sources of refcount decrement (one per ring):
    │  - Non-Live ring skip         (publisher releases inline when
    │                                state != Live during admission)
    │  - Ring overflow eviction     (publisher evicts oldest entry)
    │  - drain_unconsumed()         (subscriber destructor releases
    │                                the ring's reference for all
    │                                entries in the live window)
    │
    │  Note: try_receive() pins (rc+1) and unpins (rc-1) the slot
    │  during memcpy — net zero. The ring's original reference is
    │  released later by eviction or drain, not by try_receive().
    │
    │  Additional pin source:
    │  - ~SampleView() destruction  (zero-copy pin released)
    │
    │  Each decrement is fetch_sub(1). Only the one that
    │  transitions refcount from 1 → 0 pushes to the free stack.
    ▼
  refcount hits 0
    │
    │  treiber_push() by whoever did the last fetch_sub
    ▼
  FREE (back in Treiber stack)
```


## Crash Resilience

A publisher can crash at any point during `publish()`. The design
ensures that the channel **never deadlocks**, at the cost of bounded
resource leaks.

### Crash points

```
Crash point                        Consequence
─────────────────────────────────────────────────────────────────────
After treiber_pop, before          Pool slot orphaned (popped but
  refcount pre-set                 never published: refcount 0, off
                                   the free stack, in no ring).
                                   Bounded: 1 slot per crash.
                                   Recovered by reclaim_orphaned_
                                   slots() via its free-stack
                                   membership walk.

After refcount pre-set, during     Refcount was set to max_subs but
  the ring-push loop (delivered    only k out of N rings were visited
  to k of N rings)                 before the crash. Rings visited
                                   before the crash released their
                                   reference (inline for non-Live, or
                                   via eviction/consumption for Live).
                                   Remaining (max_subs - k) references
                                   are never released by normal paths;
                                   the slot is leaked until reclaim_
                                   orphaned_slots() reclaims it (once
                                   the k ring entries have been
                                   consumed or evicted).

After CAS on write_pos, before     Two sub-cases depending on whether
  sequence store (the dangerous    the publisher reached the CAS lock:
  window)
                                   Case A -- crash after CAS lock
                                   (entry stuck at a position-tagged
                                   lock): next publisher at this
                                   position waits commit_timeout,
                                   observes one stable lock value across
                                   the whole wait, steals the entry
                                   (CAS) and commits it as a skip
                                   marker. The external
                                   repair_locked_entries() does the
                                   same with a grace re-check.

                                   Case B -- crash before CAS lock
                                   (entry still at the previous
                                   cycle's committed sequence):
                                   Next publisher at this position
                                   also waits commit_timeout and drops.
                                   repair_locked_entries() detects the
                                   entry is more than one full wrap
                                   behind and skip-commits it.

                                   In both cases the position ends as
                                   a skip marker: the subscriber
                                   counts it as one lost message
                                   without reading the metadata, and
                                   any slot ref the stale entry held
                                   is recovered by
                                   reclaim_orphaned_slots. (The
                                   repair's INVALID_SLOT store is a
                                   diagnostic only -- nothing trusts
                                   metadata under a skip tag.)

After sequence store               No issue. Entry is committed.
                                   Subscribers can read it normally.
```

### Timeout mechanism

When a publisher wraps around to a ring entry that was previously
claimed but never committed, it calls `wait_for_commit()`:

```
wait_for_commit(entry, expected_seq, timeout):
    first = entry.sequence (acquire)
    deadline = now() + timeout
    loop (check clock every 1024 iterations):
        seq = entry.sequence (acquire)
        if seq is not locked and seq_pos(seq) >= expected_seq:
            return {seq, stable_lock: false}      (committed -- a plain
                                                   commit or skip marker)
        if now() >= deadline:
            return {seq, stable_lock: first is locked and seq == first}
```

The function waits out locked entries because another publisher is
mid-commit and will release shortly. `stable_lock` records that ONE
position-tagged lock value was observed at both ends of the full
window: a position is claimed by exactly one publisher and locked at
most once, so this proves a single holder exceeded the commit budget.

On timeout, the publisher:
1. Attempts the CAS lock -- if it fails, the publisher **self-repairs**:
   it steals a provably-stale entry (stable lock, or committed >1 wrap
   behind) with a CAS to `seq_repair(pos)` and commits it as a skip
   marker (`seq_skip(pos)`: tag 01, word carrying pos + 1) so the next
   publisher at this position succeeds without paying the timeout. The
   CAS backs off if a live writer commits first. A stolen-from
   publisher that was merely slow detects the theft at its own theft
   guard or CAS commit and records a drop instead of corrupting the
   entry. Its late metadata stores -- landing at any point after the
   steal -- fall under the skip tag, which nothing ever trusts:
   subscribers count the position lost without reading
   slot_idx/payload_len, and the eviction path never releases a slot
   through a skip-tagged entry. The victim's stores are permanently
   harmless by construction, not by timing.
2. Drops delivery for this ring and moves to the next subscriber ring.
   Every drop path ends in `abandon_delivery()`, which mirrors the
   success path's Dekker wake (seq_cst fence, `has_waiter` check,
   `futex_wake_all`): `write_pos` already advanced, so without the
   wake a parked subscriber would sleep out its full timeout on a
   position that will never plainly commit. The ring resumes normal
   operation on the next wrap.

The timeout is configurable per channel via `channel::Config::commit_timeout`
(default: 10 ms). The tradeoff:

- **Shorter timeout** → faster recovery after a crash, but higher risk
  of falsely evicting a slow-but-alive publisher under heavy scheduling
  pressure (RT preemption, CPU throttling, etc.).
- **Longer timeout** → safer under load, but adds worst-case latency
  whenever a publisher truly crashed mid-commit and a ring wraps to
  the abandoned position.

### How the subscriber recovers

The subscriber never deadlocks either. If a publisher crashes
mid-commit:
1. The subscriber sees `seq < expected` or a locked value
   and returns `nullopt` (data not ready yet)
2. Eventually another publisher wraps to the same position, times
   out on `wait_for_commit`, and either commits its own payload via
   the two-phase commit or steals the entry as a skip marker
3. The subscriber then sees `seq > expected` (skip-ahead path) or a
   skip marker at exactly `expected` (one message lost, metadata
   never read), counts the loss, and resumes

While the head position is claimed-but-uncommitted, a blocking
`receive()` cannot park on the futex: it watches `write_pos`, and
the commit itself produces no futex edge. It polls instead -- a
bounded yield burst (64 spins), then 100 us naps until the entry
commits or the timeout expires -- so a crashed publisher costs naps,
not a hot spin for the whole timeout.

### Leak classes

There are two distinct classes of slot leaks:

```
Class   Cause                              Stuck state
───────────────────────────────────────────────────────────────────────
A       Subscriber destructs while         Ring in Draining/Free state,
        entries remain unconsumed.         entries committed, refcount
        (deactivation race)                never decremented by this ring.

B       Publisher crashes after            Slot refcount inflated;
        treiber_pop or after write_pos     ring entry uncommitted or
        CAS but before sequence store.     slot never published at all.
        (crash leak)
```

**Class A is fully closed** via the `in_flight` quiescence protocol
and full-window drain:

```
~Subscriber():
    1. state = Draining                 — CAS, acq_rel, preserves in_flight;
                                          publishers see non-Live, skip this ring
    2. spin until in_flight == 0        — bounded by commit_timeout
       a) success: quiescence achieved
       b) timeout: publisher likely crashed — skip drain (see below)
    3. if quiesced: drain_unconsumed(ring):
         wp = ring.write_pos            — now guaranteed final
         oldest = max(0, wp - capacity)
         for each entry in [max(oldest, start_pos), wp):
           if sequence == pos + 1:      — committed and not evicted
             slot.refcount--
             if refcount == 0: treiber_push(slot)
             entry.slot_idx = INVALID_SLOT (seq_cst)
           else:
             skip (evicted, uncommitted, or locked — falls into Class B)
    4. state = Free (release)           — ring available for a new subscriber
                                          (timeout path: CAS that preserves
                                          the crashed publisher's in_flight)
```

The key invariant: `in_flight` is incremented by publishers BEFORE
reading `state`, so once `in_flight == 0` after `state = Draining`, no
publisher can be admitted. `write_pos` is truly final.

**Timeout path**: if `in_flight` does not reach 0 within
`commit_timeout`, the destructor does **not** force `in_flight` to 0
and does **not** run `drain_unconsumed()`. Forcing in_flight would
break the quiescence invariant: a slow-but-alive publisher could still
be mid-commit, and drain would race with it, causing double-decrements.
Instead, the destructor skips drain and transitions directly to `Free`.
Leaked slot references are recoverable by the GC paths
(`repair_locked_entries` + `reclaim_orphaned_slots`). A diagnostic
counter `drain_timeouts()` is incremented for observability.

The drain walks `[max(oldest, start_pos), wp)` — not just
`[read_pos, wp)` — because `try_receive()` pins and unpins the slot
(net-zero refcount change), leaving the ring's original reference
(rc=1) on consumed entries. Those entries in `[start_pos, read_pos)`
must also be released. `start_pos` is the `write_pos` captured at
subscriber construction, ensuring a reused ring slot doesn't
double-release entries from a previous subscriber.

After releasing each entry's slot, drain sets `entry.slot_idx` to
`INVALID_SLOT` to prevent a future publisher's eviction from
double-decrementing the refcount.

For `try_receive_view()`, a live `SampleView` holds an extra pin
(rc=2: ring ref + view pin). The drain releases the ring ref (rc→1);
`~SampleView()` releases the pin (rc→0) and pushes to free.

### Leak budget

Only Class B can leak slots. Each publisher crash leaks at most
**2 pool slots**:

- The slot the crashed publisher allocated (refcount stuck > 0 because
  the remaining rings were never visited for inline release; or
  refcount 0 and off the free stack, for a crash before the pre-set)
- The slot referenced by the stolen ring entry, if one existed at the
  wrapped position: the steal deliberately never releases it (the
  stalled holder may already have batch-released this ring's ref, and
  metadata under a skip tag is untrustworthy), so its ring ref leaks
  until GC -- at most one per steal

With a typical pool of 256+ slots, the system can tolerate dozens of
crashes before running low. Class B leaks can be recovered by the
garbage collector (see below).


## Garbage Collection

Publisher crash leaks (Class B) leave pool slots with inflated
refcounts -- or, for a crash in the treiber_pop window, refcount 0
while off the free stack. Since the crashed process is gone, no
normal code path will ever return them to the pool. An explicit
garbage collection pass is needed for long-running systems.

### Design principles

- **On-demand only.** The GC must be triggered explicitly by the user
  (after a known crash, on operator command, or from a health-check
  routine). It never runs automatically or periodically, so it never
  interferes with the hot path.
- **Single caller.** Only one thread/process may run GC at a time.
- **Quiesced or fenced.** The simplest approach runs GC while
  publishers and subscribers are paused. A live-traffic variant is
  possible with snapshot fencing but adds complexity.

### Two separate operations

Recovery is split into two methods with different safety profiles:

**`repair_locked_entries()`** — safe under live traffic.

Scans all ring entries. Stale committed entries (>1 wrap behind,
including stale skip markers) are stolen immediately; locked entries
are collected and re-checked after a full `commit_timeout` grace
sleep -- the same position-tagged lock value at both instants proves
its unique holder exceeded the commit budget. Every steal takes
ownership with a CAS to `seq_repair(pos)` before touching the entry,
then commits the entry as a skip marker (`seq_skip(pos)`: tag 01,
word carrying pos + 1). The `INVALID_SLOT` / zero-length stores made
under the repair lock are diagnostics only -- nothing ever trusts
metadata under a skip tag. A live publisher that commits first wins
the CAS race and the repairer backs off; a stolen-from publisher that
was merely slow detects the theft (theft guard / CAS commit) and
records a drop. Subscribers count a skip-marked position as one lost
message without reading its metadata; evictions never release a slot
through one.

Residuals, both bounded:
- The victim's late metadata stores -- landing at any point after the
  steal -- fall under the skip tag and are ignored by construction,
  so they are harmless; there is no torn-read window.
- The stolen entry's previous slot reference is never released by the
  repair (the stalled holder may already have batch-released this
  ring's ref; releasing again could double-free). At most one slot
  ref leaks per steal, recovered by `reclaim_orphaned_slots()`.

```
repair_locked_entries(region):
    candidates = []
    for each ring i in [0, max_subs):
        for pos in [oldest_live, write_pos):
            seq = entries[pos].sequence
            if seq is locked:
                candidates += {entry, pos, seq}        // grace below
            else if seq_pos(seq) + capacity < pos + 1:
                steal(entry, pos, seq)                 // Case B, CAS-guarded
    if candidates: sleep(commit_timeout)               // grace
    for {entry, pos, seq} in candidates:
        if entry.sequence == seq:                      // same holder spanned it
            steal(entry, pos, seq)

steal(entry, pos, observed):                           // entry_steal_and_clear
    CAS entry.sequence: observed -> seq_repair(pos)    // live writer wins: back off
    entry.slot_idx    = INVALID_SLOT                   // diagnostics only
    entry.payload_len = 0
    entry.sequence    = seq_skip(pos)                  // release: committed, empty
```

**`reclaim_orphaned_slots()`** -- requires full quiescence.

Builds the set of slot indices referenced by plain-committed ring
entries (locked and skip-marked entries are excluded -- skip metadata
is untrustworthy by design), walks the free stack to record
membership (exact under the quiescence contract, bounded by
`pool_size` against corrupt `next_free` cycles), then reclaims every
slot that is neither referenced nor on the stack -- regardless of
refcount. The membership walk is what recovers rc == 0 orphans (a
publisher crash between `treiber_pop` and the refcount pre-set, or a
reclaimer killed between its refcount store and its push) that a
refcount-only scan never could.
NOT safe under live traffic. Requires:
- All publishers quiesced (a publisher between refcount pre-set and
  ring push has rc > 0 but no ring entry yet; one between treiber_pop
  and the pre-set holds an off-stack rc == 0 slot).
- No outstanding `SampleView` objects (a view holds a refcount pin
  without a ring entry reference; reclaiming it would free memory
  still being read).

```
reclaim_orphaned_slots(region):
    referenced = {}
    for each ring i in [0, max_subs):
        for pos in [oldest_live, write_pos):
            seq = entries[pos].sequence
            if seq is plain-committed (tag 00) and seq >= pos + 1:
                referenced.insert(entries[pos].slot_idx)

    on_stack = {}                            // exact under quiescence
    for idx in chain from free_top, bounded by pool_size:
        on_stack.insert(idx)

    for idx in [0, pool_size):
        if idx not in referenced and idx not in on_stack:
            slot[idx].refcount = 0
            treiber_push(free_top, slot[idx], idx)
```

### What this recovers

```
Crash scenario                       GC effect
──────────────────────────────────────────────────────────────────────
After treiber_pop, before publish    Slot has refcount 0, not in any
                                     ring, not in the free stack. The
                                     free-stack membership walk proves
                                     it off-stack, and it appears in no
                                     committed entry -> reclaimed.

After refcount pre-set, delivered    Slot is in k rings but refcount
  to k of N rings                    is max_subs. The k ring references
                                     keep it in the `referenced` set,
                                     so GC cannot blindly reclaim it.
                                     However, once those k entries are
                                     eventually evicted or consumed,
                                     refcount drops to (max_subs - k)
                                     and no ring references remain →
                                     next GC pass reclaims it.

After write_pos CAS, before          Entry is overwritten after
  sequence store                     commit_timeout. The crashed slot's
                                     index may be garbage and won't
                                     appear in any committed entry →
                                     reclaimed on next GC pass.
```

### API

```cpp
// Lightweight health check — read-only, safe under live traffic.
// Call periodically from a supervisor to detect crash damage.
// Note: a single nonzero reading may be a transient state (e.g.,
// Draining ring with publishers finishing). Call twice with a gap
// > commit_timeout; persistent counts indicate a real crash.
auto report = region.diagnose();
// report.locked_entries:  entries holding a position-tagged lock, or
//                         committed >1 wrap stale (incl. stale skip markers)
// report.retired_rings:   Free rings with stale in_flight > 0
// report.draining_rings:  Draining rings with in_flight > 0 (usually transient)
// report.dead_rings:      Live/Draining/Reclaiming rings whose owner is gone
// report.live_rings:      active subscriber rings
// report.schema_stuck:    schema slot at Claiming (advisory; may be a live claim)

// Safe under live traffic — commits poisoned ring entries as skip
// markers (CAS steal after a grace re-check; blocks one commit_timeout
// when locked entries exist). Can be called on a health-check timer.
std::size_t repaired = region.repair_locked_entries();

// Reclaims rings whose owning subscriber process has died. Safe under
// live traffic: only provably-dead owners (pid + start time) are touched.
std::size_t dead = region.reclaim_dead_rings();

// Resets retired rings (Free | in_flight>0) so new subscribers can
// claim them. Only safe after confirming the crashed publisher is gone.
// Deliberate post-crash action, not a routine maintenance call.
std::size_t reset = region.reset_retired_rings();

// Requires full quiescence — reclaims orphaned slots.
std::size_t reclaimed = region.reclaim_orphaned_slots();
```

**`diagnose()`** — read-only scan, safe under live traffic. Returns
counts of locked entries and stuck rings. The supervisor calls this
periodically; persistent nonzero counts signal recovery is needed.

**`repair_locked_entries()`** -- commits provably-stale entries as
skip markers. Safe under live traffic: steals are CAS-guarded and
gated on a commit_timeout grace re-check, so a live publisher always
wins and a slow one detects the theft. Can run on a timer.

**`reclaim_dead_rings()`** -- reclaims `Live`/`Draining`/`Reclaiming`
rings whose owning subscriber process has died, identified by the
`owner_pid` + `owner_starttime` recorded on the ring at claim time
and checked against the OS (same liveness test as
`Registry::sweep_stale`). Safe under live traffic and against
concurrent reclaimers: a single-shot CAS moves the ring to
`Reclaiming`, owner death is re-verified under that exclusivity, then
the ring is freed -- or restored, if the check turned out to have
raced a fresh claim (the restore loop only acts while the state is
still `Reclaiming`, so it never stomps the owner's own teardown). A
slow-but-alive subscriber is never touched.
A reclaimer that crashes mid-pass leaves the ring at `Reclaiming`;
that residue is recovered by a later call (owner dead) or by the live
owner's own teardown.
`in_flight` is preserved (a mid-commit publisher must still
`fetch_sub`), so a reclaimed ring with leftover `in_flight` lands in
the retired state for `reset_retired_rings()` to finish.

**`reset_retired_rings()`** — resets stuck rings (`Free | in_flight>0`
→ `Free | in_flight=0`). Only safe after confirming the crashed
publisher is gone. Unlike `repair_locked_entries()`, this is a
deliberate post-crash action. For a dead *subscriber* prefer
`reclaim_dead_rings()`, which is liveness-checked and cannot underflow
`in_flight` against a slow publisher.

**`reclaim_orphaned_slots()`** -- walks all rings to build a
referenced-slot set and the free stack for membership, then frees any
slot that is neither referenced nor on the stack, regardless of
refcount. NOT safe under live traffic -- requires all publishers
quiesced and no outstanding `SampleView` objects.

### Recommended recovery sequence

```
1. diagnose() → persistent nonzero counts (check twice, gap > commit_timeout)
2. repair_locked_entries()          — safe under live traffic
3. reclaim_dead_rings()             — safe under live traffic (dead subscribers)
4. reset_retired_rings()            — after confirming crashed publisher is gone
5. (optional) pause all publishers
6. reclaim_orphaned_slots()         — requires quiescence
7. resume publishers
```

Steps 5–7 are only needed if the pool is exhausted from leaked slots.
In most cases, steps 2–4 restore the channel to full operation.

### Recovery limits and residual windows

The recovery primitives close the common crash classes but have a few
documented edges, all bounded:

- **`reclaim_orphaned_slots()` mid-pass crash is self-healing.** It
  resets a slot's refcount to 0 and pushes it to the free stack as two
  separate stores. If the *recovering* process is killed between them,
  the slot is left rc == 0, unreferenced, and off the stack -- exactly
  the shape the next run's free-stack membership walk reclaims. No
  permanent leak; just re-run the pass.

- **`reset_retired_rings()` trusts the operator.** It zeroes `in_flight`;
  if called while a misclassified slow-but-alive publisher still owes a
  `fetch_sub`, that decrement underflows the packed counter into the state
  bits. It is a post-crash-only action. For a dead *subscriber*,
  `reclaim_dead_rings()` is the liveness-checked alternative and avoids
  this entirely.

- **Claim-window residual (rings and registry).** Liveness recovery keys on
  an owner identity (`owner_pid` for rings, `pid` for registry entries)
  recorded *after* the claiming CAS. A process that crashes in the few
  instructions between winning the CAS and recording its pid leaves the
  slot owned by pid 0 — unattributable, so neither `reclaim_dead_rings()`
  nor `sweep_stale()` reclaims it. The window is a handful of instructions
  and closing it fully would need a separate intent log; in practice the
  slot is lost only on a crash landing in that exact gap, and the pool /
  registry tolerate many such losses before exhaustion.


## ABA Safety

The [ABA problem](https://en.wikipedia.org/wiki/ABA_problem) is the main
pitfall of lock-free CAS loops: between a thread's read and its CAS, other
threads may change a value away and back, making the CAS succeed on stale
state.

Kickmsg avoids ABA by ensuring that **every CAS target is effectively
monotonic** -- it can never return to a previously observed value:

```
CAS site                Why ABA-safe
────────────────────────────────────────────────────────────────────────────
free_top (Treiber)      64-bit tagged pointer: 32-bit generation counter
                        incremented on every push/pop, packed with the
                        32-bit slot index. Same index + different
                        generation = CAS fails.

write_pos (rings)       Monotonically increasing 64-bit counter. Only goes
                        up, never revisits a value.

state (subscriber)      One-way claim cycle: Free -> Live -> Draining ->
                        Free, plus a Reclaiming detour (Live/Draining ->
                        Reclaiming -> Free, or back) that
                        reclaim_dead_rings holds exclusively while
                        re-verifying owner death.
                        Publishers only deliver to Live rings. The
                        packed state_flight design (state + in_flight
                        in a single uint32) eliminates cross-variable
                        ordering concerns: the publisher's CAS atomically
                        verifies state==Live AND increments in_flight in
                        one operation (acq_rel). No Dekker protocol or
                        seq_cst needed.

refcount (pinning)      CAS from rc to rc+1 only when rc > 0. Even if
                        intermediate transitions bring it back to the same
                        value, the invariant ("slot is alive") still
                        holds -- the operation is idempotent on the safety
                        property.
```

The key principle: **make every CAS target monotonic**, either naturally
(counters that only go up) or artificially (generation tag alongside a
recycled value).


## Portability

ABA safety is a property of the **algorithm**, not the CPU. It relies on
the C++ memory model guarantees for `std::atomic`, which are
architecture-independent.

What varies across architectures is the **cost** of atomic operations:

- **x86-64**: strong memory model. `compare_exchange` compiles to a single
  `LOCK CMPXCHG` instruction. `relaxed` loads/stores are free (no extra
  fences emitted).
- **AArch64 (ARMv8)**: weak memory model. `compare_exchange` uses
  `LDXR`/`STXR` (load-exclusive / store-exclusive) pairs. `acquire`/`release`
  orderings emit `LDAR`/`STLR` variants which carry a small cost compared
  to x86, but remain single instructions -- not full memory barriers.
  `relaxed` loads/stores are free on ARM as well.

Additional supported architectures:

- **RISC-V (RV64)**: weak memory model (RVWMO). Lock-free 64-bit atomics
  via `LR`/`SC` pairs. Acquire/release use `fence` instructions.
  Performance characteristics similar to AArch64.
- **MIPS64**: provides 64-bit `LL`/`SC` for lock-free CAS.

**Excluded**: 32-bit platforms (RV32, MIPS32, ARMv7) lack native 64-bit
atomic operations. The library enforces this at compile time via
`static_assert(std::atomic<uint64_t>::is_always_lock_free)`.

All supported architectures provide native 64-bit atomic CAS on aligned
values, so there is no risk of torn reads. The correctness is portable;
only the per-operation latency differs (by a few nanoseconds).

### Platform Abstraction

kickmsg includes its own minimal OS abstraction layer in `os/`, extracted
from the [KickCAT](https://github.com/leducp/KickCAT) project. It follows
the same conventions and organisation: headers in `include/kickmsg/os/`,
platform-specific implementations in `os/<platform>/`.

```
Abstraction      Header                Linux              macOS              Windows
──────────────────────────────────────────────────────────────────────────────────────
SharedMemory     kickmsg/os/           shm_open           shm_open           CreateFileMapping
                 SharedMemory.h        ftruncate/mmap     ftruncate/mmap     MapViewOfFile
Futex            kickmsg/os/           SYS_futex          __ulock_wait       WaitOnAddress
                 Futex.h               FUTEX_WAIT/_WAKE   __ulock_wake       WakeByAddressAll
Time             kickmsg/os/           clock_nanosleep    nanosleep          QueryPerformanceCounter
                 Time.h                clock_gettime      clock_gettime      Sleep
```

**macOS caveat:** `__ulock_wait` / `__ulock_wake` are private Apple APIs.
The ABI has been stable since macOS 10.12 and is used internally by libc++
and libdispatch, but Apple has not published a formal stability guarantee.

The core engine (`types.h`, `Region.h`, `Publisher.h`, `Subscriber.h`,
`Node.h`) uses only `std::atomic` C++17 and these three abstractions --
no platform `#ifdef` leaks into the messaging logic.

To add a new platform, implement Time, Futex, and SharedMemory in a new
`os/<platform>/` directory and add the sources to `CMakeLists.txt`.


## Why `futex` instead of `mutex` + `condvar`?

Blocking subscribers to wait for new data could be done with a
`pthread_mutex` + `pthread_cond` pair, but `futex` is a better fit:

- **No shared mutex state in the ring.**  A mutex/condvar requires
  initializing a `pthread_mutex_t` + `pthread_cond_t` in shared memory
  with `PTHREAD_PROCESS_SHARED`, which adds complexity and is fragile
  (a crashed process can leave the mutex locked, causing deadlock).
- **Atomic-native.**  `futex` operates directly on the atomic variable
  the subscriber already checks (`write_pos`). The subscriber does
  `if (write_pos == old) futex_wait(&write_pos, old)`. There is no
  separate lock to acquire.
- **No thundering herd in practice.**  The publisher does
  `futex_wake_all` after writing, but each subscriber reads from its
  own ring -- there is no contention on wakeup.
- **Minimal overhead.**  When data is already available, no syscall
  is issued at all (the subscriber's fast path is a single atomic load).
  The `futex` syscall only triggers when the subscriber must actually
  sleep.


## Channel Patterns

All patterns are conventions on top of the same MPMC pool + rings engine.
The backbone does not enforce these constraints; they are established by
the `Node` API which controls how shared-memory regions are named and
how `channel::Config` defaults are set.

```
PubSub (1-to-N)         Broadcast (N-to-N)         Mailbox (N-to-1)
/{prefix}_{topic}       /{prefix}_broadcast_{ch}    /{prefix}_{owner}_mbx_{tag}
                                                    max_subscribers=1
┌─────┐                 ┌─────┐   ┌─────┐          ┌─────┐   ┌─────┐
│Pub A│                 │Pub A│   │Pub B│          │Pub A│   │Pub B│
└──┬──┘                 └──┬──┘   └──┬──┘          └──┬──┘   └──┬──┘
   │                       │         │                │         │
   ▼                       └────┬────┘                └────┬────┘
┌──────┐                       ▼                           ▼
│ Pool │                   ┌──────┐                    ┌──────┐
└──┬───┘                   │ Pool │                    │ Pool │
   │                       └──┬───┘                    └──┬───┘
   ├──▶ Ring[0] → Sub A       ├──▶ Ring[0] → Sub A        │
   ├──▶ Ring[1] → Sub B       ├──▶ Ring[1] → Sub B        └──▶ Ring[0] → Owner
   └──▶ Ring[2] → Sub C       ├──▶ Ring[2] → Sub A(*)
                               └──▶ Ring[3] → Sub B(*)
                           (*) each node is both pub+sub
```

### Topic Naming

Topics are global within a prefix namespace. The publisher's node name
is not part of the shared-memory path for PubSub or Broadcast channels
(it is stored as metadata in the header's `creator_name` field).

```
Node API                           SHM name
──────────────────────────────────────────────────────
advertise("lidar", cfg)            /{prefix}_lidar
subscribe("lidar")                 /{prefix}_lidar
join_broadcast("events", cfg)      /{prefix}_broadcast_events
create_mailbox("reply", cfg)       /{prefix}_{node_name}_mbx_reply
open_mailbox("peer", "reply")      /{prefix}_peer_mbx_reply
```

Mailbox paths include the owner's node name because they are personal
reply channels -- the sender must know who to reply to.

Name components are sanitized into POSIX-shm-safe fragments, and that
mapping is many-to-one ("a:b" and "a b" both become "a_b"); the
"broadcast_" / "_mbx_" infixes are not escaped either, and macOS shm
names are truncated hashes.  Two distinct logical channels can
therefore collide onto one shm name.  To detect this, the Node stamps
`identity_hash` into the header at create time — an FNV-1a chain over
a kind tag ("topic" / "broadcast" / "mailbox"), the namespace, and the
RAW (pre-sanitization) logical coordinates.  Opens verify it when both
sides are nonzero and fail with "Identity mismatch on existing region"
instead of silently sharing the region.  Not part of `config_hash`.


## Registry & Discovery

Kickmsg's core IPC path is decentralized: no broker, no daemon, no
central registry.  Channels are plain shared-memory regions that
producers and consumers find by name.  That works well until an
operator asks *"what's running on this box?"* — which is the job of
the **participant registry**.

### What it is

A single dedicated shared-memory region per namespace, at
`/{namespace}_registry`, holding a fixed-size array of participant
entries.  Each entry records one `(Node, channel, role)` membership.
All scalar fields are `std::atomic<T>` so that the seqlock protocol
between the writer and a concurrent new-tenant claim never involves
plain non-atomic accesses on the same bytes:

- `state` — atomic `Free` / `Claiming` / `Active` / `Reclaiming`
- `generation` — atomic counter bumped on every claim and release (seqlock)
- `pid` — atomic; OS process id of the owner
- `pid_starttime` — atomic; opaque OS-specific process start time
- `channel_type` — atomic; PubSub / Broadcast
- `role` — atomic; Publisher / Subscriber / Both
- `kind` — atomic; Pubsub / Broadcast / Mailbox (registry-level intent tag)
- `topic_name` — user-facing path (leading `/`, interior `/` preserved)
- `shm_name` — implementation-level POSIX path
- `node_name` — the `Node` that registered
- `created_at_ns` — atomic; registration timestamp

A `Node` lazily opens-or-creates the registry on its first
`advertise` / `subscribe` / `join_broadcast` / `create_mailbox` /
`open_mailbox` call, scans for the first `Free` slot, CAS-claims it
(`Free → Claiming`), writes the fields, then release-stores
`Active`.  The `Node`'s destructor deregisters every slot it claimed.

The key property is **cross-platform parity**: Linux `/dev/shm` is
filesystem-visible, but macOS and Windows are not — we can't use
`readdir` to list topics there.  Routing discovery through a regular
`SharedMemory` region means one code path, identical behaviour on
all three targets.

### State machine

```
Free (0) ── CAS ──► Claiming (1) ── release-store ──► Active (2)
   ▲                                                    │
   │                                                    │
   ├────────── deregister: store-release ◄──────────────┤
   │                                                    │
   │            sweep_stale:                            │
   └── CAS(Reclaiming → Free) ◄── CAS(Active | Claiming → Reclaiming)
```

Snapshots acquire-load `state` per slot; only `Active` entries are
returned.  The `Claiming` state is the publication fence for the
field bytes — a reader observing `Active` is guaranteed to see all
the field writes that happened-before the release-store.

`Reclaiming` is the exclusive lock held by `sweep_stale` while it
verifies the dead-pid condition and finalizes the slot to `Free`.
It blocks concurrent registrants (they need `Free → Claiming`) and
prevents ABA on the state CAS: without it, a full `dereg + register`
cycle on another CPU could restore the slot to `Active` between the
sweeper's pid check and its CAS, causing the sweeper to stomp a live
tenant's registration.  `sweep_stale` releases `Reclaiming` back to
the pre-CAS value if the re-verified pid differs from what it
observed (ABA detected), so the live tenant is restored.

### Role upgrade

A `Node` that both advertises and subscribes to the same topic is
recorded as a single entry with `role = Both` rather than two
separate entries.  `Node::touch_registry` detects the existing slot
and upgrades via deregister + re-register.  Upgrades happen at
connect time only — zero hot-path cost.

### Liveness

The registry does not track heartbeats.  A crashed process that
never ran its `Node` destructor leaves its entries stuck at
`Active` (or `Claiming`, if it died mid-register) until reclaimed.
Two recovery paths:

- **Query-time filter**: diagnostic tools probe each entry's pid via
  `process_exists()` and hide dead entries from the user without
  touching the registry.  Non-invasive; safe under live traffic.
- **`Registry::sweep_stale()`**: CAS-resets any `Active` or `Claiming`
  slot whose `pid` is dead.  Opt-in cleanup for an operator or
  supervisor sweep; also called opportunistically from
  `register_participant` when the registry is full, so long-running
  deployments don't silently drop new registrations as crashed-process
  residue accumulates.

The `Claiming` reclaim branch skips slots where `pid == 0`: that state
is the brief window between the `Free→Claiming` CAS and the
registrant's first field store.  Claiming the slot in that window
would stomp a live registrant.  Cost: an early crash (before the pid
store) leaks one slot until the region is unlinked.

**PID-reuse mitigation.**  `pid_starttime` is captured at register
time: `/proc/<pid>/stat` on Linux, `sysctl(KERN_PROC_PID)` on Darwin,
`GetProcessTimes()` on Windows.  Sweep compares the stored starttime
against the live process's starttime; a PID-reuse after wraparound
yields a different value and we reclaim.  If the OS returns 0 (the
process is gone by the time we query), the check degrades to
PID-alone.

### Sizing

Default capacity is 4096 slots × 512 B = 2 MB per namespace.  A robot
telemetry Node typically holds a few hundred topics; the registry is
sized for an order of magnitude of headroom above that.  Exhaustion
is non-fatal: `register_participant` returns `INVALID_SLOT` and the
Node continues to work without a discovery row — registration is a
diagnostic nicety, not a correctness dependency.

### Implicit invariants

Load-bearing assumptions for anyone editing the registry:

- **Field order is ABI.**  `sizeof(ParticipantEntry) == 512` and
  `offsetof(…, _padding) == 368` are statically asserted; any field
  reorder or resize must update the padding and bump `registry::VERSION`.
- **State publication fence.**  `state.store(Active, release)` in
  `register_participant` is the one fence that publishes all field
  writes that preceded it.  `pid` has its own earlier release-store so
  `sweep_stale` can acquire-load it while state is still `Claiming`
  (before the Active fence).  Any new field that needs to be visible
  during `Claiming` must use its own release/acquire pair.
- **Generation bump on every mutation.**  `generation` is bumped by
  `register_participant` *and* by `deregister` *and* by
  `sweep_stale`'s reclaim path.  A snapshot's seqlock recheck detects
  only mutations that bump gen — adding a future write-path that
  modifies fields without bumping gen will cause torn reads.
- **`touch_registry` must never throw.**  `Node::advertise` and friends
  treat registration as best-effort.  A failure is logged once per
  `Node` (latched via `registry_disabled_`) and subsequent calls
  become no-ops.  Don't change this contract without also changing
  `Node::advertise`'s error handling.
- **Role upgrade has a brief visibility gap.**  `touch_registry`
  upgrades Publisher/Subscriber → Both via `deregister` + re-register
  (no atomic "change role in place" primitive).  A concurrent
  `snapshot()` may see this Node absent for ~1 µs during the swap.
  Callers treating an empty `list_topics` result as "definitely
  absent" are wrong; the right reading is "absent or in a brief gap."
- **`Kind` and ring state enums are stringified in two places.**  The
  native binding's `__repr__` methods and the Python `_KIND_NAME` /
  ring-state maps in `diagnostics.py` must stay in sync when a new
  enum value is added.


## Design Tradeoffs

### Silent data loss on slow subscribers

When a subscriber's ring overflows, the publisher silently evicts the
oldest entry to make room. The subscriber only discovers lost messages
after the fact, via the `lost()` counter. This is an intentional design
choice:

- **Non-blocking guarantee.** A fast publisher is never stalled by a slow
  subscriber. This is critical for real-time data where the latest value
  matters more than completeness.
- **Per-subscriber isolation.** Overflow in one ring does not affect other
  subscribers (each ring is independent).
- **No backpressure.** There is no mechanism for a subscriber to signal
  the publisher to slow down. If you need reliable delivery, implement
  acknowledgement and flow control in the upper layer.

The `lost()` counter lets the application detect overflow and act on it
(e.g., log a warning, skip to the latest sample, or resize the ring).

### Pool exhaustion

When the slot pool is empty, `allocate()` returns `nullptr` and
`send()` returns `-EAGAIN`. If the payload exceeds `max_payload_size`,
`send()` returns `-EMSGSIZE`. On success, `send()` returns the number
of bytes written. The publisher must handle errors — typically by
yielding and retrying on `-EAGAIN`, or failing on `-EMSGSIZE`.
No exception is thrown and no slot is leaked.

This happens when all pool slots are in-flight (allocated, published,
but not yet consumed and released by all subscribers). Increasing
`pool_size` or reducing the number of active subscribers alleviates it.

### CAS-lock contention

During the two-phase commit, the publisher CAS-locks the ring entry
before writing data. If another publisher holds the lock, the current
publisher retries up to 64 times. If all retries fail, delivery to
that subscriber ring is silently abandoned — the message is lost for
that subscriber only. The excess refcount adjustment handles the
slot lifecycle correctly.

This only occurs under very high MPMC contention (many publishers
competing for the same ring entry). In practice, the lock is held
for two relaxed stores + one release store (~nanoseconds), so the
64-retry budget is generous.

### Slot leak window on publisher crash (Class B)

If a publisher crashes between `treiber_pop` (slot allocated, refcount=0)
and `refcount.store(max_subs)`, the slot has refcount=0 and is neither
in the free stack nor referenced by any ring entry. No normal code
path will ever touch it again -- but it is not lost:
`reclaim_orphaned_slots()` walks the free stack for membership and
reclaims any slot that is neither referenced nor on the stack,
regardless of refcount. A fresh `SharedRegion::create` (full
reinitialization) also recovers it.

This is a bounded leak between GC passes: at most one slot per
publisher crash in that specific window (a few instructions wide).

**Operational guidance:** if your deployment involves frequent publisher
crashes (e.g. during development, or in a watchdog-restart architecture),
size the pool with enough headroom to absorb the expected number of
orphans between `reclaim_orphaned_slots()` passes or region
recreations. For a pool of 256 slots and a crash rate of one per
hour, the leak is negligible.

### Pool and Ring Sizing

The `pool_size` and `sub_ring_capacity` parameters interact:

- **`sub_ring_capacity`** is the per-subscriber jitter absorption buffer.
  When a subscriber is descheduled, its ring fills up. Once full, new
  messages are dropped for that subscriber regardless of free pool slots.
  At a publish rate of R Hz, a ring of capacity C gives C/R seconds of
  tolerance before loss.

- **`pool_size`** must be at least `sub_ring_capacity * max_subscribers`.
  Each active subscriber can hold up to `sub_ring_capacity` slot
  references (its entire ring window). Pool slots are only freed when
  **all** subscribers have consumed or evicted them (refcount reaches 0).
  With M slow subscribers each holding a full ring window, the pool
  needs M * C slots to avoid starvation. If the pool is too small,
  the publisher exhausts it and `allocate()` fails even when individual
  subscribers have room.

**Sizing rule:** `pool_size >= sub_ring_capacity * max_subscribers`
(hard minimum). In practice, add 2x headroom for bursty traffic:
`pool_size = sub_ring_capacity * max_subscribers * 2`.

The `sub_ring_capacity` is the primary tuning knob:

```
Publish rate    Ring capacity    Tolerance before loss
──────────────────────────────────────────────────────
  1 kHz            64              64 ms
  1 kHz           256             256 ms
 10 kHz            64             6.4 ms
 10 kHz           256              26 ms
100 kHz            64             640 us
100 kHz           256             2.6 ms
```

At 1 kHz (typical control loop), even the default ring capacity of 64
provides 64 ms of scheduling tolerance -- well beyond typical OS jitter
(< 10 ms). Under heavy system stress with `stress-ng`, a ring of 256
recovers ~70% of messages vs ~35% with ring=64 (10 subscribers, burst
publish at full rate).

### Zero-copy pin CAS bound

In `try_receive_view()`, the subscriber CAS-pins a slot by incrementing
its refcount from `rc` to `rc + 1` (retrying while `rc > 0`). Under
contention from M concurrent subscribers pinning the same hot slot,
each CAS may fail and retry. However, the retry count is bounded by M:
every successful CAS by another subscriber represents forward progress,
so this loop cannot livelock.
