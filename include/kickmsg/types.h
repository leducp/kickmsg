#ifndef KICKMSG_TYPES_H
#define KICKMSG_TYPES_H

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
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
    constexpr uint32_t    VERSION         = 9;
    constexpr uint32_t    INVALID_SLOT    = UINT32_MAX;
    constexpr std::size_t CACHE_LINE      = 64;

    /// Entry::meta stores slot + 1 in 24 bits; zero means no slot.
    constexpr uint64_t    MAX_POOL_SIZE   = (1ULL << 24) - 2;

    // [tag:2 | pos:62]
    //   00: committed, pos + 1
    //   01: skip marker, pos + 1; no payload, but the slot claim remains valid
    //   10: publisher lock at pos
    //   11: repairer lock at pos
    //
    // Each position has one publisher. An unchanged lock across the grace
    // period can be stolen. Readers count skip markers as lost samples.
    constexpr uint64_t SEQ_LOCK_BIT   = 1ULL << 63;
    constexpr uint64_t SEQ_REPAIR_BIT = 1ULL << 62;

    constexpr bool     seq_is_locked(uint64_t seq) { return (seq & SEQ_LOCK_BIT) != 0; }
    constexpr uint64_t seq_lock(uint64_t pos)      { return SEQ_LOCK_BIT | pos; }
    constexpr uint64_t seq_repair(uint64_t pos)    { return SEQ_LOCK_BIT | SEQ_REPAIR_BIT | pos; }
    constexpr bool     seq_is_skip(uint64_t seq)
    {
        return (seq & (SEQ_LOCK_BIT | SEQ_REPAIR_BIT)) == SEQ_REPAIR_BIT;
    }
    constexpr uint64_t seq_skip(uint64_t pos)      { return SEQ_REPAIR_BIT | (pos + 1); }
    constexpr uint64_t seq_pos(uint64_t seq)       { return seq & (SEQ_REPAIR_BIT - 1); }

    // Override via channel::Config::commit_timeout. Increase under heavy
    // scheduling delays to reduce recovery of slow but live publishers.
    constexpr microseconds DEFAULT_COMMIT_TIMEOUT = 10ms;

    /// Opaque payload schema, stored in shared memory. Callers choose hashes,
    /// algorithm tags, versioning, and compatibility rules.
    /// identity names the logical type; layout describes its binary layout.
    /// The fixed 512-byte layout includes reserved space for future fields.
    struct SchemaInfo
    {
        std::array<uint8_t, 64> identity;       ///< Logical fingerprint (user-defined bytes)
        std::array<uint8_t, 64> layout;         ///< Structural fingerprint (user-defined bytes)
        char                    name[128];      ///< Null-terminated, for diagnostics
        uint32_t                version;        ///< User-defined version number
        uint32_t                identity_algo;  ///< User tag: 0 = unspecified
        uint32_t                layout_algo;    ///< User tag: 0 = unspecified
        uint32_t                flags;          ///< Reserved bit flags (0 for now)
        uint8_t                 reserved[240];  ///< Future fields -- zero on write
    };
    static_assert(sizeof(SchemaInfo) == 512,
        "SchemaInfo layout is part of the shared-memory ABI");
    static_assert(std::is_trivially_copyable<SchemaInfo>::value,
        "SchemaInfo must be trivially copyable for memcpy into shared memory");

    /// Writers fill schema_data while Claiming and release-store Set.
    /// Readers access it only after acquiring Set.
    namespace schema
    {
        enum State : uint32_t
        {
            Unset    = 0,  ///< No schema has been claimed
            Claiming = 1,  ///< A claim is in progress; payload bytes are being written
            Set      = 2,  ///< Payload is stable and safe to read
        };

        /// Fields that differ between two schemas. Zero means all checked fields match.
        /// Flags and reserved bytes are ignored for forward compatibility.
        /// The caller decides which differences are acceptable.
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

        /// Compare schema fields without applying a compatibility policy.
        uint32_t diff(SchemaInfo const& a, SchemaInfo const& b);
    }

    namespace channel
    {
        /// `None` carries no ring geometry and is never stamped into a
        /// Header -- validate_header_geometry rejects it.  It exists so a
        /// registry row can describe a participant that has no channel at
        /// all (a Blackboard).
        enum Type : uint32_t
        {
            None      = 0,
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

            /// Optional logical-identity fingerprint, verified at open when
            /// both sides are nonzero (shm-name collision detection).  NOT
            /// part of config_hash.
            uint64_t identity = 0;
        };
    }


    /// Shared-memory region header. Written once by the creator, read by all.
    /// Layout version changes require a VERSION bump.
    struct Header
    {
        std::atomic<uint64_t> magic;    ///< MAGIC sentinel -- written last (release) during init, polled (acquire) by create_or_open
        uint32_t    version;            ///< Layout version -- rejects mismatched builds
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
        uint64_t    config_hash;        ///< FNV-1a of config fields -- detects parameter mismatches on open

        uint64_t    creator_pid;        ///< PID of the process that created the region (debug)
        uint64_t    created_at_ns;      ///< Creation timestamp in nanoseconds since epoch (debug)

        uint16_t    creator_name_len;   ///< Length of creator name string
        // creator_name bytes follow immediately after sizeof(Header)

        /// Write schema_data under Claiming, then release-store Set.
        /// Readers acquire Set before copying schema_data.
        alignas(CACHE_LINE) std::atomic<uint32_t> schema_state;
        alignas(CACHE_LINE) SchemaInfo            schema_data;

        alignas(CACHE_LINE) std::atomic<uint64_t> free_top; ///< Treiber free-stack head (tagged: gen|idx)
        std::atomic<uint64_t> steal_count;  ///< Entries stolen from a stalled publisher
        uint64_t              identity_hash; ///< Logical-identity fingerprint, written once pre-MAGIC (0 = unstamped); detects shm-name collisions at open
    };

    // The creator name follows Header and must not share a cache line
    // with its atomics.
    static_assert(sizeof(Header) % CACHE_LINE == 0,
        "Header size must be cache-line multiple to isolate atomic fields "
        "from the creator_name tail written at offset sizeof(Header)");

    // Keep magic and version at fixed offsets across all ABI versions
    // so incompatible mappings can be rejected.
    static_assert(std::is_standard_layout<Header>::value,
        "Header is placed in shared memory via reinterpret_cast");
    static_assert(offsetof(Header, magic) == 0,
        "magic offset is a permanent ABI contract across all versions");
    static_assert(offsetof(Header, version) == 8,
        "version offset is a permanent ABI contract across all versions");

    // [tag:40 | slot + 1:24]; tag is the low 40 bits of pos + 1.
    // Zero in the slot field means no claim. Each claim owns one slot reference.
    // Replacing a claim transfers the duty to release its reference.
    // Publishers CAS only older position tags, preventing late writes from
    // overwriting newer entries.
    constexpr uint64_t META_SLOT_BITS = 24;
    constexpr uint64_t META_SLOT_MASK = (1ULL << META_SLOT_BITS) - 1;
    constexpr uint64_t META_TAG_MASK  = (1ULL << 40) - 1;

    constexpr uint64_t meta_tag(uint64_t m) { return m >> META_SLOT_BITS; }

    /// Biased slot field: 0 means the entry names no slot (also the value a
    /// freshly zeroed region carries, which must not read as slot 0).
    constexpr uint32_t meta_slot_biased(uint64_t m)
    {
        return static_cast<uint32_t>(m & META_SLOT_MASK);
    }

    constexpr uint64_t meta_pack(uint64_t pos, uint32_t slot_idx)
    {
        uint64_t tag = (pos + 1) & META_TAG_MASK;
        return (tag << META_SLOT_BITS)
             | ((static_cast<uint64_t>(slot_idx) + 1) & META_SLOT_MASK);
    }

    /// True if m precedes pos under 40-bit serial-number ordering.
    /// Requires positions to be less than 2^39 apart.
    constexpr bool meta_precedes(uint64_t m, uint64_t pos)
    {
        uint64_t diff = (meta_tag(m) - ((pos + 1) & META_TAG_MASK)) & META_TAG_MASK;
        return diff != 0 and (diff & (1ULL << 39)) != 0;
    }

    /// Validated local copy of geometry used for pointer arithmetic.
    /// Shared header fields remain writable by peers after validation.
    struct Geometry
    {
        uint64_t sub_rings_offset;
        uint64_t sub_ring_stride;
        uint64_t sub_ring_capacity;
        uint64_t sub_ring_mask;
        uint64_t pool_offset;
        uint64_t slot_stride;
        uint64_t pool_size;
        uint64_t slot_data_size;
        uint64_t max_subs;
        uint64_t commit_timeout_us;
    };

    /// Ring entry: one per position in a subscriber ring.
    /// Packed to guarantee binary layout across compilers.
    struct Entry
    {
        std::atomic<uint64_t> sequence;  ///< Commit barrier (pos + 1) and seqlock for data consistency
        std::atomic<uint64_t> meta;      ///< Slot claim; see the meta-word encoding above
    };
    static_assert(sizeof(Entry) == 16 and std::is_standard_layout<Entry>::value,
        "Entry layout drives cross-process ring-stride math");

    /// Ring state machine for subscriber lifecycle.
    /// Free -> Live (subscriber joins) -> Draining (subscriber leaving) -> Free
    namespace ring
    {
        enum State : uint32_t
        {
            Free       = 0,  ///< No subscriber -- available for claim
            Live       = 1,  ///< Subscriber owns ring, publishers may deliver
            Draining   = 2,  ///< Subscriber tearing down -- no new delivery, drain in progress
            Reclaiming = 3,  ///< reclaim_dead_rings() holds the ring exclusively while re-verifying owner death
        };

        /// Packed [in_flight:30 | state:2]. One CAS checks Live and admits a publisher.
        constexpr uint32_t STATE_MASK    = 0x3u;
        constexpr uint32_t IN_FLIGHT_ONE = 0x4u;

        constexpr State    get_state(uint32_t packed)     { return static_cast<State>(packed & STATE_MASK); }
        constexpr uint32_t get_in_flight(uint32_t packed) { return packed >> 2; }
        constexpr uint32_t make_packed(State s, uint32_t in_flight = 0) { return (in_flight << 2) | s; }

        enum Waiter : uint32_t
        {
            WaiterNone    = 0,  ///< Nobody waiting -- no wake syscall
            WaiterFutex   = 1,  ///< Waiting in futex_wait on write_pos
            WaiterCarrier = 2,  ///< Waiting on a WakeBackend descriptor
        };
    }

    /// Per-subscriber ring header. state_flight combines state and publisher
    /// admission count. Hot counters share a line; separate rings use distinct lines.
    struct SubRingHeader
    {
        alignas(CACHE_LINE) std::atomic<uint32_t> state_flight; ///< Packed [in_flight:30 | state:2]
        std::atomic<uint64_t> owner_pid;                        ///< Claiming subscriber's pid; 0 when Free (liveness recovery)
        std::atomic<uint64_t> owner_starttime;                  ///< owner_pid's start time; pid-reuse guard for reclaim_dead_rings
        alignas(CACHE_LINE) std::atomic<uint64_t> write_pos;    ///< Monotonically increasing position counter
        std::atomic<uint32_t> has_waiter;                       ///< ring::Waiter, set by the subscriber before it waits
        std::atomic<uint64_t> dropped_count;                    ///< Cumulative publisher drops on this ring (all publishers)
        std::atomic<uint64_t> lost_count;                       ///< Cumulative subscriber losses on this ring (all subscribers)
    };
    // Owner fields use state_flight's padding without changing the two-line layout.
    static_assert(sizeof(SubRingHeader) == 2 * CACHE_LINE,
        "SubRingHeader must stay 2 cache lines -- expanding it past the "
        "write_pos line padding requires reconsidering ring-stride math in Region.cc");

    /// Slot header: prepended to each payload buffer in the pool.
    /// Packed to guarantee binary layout across compilers.
    struct SlotHeader
    {
        std::atomic<uint32_t> refcount;  ///< Number of ring references + SampleView pins
        std::atomic<uint32_t> next_free; ///< Next slot index in the Treiber free-stack chain
        /// Length written before publication and read under a validated slot pin.
        std::atomic<uint32_t> payload_len;
        uint32_t              _padding;
    };
    static_assert(sizeof(SlotHeader) == 16 and std::is_standard_layout<SlotHeader>::value,
        "SlotHeader layout drives cross-process slot-stride math");
    static_assert(std::is_standard_layout<SubRingHeader>::value,
        "SubRingHeader is placed in shared memory via reinterpret_cast");

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
    SubRingHeader* sub_ring_at(void* base, Geometry const& g, uint32_t idx);

    /// Clear owner and wake mode before publishing Free, while no replacement
    /// can claim the ring.
    void clear_owner(SubRingHeader* ring);
    Entry*         ring_entries(SubRingHeader* ring);
    SlotHeader*    slot_at(void* base, Header const* h, uint32_t idx);
    SlotHeader*    slot_at(void* base, Geometry const& g, uint32_t idx);
    SlotHeader*    slot_at(void* pool_base, std::size_t slot_stride, uint32_t idx);
    uint8_t*       slot_data(SlotHeader* slot);
    char*          header_creator_name(Header* h);

    uint64_t compute_config_hash(channel::Type type, channel::Config const& cfg);

    /// CAS a stale observed sequence to a repair lock, then publish a skip at pos.
    /// Returns false if the sequence changed. Keeps Entry::meta and its reference
    /// for the next publisher or drainer to release.
    bool entry_steal_and_skip(Entry& e, uint64_t pos, uint64_t observed);

    void     treiber_push(std::atomic<uint64_t>& top, SlotHeader* slot, uint32_t slot_idx);
    void     treiber_push(std::atomic<uint64_t>& top, void* pool_base, std::size_t slot_stride, uint32_t slot_idx);
    uint32_t treiber_pop(std::atomic<uint64_t>& top, void* base, Header const* h);
    uint32_t treiber_pop(std::atomic<uint64_t>& top, void* base, Geometry const& g);
    /// Return INVALID_SLOT if a shared free-list index is outside pool_size.
    uint32_t treiber_pop(std::atomic<uint64_t>& top, void* pool_base, std::size_t slot_stride,
                         uint64_t pool_size);

}

#endif
