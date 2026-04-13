#ifndef KICKMSG_TYPES_H
#define KICKMSG_TYPES_H

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <optional>
#include <type_traits>

namespace kickmsg
{
    using namespace std::chrono;

    static_assert(std::atomic<uint64_t>::is_always_lock_free,
        "Kickmsg requires lock-free 64-bit atomics. "
        "32-bit platforms (RV32, MIPS32) are not supported.");
    static_assert(std::atomic<uint32_t>::is_always_lock_free,
        "Kickmsg requires lock-free 32-bit atomics.");

    constexpr uint64_t    MAGIC           = 0x4B49434B4D534721ULL; // "KICKMSG!"
    constexpr uint32_t    VERSION         = 4;
    constexpr uint32_t    INVALID_SLOT    = UINT32_MAX;
    constexpr uint64_t    LOCKED_SEQUENCE = UINT64_MAX;
    constexpr std::size_t CACHE_LINE      = 64;

    // A healthy commit (memcpy + atomic release-store) finishes in a few
    // microseconds; even under moderate CAS contention it stays well under
    // a millisecond.  10 ms is therefore ~1000× a normal commit — enough
    // to absorb routine preemption without falsely evicting a live
    // publisher, while still recovering from a real crash fast enough to
    // avoid stalling subscribers.  Applications running under severe
    // oversubscription (threads ≫ cores) may want to raise this; hard
    // real-time setups may want to lower it.  Override via
    // channel::Config::commit_timeout.
    constexpr microseconds DEFAULT_COMMIT_TIMEOUT = 10ms;

    /// Optional payload schema descriptor.
    ///
    /// The library never interprets any byte of this structure: it stores it
    /// in the shared-memory header so that multiple processes (possibly built
    /// at different times, from different sources) can agree — or disagree —
    /// on the payload format carried by the channel.
    ///
    /// Policy (which fields to fill, how to compute the hashes, what counts as
    /// a mismatch) is entirely up to the user.  Typical usage:
    ///   - identity: cryptographic or non-cryptographic hash of a canonical
    ///     descriptor of the logical type (name + version + field list).
    ///   - layout: fingerprint of this binary's in-memory layout
    ///     (e.g. a checksum over (offset, size, kind) tuples per member).
    ///     Useful to distinguish "wrong type" from "same type, different ABI".
    ///   - name: human-readable identifier for diagnostics.
    ///   - version: user-defined version number.
    ///   - identity_algo / layout_algo: opaque tags that let the user's tooling
    ///     know which algorithm produced the corresponding bytes (e.g. 1=sha256,
    ///     2=fnv128).  The library never reads them.
    ///
    /// Size is fixed at 512 bytes (8 cache lines) to leave generous room for
    /// future fields without requiring another layout-version bump.
    struct SchemaInfo
    {
        std::array<uint8_t, 64> identity;       ///< Logical fingerprint (user-defined bytes)
        std::array<uint8_t, 64> layout;         ///< Structural fingerprint (user-defined bytes)
        char                    name[128];      ///< Null-terminated, for diagnostics
        uint32_t                version;        ///< User-defined version number
        uint32_t                identity_algo;  ///< User tag: 0 = unspecified
        uint32_t                layout_algo;    ///< User tag: 0 = unspecified
        uint32_t                flags;          ///< Reserved bit flags (0 for now)
        uint8_t                 reserved[240];  ///< Future fields — zero on write
    };
    static_assert(sizeof(SchemaInfo) == 512,
        "SchemaInfo layout is part of the shared-memory ABI");
    static_assert(std::is_trivially_copyable<SchemaInfo>::value,
        "SchemaInfo must be trivially copyable for memcpy into shared memory");

    /// Schema-slot publication state.  Drives a small state machine in the
    /// header so a claim writes the payload bytes between Claiming and Set,
    /// and readers only observe the payload once Set is published.
    namespace schema
    {
        enum State : uint32_t
        {
            Unset    = 0,  ///< No schema has been claimed
            Claiming = 1,  ///< A claim is in progress; payload bytes are being written
            Set      = 2,  ///< Payload is stable and safe to read
        };

        /// Bitmask describing how two SchemaInfo values differ.
        ///
        /// Returned by diff().  Zero (Equal) means all checked fields match.
        /// The library only compares fields with current semantic meaning —
        /// `flags` and `reserved[]` are deliberately excluded so that
        /// forward-compatible additions (a new flag bit, a new field carved
        /// from reserved) do NOT retroactively break existing comparisons.
        ///
        /// The library never decides what counts as a mismatch for the
        /// caller: users combine these bits per their own policy (e.g.
        /// "Identity mismatch is fatal, Version mismatch triggers a
        /// negotiation, Name mismatch is just logged").
        enum Diff : uint32_t
        {
            Equal        = 0,
            Identity     = 1u << 0,  ///< identity[] bytes differ
            Layout       = 1u << 1,  ///< layout[] bytes differ
            Version      = 1u << 2,  ///< version numbers differ
            Name         = 1u << 3,  ///< name strings differ (up to 128 B)
            IdentityAlgo = 1u << 4,  ///< identity_algo tags differ
            LayoutAlgo   = 1u << 5,  ///< layout_algo tags differ
        };

