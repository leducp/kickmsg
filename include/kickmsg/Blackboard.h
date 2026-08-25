#ifndef KICKMSG_BLACKBOARD_H
#define KICKMSG_BLACKBOARD_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

#include "kickmsg/types.h"
#include "kickmsg/os/SharedMemory.h"

namespace kickmsg
{
    namespace blackboard
    {
        constexpr uint32_t    VERSION                = 1;
        constexpr uint64_t    MAGIC                  = 0x214B4C424B43494BULL; // "KICKBLK!"
        constexpr std::size_t KEY_MAX                = 128;
        constexpr std::size_t NODE_NAME_MAX          = 64;
        constexpr uint32_t    DEFAULT_CAPACITY       = 256;
        constexpr uint32_t    MAX_CAPACITY           = 65536;
        // 1008 + sizeof(BlackboardCell) lands a cell on exactly 1 KiB.
        constexpr std::size_t DEFAULT_MAX_VALUE_SIZE = 1008;
        constexpr std::size_t MAX_VALUE_SIZE         = 1u << 20;
        constexpr int         READ_RETRY_BUDGET      = 64;

        /// Value cells per key.  The writer always targets the cell readers
        /// are NOT on, so a writer that dies mid-copy corrupts only a cell
        /// nothing reads -- the last good value stays visible and there is no
        /// wedged state to repair.  Power of two: the cell index is a mask of
        /// the write counter, never an index read out of shared memory.
        constexpr uint64_t CELLS_PER_KEY = 2;

        /// Exclusion is the board lock, not a state value: a takeover or a
        /// release never moves the entry out of `Active`.  `Claiming` exists
        /// only to survive its claimant's death -- it tells a recoverer the
        /// entry was mid-claim, and such an entry always returns to `Free`.
        enum KeyState : uint32_t
        {
            Free     = 0,
            Claiming = 1,
            Active   = 2,
        };

        /// A read has several distinguishable outcomes, so it reports a status
        /// rather than collapsing them into a std::optional.
        enum Status : uint32_t
        {
            Ok           = 0,  ///< `len` bytes were copied into the caller's buffer
            Missing      = 1,  ///< No entry for this key
            Unset        = 2,  ///< Key is declared but no value has ever been written
            Truncated    = 3,  ///< Buffer too small; `len` is the true size, nothing copied
            Busy         = 4,  ///< Retry budget exhausted under a very hot writer; transient
            SizeMismatch = 5,  ///< Typed read only: the value is not sizeof(T) bytes
        };

        /// Geometry of a blackboard region.  Only the creator's values are
        /// stamped; an opener's are checked against the stamped config_hash.
        struct Config
        {
            uint32_t    capacity       = DEFAULT_CAPACITY;
            std::size_t max_value_size = DEFAULT_MAX_VALUE_SIZE;

            /// Optional logical-name fingerprint, verified at open when both
            /// sides are nonzero.  Mirrors channel::Config::identity: macOS
            /// PSHMNAMLEN forces compose_shm_name to hash, so two distinct
            /// boards can collide onto one shm object.
            uint64_t    identity       = 0;
        };

        struct ReadOutcome
        {
            Status      status       {Missing};
            std::size_t len          {0};
            uint64_t    updated_at_ns{0};  ///< monotonic_ns of the last write, 0 if never
            uint64_t    update_count {0};
        };

        /// Diagnostic view of one key.  owner_alive costs one OS probe, so
        /// this is a CLI / health-timer structure, never a read-path one.
        struct KeyStatus
        {
            std::string key;
            std::size_t value_len;
            uint64_t    updated_at_ns;
            uint64_t    update_count;
            uint64_t    owner_pid;    ///< 0 when the key holds a value but is unowned
            std::string owner_node;
            bool        owner_alive;
        };
    }

    /// One value cell.  Written between the odd and even `publish` stores and
    /// validated by the same sequence check as the payload, so these can never
    /// describe a different cell than the one copied.  Payload bytes follow
    /// immediately at offset sizeof(BlackboardCell).
    struct BlackboardCell
    {
        std::atomic<uint64_t> updated_at_ns;
        std::atomic<uint32_t> value_len;   ///< clamped to value_capacity by every reader
        std::atomic<uint32_t> _pad0;
    };
    static_assert(sizeof(BlackboardCell) == 16,
        "BlackboardCell layout is part of the blackboard ABI");
    static_assert(std::is_standard_layout<BlackboardCell>::value,
        "BlackboardCell is placed in shared memory via reinterpret_cast");