        /// Compute a bitwise diff of the semantically-meaningful fields of
        /// two schema descriptors.  Pure, side-effect free; library does
        /// not apply any mismatch policy.
        uint32_t diff(SchemaInfo const& a, SchemaInfo const& b);
    }

    namespace channel
    {
        enum Type : uint32_t
        {
            PubSub    = 1,
            Broadcast = 2,
        };

        struct Config
        {
            std::size_t max_subscribers   = 16;
            std::size_t sub_ring_capacity = 64;
            std::size_t pool_size         = 256;
            std::size_t max_payload_size  = 4096;

            // Maximum time a publisher waits for a previous writer to commit
            // before assuming it crashed.  Shorter = faster crash recovery but
            // higher risk of falsely evicting a slow-but-alive publisher under
            // heavy scheduling pressure.  Longer = safer under load but adds
            // latency when a real crash occurs.
            microseconds commit_timeout{DEFAULT_COMMIT_TIMEOUT};

            /// Optional schema descriptor baked into the header at create time.
            /// Orthogonal to channel geometry: not included in config_hash, never
            /// enforced by the library.  Users read it back via
            /// SharedRegion::schema() and apply their own mismatch policy.
            std::optional<SchemaInfo> schema;
        };
    }

    // ---- Shared-memory layout structures ----
    //
    // Convention: atomic fields accessed without explicit memory_order
    // (e.g. slot->refcount = 0) are in contexts where ordering is irrelevant
    // (quiesced GC, post-join verification, single-threaded init).
    // Explicit memory_order at all synchronization points makes them
    // visually distinct from incidental reads.

    /// Shared-memory region header. Written once by the creator, read by all.
    /// Layout version changes require a VERSION bump.
    struct Header
    {
        std::atomic<uint64_t> magic;    ///< MAGIC sentinel — written last (release) during init, polled (acquire) by create_or_open
        uint32_t    version;            ///< Layout version — rejects mismatched builds
        channel::Type channel_type;     ///< PubSub or Broadcast

        uint64_t    total_size;         ///< Total shared-memory region size in bytes

        uint64_t    sub_rings_offset;   ///< Byte offset from base to subscriber rings array
        uint64_t    pool_offset;        ///< Byte offset from base to slot pool

        uint64_t    max_subs;           ///< Maximum number of subscriber rings
        uint64_t    sub_ring_capacity;  ///< Entries per subscriber ring (power of 2)
        uint64_t    sub_ring_mask;      ///< sub_ring_capacity - 1 (for fast index masking)
        uint64_t    pool_size;          ///< Number of slots in the pool
        uint64_t    slot_data_size;     ///< Max payload bytes per slot
        uint64_t    slot_stride;        ///< Bytes between consecutive slots (aligned)
        uint64_t    sub_ring_stride;    ///< Bytes between consecutive subscriber rings (aligned)

        uint64_t    commit_timeout_us;  ///< Max wait for a previous writer to commit (crash detection)
        uint64_t    config_hash;        ///< FNV-1a of config fields — detects parameter mismatches on open

        uint64_t    creator_pid;        ///< PID of the process that created the region (debug)
        uint64_t    created_at_ns;      ///< Creation timestamp in nanoseconds since epoch (debug)

        uint16_t    creator_name_len;   ///< Length of creator name string
        // creator_name bytes follow immediately after sizeof(Header)

        /// Payload schema descriptor — opt-in, off the hot path.
        /// Published via a tiny state machine (Unset → Claiming → Set):
        /// writers update schema_data while schema_state == Claiming, then
        /// release-store Set.  Readers acquire-load schema_state and only
        /// read schema_data if the state is Set.
        alignas(CACHE_LINE) std::atomic<uint32_t> schema_state;
        alignas(CACHE_LINE) SchemaInfo            schema_data;

        alignas(CACHE_LINE) std::atomic<uint64_t> free_top; ///< Treiber free-stack head (tagged: gen|idx)
    };

    // The creator_name tail bytes are written at offset sizeof(Header) in the
    // shared-memory mapping.  Guaranteeing sizeof(Header) is a multiple of
    // CACHE_LINE ensures those bytes start on a fresh cache line and never
    // share a line with any atomic field above (schema_state, schema_data,
    // free_top).  The aliasing of alignas(CACHE_LINE) on several members plus
    // struct-level alignment normally produces this automatically, but we
    // assert it to catch accidental layout edits.
    static_assert(sizeof(Header) % CACHE_LINE == 0,
        "Header size must be cache-line multiple to isolate atomic fields "
        "from the creator_name tail written at offset sizeof(Header)");