    /// In-SHM key slot, 384 B.  The guard words share the first cache line so
    /// a read touches one line before the payload.
    ///
    ///   publish = (completed_writes << 1) | write_in_progress
    ///   live cell = (publish >> 1) & (CELLS_PER_KEY - 1)
    ///
    /// A masked parity, never a stored index: a corrupt word cannot address
    /// outside the cells.  `publish` is monotonic for the life of the region --
    /// a takeover continues it instead of resetting, which preserves the dead
    /// owner's value and closes the counter-rewind ABA window.  publish == 0
    /// therefore means "never written", and a zeroed region is a valid board.
    struct BlackboardEntry
    {
        std::atomic<uint32_t> state;            ///< blackboard::KeyState
        std::atomic<uint32_t> _pad0;
        std::atomic<uint64_t> publish;
        std::atomic<uint64_t> tenancy;          ///< bumped on every claim AND release
        std::atomic<uint64_t> owner_pid;        ///< release-stored after owner_starttime
        std::atomic<uint64_t> owner_starttime;
        std::atomic<uint64_t> key_hash;         ///< resolve pre-filter only, never an identity proof
        std::atomic<uint64_t> declared_at_ns;
        uint8_t               _pad1[8];
        char                  key[blackboard::KEY_MAX];              ///< may be unterminated
        char                  owner_node[blackboard::NODE_NAME_MAX]; ///< may be unterminated
        uint8_t               _padding[128];
    };
    static_assert(sizeof(BlackboardEntry) == 384,
        "BlackboardEntry layout is part of the blackboard ABI");
    static_assert(sizeof(BlackboardEntry) % CACHE_LINE == 0,
        "entry stride must keep every entry cache-line aligned");
    static_assert(offsetof(BlackboardEntry, key) == 64,
        "the guard words must occupy exactly the first cache line");
    static_assert(offsetof(BlackboardEntry, _padding) == 256,
        "BlackboardEntry field offsets must match the expected 256 B prefix");
    static_assert(std::is_standard_layout<BlackboardEntry>::value,
        "BlackboardEntry is placed in shared memory via reinterpret_cast");

    /// Entries follow at sizeof(BlackboardHeader), value cells follow the
    /// entries.  Neither offset is stored: every field read from this region
    /// is one a hostile peer can corrupt, so the header carries only what
    /// cannot be derived.
    struct BlackboardHeader
    {
        std::atomic<uint64_t> magic;          ///< written last (release) during init, polled by spin_open
        uint32_t              version;
        uint32_t              capacity;
        /// The configured maximum, exactly as the creator asked for it -- not
        /// the padded cell stride.  The stride is derived from this at open
        /// (stride_for()), so alignment padding is never handed out as extra
        /// payload capacity and never widens the value a peer may write.
        uint64_t              max_value_size;
        uint64_t              total_size;
        uint64_t              config_hash;
        uint64_t              identity_hash; ///< 0 = unstamped
        uint64_t              creator_pid;
        uint64_t              created_at_ns; ///< since_epoch, for display only

        /// change_seq is the futex word: futex_wait/futex_wake_all take an
        /// atomic<uint64_t> and alias its low 32 bits, exactly as
        /// SubRingHeader::write_pos does.  waiters shares the line because
        /// every commit touches both, and both are kept off the header line
        /// above, which every reader loads and no writer dirties.
        alignas(CACHE_LINE) std::atomic<uint64_t> change_seq;
        std::atomic<uint64_t> waiters;       ///< parked readers; a count, not a flag
        uint8_t               _padding[48];

        /// Board mutex, on its own line so a claim never dirties the
        /// notification line every reader polls.  Zero means unlocked.
        ///
        /// Every metadata operation (declare, release, sweep, snapshot)
        /// serializes here: key uniqueness spans the whole array, so no
        /// per-entry lock can enforce it.  The value paths never touch it.
        ///
        /// The token packs pid with a fingerprint of the holder's start time
        /// into one word, so the liveness pair can never be read torn.
        alignas(CACHE_LINE) std::atomic<uint64_t> lock_token;
        uint8_t               _padding2[56];
    };
    static_assert(sizeof(BlackboardHeader) == 3 * CACHE_LINE,
        "BlackboardHeader must be exactly three cache lines");
    static_assert(offsetof(BlackboardHeader, lock_token) == 2 * CACHE_LINE,
        "the board mutex must not share the notification line");
    static_assert(offsetof(BlackboardHeader, magic) == 0,
        "magic offset is a permanent ABI contract across all versions");
    static_assert(offsetof(BlackboardHeader, version) == 8,
        "version offset is a permanent ABI contract across all versions");
    static_assert(offsetof(BlackboardHeader, change_seq) == CACHE_LINE,
        "the notification words must not share the header's read-mostly line");
    static_assert(std::is_standard_layout<BlackboardHeader>::value,
        "BlackboardHeader is placed in shared memory via reinterpret_cast");

    BlackboardEntry* bb_entry_at(void* base, BlackboardHeader const* h, uint32_t idx);
    BlackboardCell*  bb_cell_at(void* base, BlackboardHeader const* h,
                                uint32_t idx, uint64_t parity);
    uint8_t*         bb_cell_payload(BlackboardCell* cell);

    uint64_t bb_config_hash(blackboard::Config const& cfg);

    /// Shared-memory key/value state store.
    ///
    /// A Subscriber attaches at its ring's current write_pos and never sees
    /// anything published before it, so a node broadcasting lifecycle state
    /// must heartbeat forever and a late listener still waits a full period.
    /// A blackboard reader instead observes the current value of every key the
    /// instant it attaches.  State, not stream: writers publish once and stop.
    ///
    /// One region per board at `/{namespace}_bb_{name}`, with its own MAGIC
    /// and blackboard::VERSION -- independent of the channel ABI in types.h.
    /// Persists beyond any single process; remove with unlink().
    ///
    /// Mechanism, not policy.  The library never interprets a value's bytes,
    /// never defines a staleness threshold, and never re-declares a key on a
    /// writer's behalf.  It exposes updated_at_ns, the owner pid, and a change
    /// counter; what counts as "too old" is the caller's.
    ///
    /// Lifetime: Writer and Reader hold raw pointers into the mapping.  They
    /// MUST NOT outlive the Blackboard, and the Blackboard MUST NOT be moved
    /// while any are outstanding.
    class Blackboard
    {
    public:
        Blackboard() = default;
        ~Blackboard() = default;

        Blackboard(Blackboard const&) = delete;
        Blackboard& operator=(Blackboard const&) = delete;

        // Hand-written like SharedRegion's: a defaulted move would leave the
        // source aliasing the destination's live mapping.
        Blackboard(Blackboard&& other) noexcept;
        Blackboard& operator=(Blackboard&& other) noexcept;

        /// `owner_name` labels every key this board declares, for diagnostics.
        /// Node passes its own node name, so a Node-owned board never makes
        /// the caller repeat it.
        static Blackboard open_or_create(std::string const& kmsg_namespace,
                                         std::string const& name,
                                         blackboard::Config const& cfg = {},
                                         char const* owner_name = "");

        /// Returns nullopt when the region does not exist -- for read-only
        /// tools that must not create one as a side effect of inspection.
        /// Throws on magic / version / geometry / identity mismatch.
        /// The mapping is still read/write: "try" means "does not create".
        static std::optional<Blackboard> try_open(std::string const& kmsg_namespace,
                                                  std::string const& name);

        static void unlink(std::string const& kmsg_namespace, std::string const& name);

        static std::string shm_name(std::string const& kmsg_namespace,
                                    std::string const& name);

        /// Exclusive owner of one key.  Move-only: it owns a claim.
        class Writer
        {
        public:
            Writer() = default;

            /// Releases ownership.  The value, its timestamp and its update
            /// count all survive -- late readers keep seeing the last state,
            /// the entry just becomes unowned and a later declare() may take
            /// it over.
            ~Writer();