    /// Ring entry: one per position in a subscriber ring.
    /// Packed to guarantee binary layout across compilers.
    struct Entry
    {
        std::atomic<uint64_t> sequence;     ///< Commit barrier (pos + 1) and seqlock for data consistency
        std::atomic<uint32_t> slot_idx;     ///< Index into the slot pool (INVALID_SLOT if released by drain)
        std::atomic<uint32_t> payload_len;  ///< Actual payload bytes written to the slot
    };

    /// Ring state machine for subscriber lifecycle.
    /// Free → Live (subscriber joins) → Draining (subscriber leaving) → Free
    namespace ring
    {
        enum State : uint32_t
        {
            Free     = 0,  ///< No subscriber — available for claim
            Live     = 1,  ///< Subscriber owns ring, publishers may deliver
            Draining = 2,  ///< Subscriber tearing down — no new delivery, drain in progress
        };

        /// Packed [in_flight:30 | state:2] in a single uint32_t.
        /// Single-variable atomics eliminate cross-variable ordering concerns:
        /// publisher CAS atomically checks state and increments in_flight,
        /// so acquire/release is sufficient (no Dekker protocol, no seq_cst).
        constexpr uint32_t STATE_MASK    = 0x3u;
        constexpr uint32_t IN_FLIGHT_ONE = 0x4u;

        constexpr State    get_state(uint32_t packed)     { return static_cast<State>(packed & STATE_MASK); }
        constexpr uint32_t get_in_flight(uint32_t packed) { return packed >> 2; }
        constexpr uint32_t make_packed(State s, uint32_t in_flight = 0) { return (in_flight << 2) | s; }
    }

    /// Per-subscriber ring header in shared memory.
    /// state_flight packs ring state and in_flight publisher count into one
    /// atomic, enabling single-CAS admission without cross-variable fences.
    /// write_pos and has_waiter share a cache line: the publisher writes
    /// write_pos then reads has_waiter, the subscriber reads write_pos then
    /// writes has_waiter — both access the same line, one cache miss each.
    struct SubRingHeader
    {
        alignas(CACHE_LINE) std::atomic<uint32_t> state_flight; ///< Packed [in_flight:30 | state:2]
        alignas(CACHE_LINE) std::atomic<uint64_t> write_pos;    ///< Monotonically increasing position counter
        std::atomic<uint32_t> has_waiter;                       ///< Set by subscriber before futex_wait
    };

    /// Slot header: prepended to each payload buffer in the pool.
    /// Packed to guarantee binary layout across compilers.
    struct SlotHeader
    {
        std::atomic<uint32_t> refcount;  ///< Number of ring references + SampleView pins
        std::atomic<uint32_t> next_free; ///< Next slot index in the Treiber free-stack chain
    };

    // ---- constexpr helpers (stay in header) ----

    constexpr std::size_t align_up(std::size_t val, std::size_t alignment)
    {
        return (val + alignment - 1) & ~(alignment - 1);
    }

    constexpr bool is_power_of_two(std::size_t n)
    {
        return n > 0 and (n & (n - 1)) == 0;
    }

    // ---- ABA-safe Treiber free-stack (lock-free) ----
    // Tagged pointer: high 32 bits = generation counter, low 32 bits = slot index.

    constexpr uint64_t tagged_pack(uint32_t gen, uint32_t idx)
    {
        return (static_cast<uint64_t>(gen) << 32) | idx;
    }

    constexpr uint32_t tagged_idx(uint64_t tagged) { return static_cast<uint32_t>(tagged); }
    constexpr uint32_t tagged_gen(uint64_t tagged) { return static_cast<uint32_t>(tagged >> 32); }

    SubRingHeader* sub_ring_at(void* base, Header const* h, uint32_t idx);
    Entry*         ring_entries(SubRingHeader* ring);
    SlotHeader*    slot_at(void* base, Header const* h, uint32_t idx);
    SlotHeader*    slot_at(void* pool_base, std::size_t slot_stride, uint32_t idx);
    uint8_t*       slot_data(SlotHeader* slot);
    char*          header_creator_name(Header* h);

    uint64_t compute_config_hash(channel::Type type, channel::Config const& cfg);

    void     treiber_push(std::atomic<uint64_t>& top, SlotHeader* slot, uint32_t slot_idx);
    void     treiber_push(std::atomic<uint64_t>& top, void* pool_base, std::size_t slot_stride, uint32_t slot_idx);
    uint32_t treiber_pop(std::atomic<uint64_t>& top, void* base, Header const* h);
    uint32_t treiber_pop(std::atomic<uint64_t>& top, void* pool_base, std::size_t slot_stride);

}

#endif