            Writer(Writer const&) = delete;
            Writer& operator=(Writer const&) = delete;
            Writer(Writer&& other) noexcept;
            Writer& operator=(Writer&& other) noexcept;

            /// Publish a new value.  Returns false when `len` exceeds the
            /// board's max_value_size, when this writer no longer owns the
            /// key (its entry was swept and re-tenanted after a false death
            /// verdict), or when the caller is a fork() child of the declaring
            /// process.  In every case the previous value is untouched.
            bool write(void const* data, std::size_t len);

            /// Typed convenience, gated exactly like hash::fnv1a_64<T>.
            template <typename T>
            auto write(T const& value)
                -> std::enable_if_t<std::is_trivially_copyable_v<T>
                                    and not std::is_pointer_v<T>
                                    and not std::is_null_pointer_v<T>, bool>
            {
                return write(&value, sizeof(T));
            }

            /// Drop ownership now rather than at destruction.  Same
            /// value-preserving semantics as the destructor.  A no-op in a
            /// fork() child, and a no-op if the board lock stays held by a
            /// wedged peer for the whole (bounded) wait -- the key is then
            /// reclaimed when this process exits.
            void release();

            std::string const& key() const { return key_; }

            /// False only for a default-constructed or moved-from Writer.  A
            /// live claim that later loses its entry surfaces through write()
            /// returning false, not through this.
            bool valid() const { return base_ != nullptr; }

        private:
            friend class Blackboard;
            Writer(void* base, uint32_t entry_idx, uint64_t tenancy,
                   uint64_t writes, uint64_t owner_pid, std::string key);

            void*       base_{nullptr};
            uint32_t    entry_idx_{INVALID_SLOT};
            uint64_t    tenancy_{0};
            uint64_t    writes_{0};   ///< sole owner, so this counter lives in the handle
            /// Declaring process.  A Writer inherited across fork() writes and
            /// releases nothing: the claim stays the parent's.
            uint64_t    owner_pid_{0};
            std::string key_;
        };

        /// Declared read interest in one key.  Copyable: it owns nothing.
        class Reader
        {
        public:
            Reader() = default;
            ~Reader() = default;
            Reader(Reader const&) = default;
            Reader& operator=(Reader const&) = default;
            Reader(Reader&&) noexcept = default;
            Reader& operator=(Reader&&) noexcept = default;

            /// Copy the current value into the caller's buffer.  No
            /// allocation, no syscall.
            ///
            /// Always a copy: a writer may overwrite the cell mid-read, so
            /// unlike SampleView's pinned slot there is nothing safe to point
            /// at.  Do not add a zero-copy view.
            blackboard::ReadOutcome read(void* out, std::size_t cap) const;

            /// Typed form.  Reports SizeMismatch when the stored value is not
            /// exactly sizeof(T) bytes -- a uint32_t read as a uint64_t would
            /// otherwise return Ok over a half-filled object.  `out` is
            /// written only on Ok.
            template <typename T>
            auto read(T& out) const
                -> std::enable_if_t<std::is_trivially_copyable_v<T>
                                    and not std::is_pointer_v<T>
                                    and not std::is_null_pointer_v<T>,
                                    blackboard::ReadOutcome>
            {
                alignas(T) unsigned char staging[sizeof(T)];
                blackboard::ReadOutcome result = read(staging, sizeof(T));
                if (result.status != blackboard::Ok)
                {
                    return result;
                }
                if (result.len != sizeof(T))
                {
                    result.status = blackboard::SizeMismatch;
                    return result;
                }
                std::memcpy(&out, staging, sizeof(T));
                return result;
            }

            /// Owning form: resizes `out` to the value length, reusing its
            /// capacity.  Never returns Truncated.
            blackboard::ReadOutcome read(std::vector<uint8_t>& out) const;

            /// Probe whether the key's current owner process still exists.
            /// One OS call -- health-timer scale, not read-path scale.
            bool owner_alive() const;

            std::string const& key() const { return key_; }

        private:
            friend class Blackboard;
            Reader(void* base, std::string key);

            /// Resolves entry_idx_ if it is unset or its tenancy moved.
            /// Returns false when no active entry holds this key.
            bool resolve() const;

            void*            base_{nullptr};
            /// INVALID_SLOT until the key first materializes: observing a key
            /// before its writer exists is a supported use.
            mutable uint32_t entry_idx_{INVALID_SLOT};
            mutable uint64_t tenancy_{0};
            uint64_t         key_hash_{0};
            std::string      key_;
        };

        /// Claim exclusive ownership of `key`.
        ///
        /// Succeeds when the key is free, when it is unowned (a previous
        /// writer released it -- the value carries over untouched), or when
        /// its owner is provably dead (pid + start time): the crash-restart
        /// takeover path.
        ///
        /// Throws std::runtime_error when a live process already owns the key,
        /// the board is at capacity, or the board lock could not be taken; and
        /// std::invalid_argument when `key` is empty or exceeds KEY_MAX.
        /// `owner_node` defaults to the board's owner name; pass one only to
        /// override it.
        Writer declare(char const* key, char const* owner_node = nullptr);

        /// Track `key` for O(1) reads.  Never creates it: a Reader on a key
        /// that does not exist yet reads Missing, and starts returning Ok as
        /// soon as some writer declares and writes it -- no second call.
        Reader observe(char const* key);

        /// Monotonic count of value updates and key claims across this board.
        uint64_t change_seq() const;

        /// Block until change_seq() differs from `last_seen`, or `timeout`
        /// elapses.  Returns true on a change, false on timeout.
        ///
        /// Read change_seq() BEFORE processing, process, then wait on that
        /// value: that is what closes the lost-wakeup window.  One region-wide
        /// wait, not a per-key queue -- a woken reader re-reads whichever keys
        /// it cares about.
        ///
        /// The timeout is mandatory and finite: futex_wait compares only the
        /// low 32 bits of change_seq, so an infinite wait could in principle
        /// miss a wakeup forever.
        bool wait(uint64_t last_seen, nanoseconds timeout);

        /// Diagnostic copy of every active key.  Probes owner liveness, so it
        /// costs one OS call per active key.  Safe under live traffic.
        ///
        /// Serializes against declare/release/sweep: a takeover rewrites
        /// owner_node in place with no seqlock over those bytes.  Throws
        /// std::runtime_error if the board lock cannot be taken.
        std::vector<blackboard::KeyStatus> snapshot() const;

        /// Reclaim crash residue.  Frees keys whose owner process is provably
        /// dead -- destroying their values -- and recovers entries left in a
        /// transient state by a process that died mid-operation.
        /// Returns the number of entries reclaimed, freed and recovered alike.
        ///
        /// Takes the board lock and re-verifies death under it, so it is safe
        /// under live traffic and under concurrent sweepers; a slow-but-alive
        /// writer is never touched.  Returns 0 if the lock is unavailable.
        ///
        /// An operator tool: the normal crash-restart path is declare()'s
        /// takeover, which preserves the value.  declare() calls it itself
        /// before reporting that a claim could not be made.
        uint32_t sweep_stale();

        /// False for a default-constructed or moved-from board.  Every method
        /// that touches the region -- declare, observe, wait, snapshot,
        /// sweep_stale, and the accessors below -- throws std::runtime_error
        /// when this is false.
        bool valid() const { return base_ != nullptr; }

        std::string const& name() const { return name_; }
        uint32_t           capacity() const;
        std::size_t        max_value_size() const;

        BlackboardHeader*       header()       { return static_cast<BlackboardHeader*>(base_); }
        BlackboardHeader const* header() const { return static_cast<BlackboardHeader const*>(base_); }

    private:
        static std::optional<Blackboard> spin_open(std::string const& shm,
                                                   blackboard::Config const& cfg,
                                                   bool check_config);
        void init_as_creator(blackboard::Config const& cfg);

        /// Sweep body, run by a caller that already holds the board lock.
        uint32_t sweep_locked(BlackboardHeader* h);

        SharedMemory shm_;
        std::string  name_;
        std::string  owner_name_;
        void*        base_{nullptr};
        std::size_t  size_{0};
    };
}

#endif
