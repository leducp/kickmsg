#include "kickmsg/Blackboard.h"

#include <cstring>
#include <stdexcept>

#include "kickmsg/Hash.h"
#include "kickmsg/Naming.h"
#include "kickmsg/os/Futex.h"
#include "kickmsg/os/Process.h"
#include "kickmsg/os/Time.h"


#define KICKMSG_BB_NOINLINE __attribute__((noinline))


namespace kickmsg
{
    namespace
    {
        constexpr uint64_t CELL_MASK = blackboard::CELLS_PER_KEY - 1;

        /// How long ~Writer waits for the board lock.  Bounded by wall clock,
        /// not by yields, because only wall clock bounds how long a destructor
        /// can stall.  Giving up leaves the key owned by this live process
        /// until it exits, and a sweep then reclaims it.
        constexpr nanoseconds RELEASE_LOCK_WAIT = 2s;

        void require_open(void const* base)
        {
            if (base == nullptr)
            {
                throw std::runtime_error("Blackboard is not open");
            }
        }

        /// Copy a value payload.  A reader overtaken by CELLS_PER_KEY writes
        /// races the writer's copy here and discards the result; the payload
        /// race lives in this function alone, and tests/tsan.supp names it.
        KICKMSG_BB_NOINLINE void bb_copy_payload(void* dst, void const* src, std::size_t n)
        {
            std::memcpy(dst, src, n);
        }

        /// Compare a stored key against \p key.  Reader::resolve() runs this
        /// unlocked against claim_free_slot()'s copy_field, the board's second
        /// suppressed race; declare() runs it under the board lock.
        KICKMSG_BB_NOINLINE bool bb_key_equals(char const* stored, char const* key,
                                               std::size_t key_len)
        {
            return ::strnlen(stored, blackboard::KEY_MAX) == key_len
               and std::memcmp(stored, key, key_len) == 0;
        }

        std::size_t stride_for(std::size_t max_value_size)
        {
            return align_up(sizeof(BlackboardCell) + max_value_size, CACHE_LINE);
        }

        std::size_t region_size(uint32_t capacity, std::size_t max_value_size)
        {
            return sizeof(BlackboardHeader)
                 + static_cast<std::size_t>(capacity) * sizeof(BlackboardEntry)
                 + static_cast<std::size_t>(capacity) * blackboard::CELLS_PER_KEY
                   * stride_for(max_value_size);
        }

        /// The value limit is the creator's configured size, never the padded
        /// stride: handing out the alignment slack would let one peer write
        /// more than its correctly-sized readers can hold.
        std::size_t value_capacity(BlackboardHeader const* h)
        {
            return static_cast<std::size_t>(h->max_value_size);
        }

        std::size_t value_stride(BlackboardHeader const* h)
        {
            return stride_for(static_cast<std::size_t>(h->max_value_size));
        }

        void copy_field(char* dst, std::size_t dst_size, char const* src)
        {
            std::memset(dst, 0, dst_size);
            if (src == nullptr)
            {
                return;
            }
            std::size_t n = ::strnlen(src, dst_size - 1);
            std::memcpy(dst, src, n);
        }

        std::string read_field(char const* src, std::size_t size)
        {
            return std::string(src, ::strnlen(src, size));
        }

        void clear_identity(BlackboardEntry* e)
        {
            e->key_hash.store(0, std::memory_order_relaxed);
            e->owner_pid.store(0, std::memory_order_relaxed);
            e->owner_starttime.store(0, std::memory_order_relaxed);
        }

        // Board mutex token: (start-time fingerprint : 32 | pid : 32).
        //
        // Both halves live in one word so the pair can never be read torn: as
        // two fields, a transfer exposes the new holder's pid beside the old
        // holder's start time, which reads as a dead holder.  pid fits in 32
        // bits on all supported platforms.  A fingerprint collision costs a
        // missed reclaim, never a false one -- the pid must match too.

        uint64_t make_token(uint64_t pid, uint64_t starttime)
        {
            uint64_t fp = hash::fnv1a_64(starttime) >> 32;
            if (fp == 0)
            {
                fp = 1;
            }
            return (fp << 32) | (pid & 0xFFFFFFFFull);
        }

        uint64_t token_pid(uint64_t token) { return token & 0xFFFFFFFFull; }

        uint64_t self_token()
        {
            SelfIdentity self = self_identity();
            return make_token(self.pid, self.starttime);
        }

        bool token_holder_dead(uint64_t token)
        {
            uint64_t pid = token_pid(token);
            if (pid == 0)
            {
                return false;
            }
            if (not process_exists(pid))
            {
                return true;
            }
            ProcessProbe live = process_probe(pid);
            // A zombie answers kill(pid, 0) with its recorded start time, so
            // without this an unreaped holder owns the board lock forever.
            if (live.exited)
            {
                return true;
            }
            if (live.starttime == 0)
            {
                // Platform cannot disambiguate: trust pid-alone and treat the
                // holder as alive.
                return false;
            }
            return make_token(pid, live.starttime) != token;
        }

        void repair_board(void* base, BlackboardHeader* h);

        /// Returns false if the wait ran out, which means a live holder.
        /// `budget` bounds yields and `limit` bounds wall clock; zero disables
        /// that bound, and at least one of the two must be set.
        ///
        /// An abandoned lock transfers directly from the dead holder's token
        /// to ours, never through zero, so nothing can slip in mid-repair.
        bool board_lock(void* base, BlackboardHeader* h, uint64_t my_token,
                        int budget, nanoseconds limit)
        {
            nanoseconds start = monotonic_ns();
            for (int attempt = 0; budget == 0 or attempt < budget; ++attempt)
            {
                if (attempt > 0)
                {
                    kickmsg::yield();
                    if (limit != nanoseconds::zero() and elapsed_time(start) >= limit)
                    {
                        return false;
                    }
                }
                uint64_t held = h->lock_token.load(std::memory_order_acquire);
                if (held == 0)
                {
                    uint64_t expected = 0;
                    if (h->lock_token.compare_exchange_strong(
                            expected, my_token,
                            std::memory_order_acq_rel, std::memory_order_relaxed))
                    {
                        return true;
                    }
                    continue;
                }
                // Every thread in a process computes the same token, so a
                // matching token is a peer thread, not re-entry.  Nothing
                // nests, so waiting is correct.
                if (not token_holder_dead(held))
                {
                    continue;
                }
                uint64_t expected = held;
                if (h->lock_token.compare_exchange_strong(
                        expected, my_token,
                        std::memory_order_acq_rel, std::memory_order_relaxed))
                {
                    repair_board(base, h);
                    return true;
                }
            }
            return false;
        }

        void board_unlock(BlackboardHeader* h)
        {
            h->lock_token.store(0, std::memory_order_release);
        }

        /// The only way to hold the board lock: [[nodiscard]] so acquisition
        /// failure cannot be ignored, and the destructor unlocks on every path
        /// including a throw.
        ///
        /// The wait is a yield budget or a wall-clock limit; both forms give
        /// up rather than block forever.
        class [[nodiscard]] BoardGuard
        {
        public:
            /// The token is always the caller's own, so the guard derives it
            /// rather than taking one -- a parameter that can only be passed
            /// one way is a hazard, not a knob.
            BoardGuard(void* base, BlackboardHeader* h, int budget)
                : h_{h}
                , held_{board_lock(base, h, self_token(), budget, nanoseconds::zero())}
            {
            }

            BoardGuard(void* base, BlackboardHeader* h, nanoseconds limit)
                : h_{h}
                , held_{board_lock(base, h, self_token(), 0, limit)}
            {
            }

            ~BoardGuard()
            {
                if (held_)
                {
                    board_unlock(h_);
                }
            }

            BoardGuard(BoardGuard const&)            = delete;
            BoardGuard& operator=(BoardGuard const&) = delete;

            explicit operator bool() const { return held_; }

        private:
            BlackboardHeader* h_;
            bool              held_;
        };

        /// Normalize residue left by a holder that died, or forged by a
        /// corrupt peer.  A Claiming entry never committed and never handed
        /// out a Writer, so it owns nothing and must return to Free --
        /// promoting it would invent an owner and could duplicate a key.
        /// Bring one entry to a valid terminal state, clearing residue left by
        /// a holder that died or forged by a corrupt peer.  Returns true if
        /// anything changed.
        ///
        /// Lock recovery and sweep_stale() must apply exactly the same rules --
        /// a rule added to one and not the other silently diverges them -- so
        /// they share this rather than each carrying a copy.
        bool normalize_entry(BlackboardEntry* e)
        {
            uint32_t st = e->state.load(std::memory_order_acquire);

            if (st == blackboard::Claiming)
            {
                // A claim that never committed never handed out a Writer.
                clear_identity(e);
                e->state.store(blackboard::Free, std::memory_order_release);
                return true;
            }
            if (st == blackboard::Active
                and e->key_hash.load(std::memory_order_relaxed) == 0)
            {
                // Half-finished publish_free().  Active with no key is a
                // phantom: matches no reader, has no owner to sweep.
                clear_identity(e);
                e->state.store(blackboard::Free, std::memory_order_release);
                return true;
            }
            if (st == blackboard::Free
                and (e->key_hash.load(std::memory_order_relaxed) != 0
                     or e->owner_pid.load(std::memory_order_relaxed) != 0))
            {
                // The mirror case: Free published before identity cleared.
                clear_identity(e);
                return true;
            }
            return false;
        }

        void repair_board(void* base, BlackboardHeader* h)
        {
            for (uint32_t i = 0; i < h->capacity; ++i)
            {
                normalize_entry(bb_entry_at(base, h, i));
            }
        }

        /// Return a held entry to Free.  Identity is cleared before the state
        /// is published so a Free entry never carries an owner for the next
        /// claimant to inherit.
        void publish_free(BlackboardEntry* e)
        {
            clear_identity(e);
            e->state.store(blackboard::Free, std::memory_order_release);
        }

        /// An entry is takeable when nobody owns it or its owner is provably
        /// gone.  owner_is_dead() reports false for pid 0, so the unowned case
        /// must be spelled out.
        bool entry_takeable(uint64_t pid, uint64_t starttime)
        {
            return pid == 0 or owner_is_dead(pid, starttime);
        }

        bool key_matches(BlackboardEntry const* e, char const* key, std::size_t key_len)
        {
            return bb_key_equals(e->key, key, key_len);
        }

        /// Fingerprint of the RAW (namespace, name) pair, each component
        /// chained with its length so ("ab","c") and ("a","bc") differ.
        /// Stamped when the caller supplies none, so two logical names that
        /// sanitize to one shm path are rejected instead of sharing a region.
        uint64_t derived_identity(std::string const& kmsg_namespace,
                                  std::string const& name)
        {
            uint64_t h = hash::fnv1a_64(std::string_view("blackboard"),
                                        hash::FNV1A_64_OFFSET_BASIS);
            h = hash::fnv1a_64(std::size_t{10}, h);
            h = hash::fnv1a_64(std::string_view(kmsg_namespace), h);
            h = hash::fnv1a_64(kmsg_namespace.size(), h);
            h = hash::fnv1a_64(std::string_view(name), h);
            h = hash::fnv1a_64(name.size(), h);
            if (h == 0)
            {
                h = 1;
            }
            return h;
        }

        /// key_hash == 0 is the "no key published yet" sentinel, so a real key
        /// must never hash to it.
        uint64_t key_fingerprint(char const* key, std::size_t key_len)
        {
            uint64_t kh = hash::fnv1a_64(std::string_view(key, key_len));
            if (kh == 0)
            {
                kh = 1;
            }
            return kh;
        }

        void notify_change(BlackboardHeader* h)
        {
            h->change_seq.fetch_add(1, std::memory_order_release);
            // Orders the change_seq bump before the waiters load: without it a
            // weakly-ordered CPU reads waiters == 0 stale and a parked reader
            // sleeps to its timeout.  Pairs with wait()'s fence.
            std::atomic_thread_fence(std::memory_order_seq_cst);
            if (h->waiters.load(std::memory_order_relaxed) != 0)
            {
                futex_wake_all(h->change_seq);
            }
        }
    }

    BlackboardEntry* bb_entry_at(void* base, BlackboardHeader const* h, uint32_t idx)
    {
        (void)h;
        auto* bytes = static_cast<uint8_t*>(base) + sizeof(BlackboardHeader);
        return reinterpret_cast<BlackboardEntry*>(bytes + static_cast<std::size_t>(idx) * sizeof(BlackboardEntry));
    }

    BlackboardCell* bb_cell_at(void* base, BlackboardHeader const* h,
                               uint32_t idx, uint64_t parity)
    {
        std::size_t values = sizeof(BlackboardHeader)
                           + static_cast<std::size_t>(h->capacity) * sizeof(BlackboardEntry);
        std::size_t cell = (static_cast<std::size_t>(idx) * blackboard::CELLS_PER_KEY
                            + static_cast<std::size_t>(parity & CELL_MASK))
                         * value_stride(h);
        return reinterpret_cast<BlackboardCell*>(static_cast<uint8_t*>(base) + values + cell);
    }

    uint8_t* bb_cell_payload(BlackboardCell* cell)
    {
        return reinterpret_cast<uint8_t*>(cell) + sizeof(BlackboardCell);
    }

    uint64_t bb_config_hash(blackboard::Config const& cfg)
    {
        uint64_t h = hash::fnv1a_64(cfg.capacity);
        h = hash::fnv1a_64(static_cast<uint64_t>(cfg.max_value_size), h);
        return h;
    }

    // ---- construction ----------------------------------------------------

    Blackboard::Blackboard(Blackboard&& other) noexcept
        : shm_{std::move(other.shm_)}
        , name_{std::move(other.name_)}
        , owner_name_{std::move(other.owner_name_)}
        , base_{other.base_}
        , size_{other.size_}
    {
        other.base_ = nullptr;
        other.size_ = 0;
    }

    Blackboard& Blackboard::operator=(Blackboard&& other) noexcept
    {
        if (this != &other)
        {
            shm_        = std::move(other.shm_);
            name_       = std::move(other.name_);
            owner_name_ = std::move(other.owner_name_);
            base_       = other.base_;
            size_       = other.size_;
            other.base_ = nullptr;
            other.size_ = 0;
        }
        return *this;
    }

    std::string Blackboard::shm_name(std::string const& kmsg_namespace,
                                     std::string const& name)
    {
        return compose_shm_name(
            sanitize_shm_component(kmsg_namespace, "namespace"),
            "bb_" + sanitize_shm_component(name, "blackboard"));
    }

    void Blackboard::init_as_creator(blackboard::Config const& cfg)
    {
        std::size_t bytes = region_size(cfg.capacity, cfg.max_value_size);
        std::memset(base_, 0, bytes);

        auto* h = header();
        h->version        = blackboard::VERSION;
        h->capacity       = cfg.capacity;
        h->max_value_size = cfg.max_value_size;
        h->total_size     = bytes;
        h->config_hash   = bb_config_hash(cfg);
        h->identity_hash = cfg.identity;
        h->creator_pid   = current_pid();
        h->created_at_ns = static_cast<uint64_t>(since_epoch().count());

        // MAGIC published last -- openers spin on it with acquire.
        h->magic.store(blackboard::MAGIC, std::memory_order_release);
    }

    std::optional<Blackboard> Blackboard::spin_open(std::string const& shm,
                                                    blackboard::Config const& cfg,
                                                    bool check_config)
    {
        for (int i = 0; i < 200; ++i)
        {
            SharedMemory mapping;
            if (mapping.try_open(shm))
            {
                if (mapping.size() < sizeof(BlackboardHeader))
                {
                    throw std::runtime_error("Blackboard segment too small: " + shm);
                }
                auto const* h = static_cast<BlackboardHeader const*>(mapping.address());
                if (h->magic.load(std::memory_order_acquire) == blackboard::MAGIC)
                {
                    if (h->version != blackboard::VERSION)
                    {
                        throw std::runtime_error("Blackboard version mismatch on " + shm);
                    }
                    if (h->total_size < sizeof(BlackboardHeader)
                        or h->total_size > mapping.size())
                    {
                        throw std::runtime_error("Blackboard total_size invalid on " + shm);
                    }
                    // capacity and value_stride drive every walk and every
                    // pointer computation; a corrupt value would send
                    // snapshot()/read() off the mapping.  Bound both
                    // (division-based, so no intermediate can overflow).
                    std::size_t after_header = static_cast<std::size_t>(h->total_size)
                                             - sizeof(BlackboardHeader);
                    if (h->capacity == 0 or h->capacity > blackboard::MAX_CAPACITY
                        or h->capacity > after_header / sizeof(BlackboardEntry))
                    {
                        throw std::runtime_error("Blackboard capacity exceeds segment on " + shm);
                    }
                    // Bounding max_value_size (not the derived stride) is what
                    // keeps the accepted range identical on both sides: a board
                    // created at exactly MAX_VALUE_SIZE must stay openable.
                    if (h->max_value_size == 0
                        or h->max_value_size > blackboard::MAX_VALUE_SIZE)
                    {
                        throw std::runtime_error("Blackboard max_value_size invalid on " + shm);
                    }
                    std::size_t entries_bytes = static_cast<std::size_t>(h->capacity)
                                              * sizeof(BlackboardEntry);
                    std::size_t after_entries = after_header - entries_bytes;
                    if (h->capacity > after_entries
                            / (blackboard::CELLS_PER_KEY * value_stride(h)))
                    {
                        throw std::runtime_error("Blackboard value area exceeds segment on " + shm);
                    }
                    if (check_config and h->config_hash != bb_config_hash(cfg))
                    {
                        throw std::runtime_error("Blackboard config mismatch on " + shm);
                    }
                    if (cfg.identity != 0 and h->identity_hash != 0
                        and h->identity_hash != cfg.identity)
                    {
                        throw std::runtime_error("Blackboard identity mismatch on " + shm);
                    }

                    Blackboard out;
                    out.name_ = shm;
                    out.base_ = mapping.address();
                    out.size_ = mapping.size();
                    out.shm_  = std::move(mapping);
                    return out;
                }
            }
            kickmsg::sleep(10ms);
        }
        return std::nullopt;
    }

    Blackboard Blackboard::open_or_create(std::string const& kmsg_namespace,
                                          std::string const& name,
                                          blackboard::Config const& cfg,
                                          char const* owner_name)
    {
        if (cfg.capacity == 0 or cfg.capacity > blackboard::MAX_CAPACITY)
        {
            throw std::invalid_argument("Blackboard capacity out of range");
        }
        if (cfg.max_value_size == 0 or cfg.max_value_size > blackboard::MAX_VALUE_SIZE)
        {
            throw std::invalid_argument("Blackboard max_value_size out of range");
        }

        blackboard::Config stamped = cfg;
        if (stamped.identity == 0)
        {
            stamped.identity = derived_identity(kmsg_namespace, name);
        }

        std::string shm   = shm_name(kmsg_namespace, name);
        std::size_t bytes = region_size(cfg.capacity, cfg.max_value_size);

        {
            Blackboard b;
            b.name_       = shm;
            b.owner_name_ = owner_name;
            if (b.shm_.try_create(shm, bytes))
            {
                b.base_ = b.shm_.address();
                b.size_ = b.shm_.size();
                b.init_as_creator(stamped);
                return b;
            }
        }

        auto opened = spin_open(shm, stamped, true);
        if (opened.has_value())
        {
            opened->owner_name_ = owner_name;
            return std::move(*opened);
        }
        throw std::runtime_error("Timed out waiting for blackboard init: " + shm);
    }

    std::optional<Blackboard> Blackboard::try_open(std::string const& kmsg_namespace,
                                                   std::string const& name)
    {
        std::string  shm = shm_name(kmsg_namespace, name);
        SharedMemory probe;
        if (not probe.try_open(shm))
        {
            return std::nullopt;
        }
        probe.close();

        blackboard::Config expected;
        expected.identity = derived_identity(kmsg_namespace, name);
        auto opened = spin_open(shm, expected, false);
        if (opened.has_value())
        {
            return opened;
        }
        // The object exists but never published MAGIC.  Far past any legitimate
        // creator's init window, so this is a creator that died mid-stamp --
        // reporting it as "absent" would send an operator looking for the wrong
        // problem.
        throw std::runtime_error(
            "Blackboard region exists but was never initialized: " + shm);
    }

    void Blackboard::unlink(std::string const& kmsg_namespace, std::string const& name)
    {
        SharedMemory::unlink(shm_name(kmsg_namespace, name));
    }

    uint32_t Blackboard::capacity() const
    {
        require_open(base_);
        return header()->capacity;
    }

    std::size_t Blackboard::max_value_size() const
    {
        require_open(base_);
        return value_capacity(header());
    }

    uint64_t Blackboard::change_seq() const
    {
        require_open(base_);
        return header()->change_seq.load(std::memory_order_acquire);
    }

    // ---- declare / observe -----------------------------------------------

    Blackboard::Writer Blackboard::declare(char const* key, char const* owner_node)
    {
        require_open(base_);
        if (owner_node == nullptr)
        {
            owner_node = owner_name_.c_str();
        }
        if (key == nullptr or key[0] == '\0')
        {
            throw std::invalid_argument("Blackboard key must not be empty");
        }
        std::size_t key_len = ::strnlen(key, blackboard::KEY_MAX);
        if (key_len >= blackboard::KEY_MAX)
        {
            throw std::invalid_argument("Blackboard key exceeds KEY_MAX");
        }

        auto*    h   = header();
        uint32_t cap = h->capacity;
        uint64_t kh  = key_fingerprint(key, key_len);

        SelfIdentity self     = self_identity();
        uint64_t     my_pid   = self.pid;
        uint64_t     my_start = self.starttime;

        // The entire operation is serialized.  "No two entries hold the same
        // key" spans the whole board, so scanning for the key and claiming a
        // slot must be one indivisible step -- otherwise two claimants can each
        // scan, each see nothing, and each commit.
        BoardGuard guard{base_, h, 4096};
        if (not guard)
        {
            throw std::runtime_error("Blackboard is busy: could not take the board lock");
        }

        // Pass 1: an entry already holds this key.  Taking it over preserves
        // the value and the publish counter, so a restarted writer causes no
        // blackout for readers.
        for (uint32_t i = 0; i < cap; ++i)
        {
            auto* e = bb_entry_at(base_, h, i);
            if (e->state.load(std::memory_order_acquire) != blackboard::Active)
            {
                continue;
            }
            if (e->key_hash.load(std::memory_order_relaxed) != kh)
            {
                continue;
            }
            if (not key_matches(e, key, key_len))
            {
                continue;
            }

            uint64_t pid   = e->owner_pid.load(std::memory_order_relaxed);
            uint64_t start = e->owner_starttime.load(std::memory_order_relaxed);
            if (not entry_takeable(pid, start))
            {
                throw std::runtime_error(
                    std::string("Blackboard key already owned by a live process: ") + key);
            }

            copy_field(e->owner_node, sizeof(e->owner_node), owner_node);
            e->owner_starttime.store(my_start, std::memory_order_relaxed);
            e->owner_pid.store(my_pid, std::memory_order_release);
            uint64_t tenancy = e->tenancy.fetch_add(1, std::memory_order_release) + 1;
            uint64_t writes  = e->publish.load(std::memory_order_acquire) >> 1;

            notify_change(h);
            return Writer(base_, i, tenancy, writes, my_pid, std::string(key, key_len));
        }

        // Pass 2: under the board lock, pass 1 finding nothing is a proof.
        // publish is reset because a fresh reader would otherwise resolve this
        // key and read the previous tenant's bytes.
        auto claim_free_slot = [&]() -> uint32_t
        {
            for (uint32_t i = 0; i < cap; ++i)
            {
                auto* e = bb_entry_at(base_, h, i);
                if (e->state.load(std::memory_order_acquire) != blackboard::Free)
                {
                    continue;
                }

                // Claiming makes a death here recoverable: the next lock
                // holder returns the entry to Free.
                e->state.store(blackboard::Claiming, std::memory_order_release);
                e->publish.store(0, std::memory_order_relaxed);
                copy_field(e->key, sizeof(e->key), key);
                copy_field(e->owner_node, sizeof(e->owner_node), owner_node);
                e->declared_at_ns.store(
                    static_cast<uint64_t>(monotonic_ns().count()),
                    std::memory_order_relaxed);
                e->owner_starttime.store(my_start, std::memory_order_relaxed);
                e->owner_pid.store(my_pid, std::memory_order_relaxed);
                e->key_hash.store(kh, std::memory_order_relaxed);
                // relaxed above, release here: the store of Active is the one
                // fence that publishes every field to a lock-free reader.
                e->tenancy.fetch_add(1, std::memory_order_release);
                e->state.store(blackboard::Active, std::memory_order_release);
                return i;
            }
            return INVALID_SLOT;
        };

        uint32_t claimed = claim_free_slot();
        if (claimed == INVALID_SLOT and sweep_locked(h) > 0)
        {
            // Crash residue can be sitting on the last free slots.
            claimed = claim_free_slot();
        }
        if (claimed != INVALID_SLOT)
        {
            auto* e = bb_entry_at(base_, h, claimed);
            uint64_t tenancy = e->tenancy.load(std::memory_order_acquire);
            notify_change(h);
            return Writer(base_, claimed, tenancy, 0, my_pid, std::string(key, key_len));
        }

        throw std::runtime_error("Blackboard is at capacity");
    }

    Blackboard::Reader Blackboard::observe(char const* key)
    {
        require_open(base_);
        if (key == nullptr or key[0] == '\0')
        {
            throw std::invalid_argument("Blackboard key must not be empty");
        }
        std::size_t key_len = ::strnlen(key, blackboard::KEY_MAX);
        if (key_len >= blackboard::KEY_MAX)
        {
            throw std::invalid_argument("Blackboard key exceeds KEY_MAX");
        }
        return Reader(base_, std::string(key, key_len));
    }

    // ---- Writer ----------------------------------------------------------

    Blackboard::Writer::Writer(void* base, uint32_t entry_idx, uint64_t tenancy,
                               uint64_t writes, uint64_t owner_pid, std::string key)
        : base_{base}
        , entry_idx_{entry_idx}
        , tenancy_{tenancy}
        , writes_{writes}
        , owner_pid_{owner_pid}
        , key_{std::move(key)}
    {
    }

    Blackboard::Writer::~Writer()
    {
        release();
    }

    Blackboard::Writer::Writer(Writer&& other) noexcept
        : base_{other.base_}
        , entry_idx_{other.entry_idx_}
        , tenancy_{other.tenancy_}
        , writes_{other.writes_}
        , owner_pid_{other.owner_pid_}
        , key_{std::move(other.key_)}
    {
        other.base_      = nullptr;
        other.entry_idx_ = INVALID_SLOT;
    }

    Blackboard::Writer& Blackboard::Writer::operator=(Writer&& other) noexcept
    {
        if (this != &other)
        {
            release();
            base_      = other.base_;
            entry_idx_ = other.entry_idx_;
            tenancy_   = other.tenancy_;
            writes_    = other.writes_;
            owner_pid_ = other.owner_pid_;
            key_       = std::move(other.key_);
            other.base_      = nullptr;
            other.entry_idx_ = INVALID_SLOT;
        }
        return *this;
    }

    bool Blackboard::Writer::write(void const* data, std::size_t len)
    {
        if (base_ == nullptr)
        {
            return false;
        }
        // A Writer inherited across fork() is not a claim: the entry belongs to
        // the process that declared it, and tenancy alone cannot tell them apart.
        if (self_identity().pid != owner_pid_)
        {
            return false;
        }
        auto* h = static_cast<BlackboardHeader*>(base_);
        if (len > value_capacity(h) or entry_idx_ >= h->capacity)
        {
            return false;
        }

        auto* e = bb_entry_at(base_, h, entry_idx_);
        if (e->state.load(std::memory_order_acquire) != blackboard::Active)
        {
            return false;
        }
        // relaxed: the acquire load of state above orders this read.
        if (e->tenancy.load(std::memory_order_relaxed) != tenancy_)
        {
            return false;
        }

        uint64_t k    = writes_ + 1;
        auto*    cell = bb_cell_at(base_, h, entry_idx_, k);

        e->publish.store(2 * k - 1, std::memory_order_relaxed);
        // Keeps the write-in-progress store above the payload stores.  A
        // release RMW would NOT do this: it orders prior operations only.
        std::atomic_thread_fence(std::memory_order_release);

        if (len > 0)
        {
            bb_copy_payload(bb_cell_payload(cell), data, len);
        }
        // relaxed: covered by the release fence below and validated by the
        // reader's publish re-check.
        cell->value_len.store(static_cast<uint32_t>(len), std::memory_order_relaxed);
        cell->updated_at_ns.store(
            static_cast<uint64_t>(monotonic_ns().count()), std::memory_order_relaxed);

        std::atomic_thread_fence(std::memory_order_release);
        e->publish.store(2 * k, std::memory_order_relaxed);
        writes_ = k;

        notify_change(h);
        return true;
    }

    void Blackboard::Writer::release()
    {
        if (base_ == nullptr)
        {
            return;
        }
        // ~Writer runs on every exit path in a forked child too; releasing
        // there would hand the parent's key away behind its back.
        if (self_identity().pid != owner_pid_)
        {
            base_      = nullptr;
            entry_idx_ = INVALID_SLOT;
            return;
        }
        auto* h = static_cast<BlackboardHeader*>(base_);
        if (entry_idx_ < h->capacity)
        {
            BoardGuard guard{base_, h, RELEASE_LOCK_WAIT};
            if (guard)
            {
                auto* e = bb_entry_at(base_, h, entry_idx_);
                if (e->state.load(std::memory_order_acquire) == blackboard::Active
                    and e->tenancy.load(std::memory_order_relaxed) == tenancy_)
                {
                    // The entry stays Active holding its value: readers keep
                    // seeing the last state, and a later declare() takes it over.
                    // pid is cleared before start time so a racing liveness
                    // probe never sees (live pid, zeroed start) and concludes
                    // the owner is gone.
                    e->owner_pid.store(0, std::memory_order_relaxed);
                    e->owner_starttime.store(0, std::memory_order_relaxed);
                    e->tenancy.fetch_add(1, std::memory_order_release);
                    notify_change(h);
                }
            }
        }
        base_      = nullptr;
        entry_idx_ = INVALID_SLOT;
    }

    // ---- Reader ----------------------------------------------------------

    Blackboard::Reader::Reader(void* base, std::string key)
        : base_{base}
        , key_hash_{key_fingerprint(key.data(), key.size())}
        , key_{std::move(key)}
    {
    }

    bool Blackboard::Reader::resolve() const
    {
        if (base_ == nullptr)
        {
            return false;
        }
        auto const* h = static_cast<BlackboardHeader const*>(base_);

        if (entry_idx_ < h->capacity)
        {
            auto* e = bb_entry_at(base_, h, entry_idx_);
            if (e->state.load(std::memory_order_acquire) == blackboard::Active
                and e->tenancy.load(std::memory_order_acquire) == tenancy_)
            {
                return true;
            }
        }

        for (uint32_t i = 0; i < h->capacity; ++i)
        {
            auto* e = bb_entry_at(base_, h, i);
            if (e->state.load(std::memory_order_acquire) != blackboard::Active)
            {
                continue;
            }
            // relaxed: the acquire load of state above orders it; a mismatch
            // just costs a skipped candidate.
            if (e->key_hash.load(std::memory_order_relaxed) != key_hash_)
            {
                continue;
            }
            uint64_t tenancy = e->tenancy.load(std::memory_order_acquire);
            // key_hash is only a pre-filter: it cannot survive a collision,
            // so the bytes must actually match.
            if (not bb_key_equals(e->key, key_.data(), key_.size()))
            {
                continue;
            }
            entry_idx_ = i;
            tenancy_   = tenancy;
            return true;
        }

        entry_idx_ = INVALID_SLOT;
        return false;
    }

    blackboard::ReadOutcome Blackboard::Reader::read(void* out, std::size_t cap) const
    {
        blackboard::ReadOutcome result;
        if (base_ == nullptr)
        {
            return result;
        }
        auto*       h     = static_cast<BlackboardHeader*>(base_);
        std::size_t limit = value_capacity(h);

        for (int retry = 0; retry < blackboard::READ_RETRY_BUDGET; ++retry)
        {
            if (not resolve())
            {
                result.status = blackboard::Missing;
                return result;
            }
            auto* e = bb_entry_at(base_, h, entry_idx_);

            uint64_t t1 = e->tenancy.load(std::memory_order_acquire);
            if (t1 != tenancy_)
            {
                continue;
            }
            uint64_t v1 = e->publish.load(std::memory_order_acquire);
            uint64_t k  = v1 >> 1;
            if (k == 0)
            {
                result.status = blackboard::Unset;
                return result;
            }

            auto*    cell = bb_cell_at(base_, h, entry_idx_, k);
            // relaxed: ordered by the acquire load of publish above and
            // validated by the re-check below.
            std::size_t len = cell->value_len.load(std::memory_order_relaxed);
            // A torn or hostile length must never reach the memcpy.
            if (len > limit)
            {
                len = limit;
            }
            bool fits = len <= cap;
            if (fits and len > 0)
            {
                bb_copy_payload(out, bb_cell_payload(cell), len);
            }
            uint64_t stamp = cell->updated_at_ns.load(std::memory_order_relaxed);

            // Load-bearing: an acquire LOAD orders only later accesses, so
            // without this the relaxed cell reads could be satisfied after the
            // re-checks below (cf. read_seqretry's smp_rmb).
            std::atomic_thread_fence(std::memory_order_acquire);
            uint64_t v2 = e->publish.load(std::memory_order_acquire);
            uint64_t t2 = e->tenancy.load(std::memory_order_acquire);

            if (t2 != t1)
            {
                continue;
            }
            // Cell k is clobbered once write k + CELLS_PER_KEY starts.  The
            // unsigned form is exact for both parities of v1 and cannot
            // overflow on a corrupt publish word.
            if (v2 - (v1 & ~1ULL) >= 2 * blackboard::CELLS_PER_KEY - 1)
            {
                continue;
            }

            result.len           = len;
            result.updated_at_ns = stamp;
            result.update_count  = k;
            result.status        = blackboard::Ok;
            if (not fits)
            {
                result.status = blackboard::Truncated;
            }
            return result;
        }

        result.status = blackboard::Busy;
        return result;
    }

    blackboard::ReadOutcome Blackboard::Reader::read(std::vector<uint8_t>& out) const
    {
        for (int attempt = 0; attempt < 4; ++attempt)
        {
            auto result = read(out.data(), out.size());
            if (result.status == blackboard::Truncated)
            {
                out.resize(result.len);
                continue;
            }
            if (result.status == blackboard::Ok)
            {
                out.resize(result.len);
            }
            return result;
        }
        blackboard::ReadOutcome result;
        result.status = blackboard::Busy;
        return result;
    }

    bool Blackboard::Reader::owner_alive() const
    {
        if (not resolve())
        {
            return false;
        }
        auto const* h = static_cast<BlackboardHeader const*>(base_);
        auto*       e = bb_entry_at(base_, h, entry_idx_);
        uint64_t pid = e->owner_pid.load(std::memory_order_acquire);
        if (pid == 0)
        {
            return false;
        }
        return not owner_is_dead(pid, e->owner_starttime.load(std::memory_order_relaxed));
    }

    // ---- wait / snapshot / sweep -----------------------------------------

    bool Blackboard::wait(uint64_t last_seen, nanoseconds timeout)
    {
        require_open(base_);
        auto*       h     = header();
        nanoseconds start = monotonic_ns();

        for (;;)
        {
            if (h->change_seq.load(std::memory_order_acquire) != last_seen)
            {
                return true;
            }
            nanoseconds elapsed = kickmsg::elapsed_time(start);
            if (elapsed >= timeout)
            {
                return false;
            }
            nanoseconds remaining = timeout - elapsed;

            // relaxed: the seq_cst fence below is the ordering edge.
            h->waiters.fetch_add(1, std::memory_order_relaxed);
            // Pairs with notify_change()'s fence: orders our registration
            // before futex_wait's kernel read of change_seq.
            std::atomic_thread_fence(std::memory_order_seq_cst);

            // A writer that bumped before our increment may have seen
            // waiters == 0 and skipped the wake.
            if (h->change_seq.load(std::memory_order_acquire) != last_seen)
            {
                h->waiters.fetch_sub(1, std::memory_order_relaxed);
                return true;
            }
            futex_wait(h->change_seq, last_seen, remaining);
            h->waiters.fetch_sub(1, std::memory_order_relaxed);
        }
    }

    std::vector<blackboard::KeyStatus> Blackboard::snapshot() const
    {
        require_open(base_);
        auto*    h   = const_cast<BlackboardHeader*>(header());
        uint32_t cap = h->capacity;

        std::vector<blackboard::KeyStatus> out;
        std::vector<uint64_t>              starttimes;
        // Reserved before the lock: row allocation is the only thing under
        // the critical section that can throw.
        out.reserve(cap);
        starttimes.reserve(cap);

        // Serialized: a takeover rewrites owner_node with no seqlock over
        // those bytes, so an unlocked listing can return torn text.
        {
            BoardGuard guard{const_cast<void*>(base_), h, 1024};
            if (not guard)
            {
                throw std::runtime_error("Blackboard is busy: could not take the board lock");
            }

            for (uint32_t i = 0; i < cap; ++i)
            {
                auto* e = bb_entry_at(base_, h, i);
                if (e->state.load(std::memory_order_acquire) != blackboard::Active)
                {
                    continue;
                }

                blackboard::KeyStatus ks{};
                bool                  coherent = false;

                // The lock excludes metadata, not writers.  Without the same
                // publish re-check read() does, two writes can wrap onto the
                // chosen cell and pair update_count k with the len/stamp of k+2.
                for (int retry = 0; retry < blackboard::READ_RETRY_BUDGET; ++retry)
                {
                    uint64_t t1 = e->tenancy.load(std::memory_order_acquire);

                    ks.key        = read_field(e->key, sizeof(e->key));
                    ks.owner_node = read_field(e->owner_node, sizeof(e->owner_node));
                    ks.owner_pid  = e->owner_pid.load(std::memory_order_relaxed);

                    uint64_t v1 = e->publish.load(std::memory_order_acquire);
                    uint64_t k  = v1 >> 1;
                    ks.update_count  = k;
                    ks.value_len     = 0;
                    ks.updated_at_ns = 0;
                    if (k > 0)
                    {
                        auto*       cell = bb_cell_at(base_, h, i, k);
                        std::size_t len  = cell->value_len.load(std::memory_order_relaxed);
                        if (len > value_capacity(h))
                        {
                            len = value_capacity(h);
                        }
                        ks.value_len     = len;
                        ks.updated_at_ns = cell->updated_at_ns.load(std::memory_order_relaxed);
                    }

                    std::atomic_thread_fence(std::memory_order_acquire);
                    uint64_t v2 = e->publish.load(std::memory_order_acquire);
                    uint64_t t2 = e->tenancy.load(std::memory_order_acquire);
                    uint32_t s2 = e->state.load(std::memory_order_acquire);

                    if (s2 != blackboard::Active or t1 != t2)
                    {
                        break;
                    }
                    if (k > 0
                        and v2 - (v1 & ~1ULL) >= 2 * blackboard::CELLS_PER_KEY - 1)
                    {
                        continue;
                    }
                    coherent = true;
                    break;
                }
                if (not coherent)
                {
                    continue;
                }

                starttimes.push_back(e->owner_starttime.load(std::memory_order_relaxed));
                out.push_back(std::move(ks));
            }
        }

        // Probed after the lock is dropped: one /proc read per active key,
        // held across a full board, is long enough for a concurrent declare()
        // to burn its yield budget and report a busy board.
        for (std::size_t i = 0; i < out.size(); ++i)
        {
            out[i].owner_alive = out[i].owner_pid != 0
                             and not owner_is_dead(out[i].owner_pid, starttimes[i]);
        }
        return out;
    }

    uint32_t Blackboard::sweep_locked(BlackboardHeader* h)
    {
        uint32_t reclaimed = 0;
        for (uint32_t i = 0; i < h->capacity; ++i)
        {
            auto* e = bb_entry_at(base_, h, i);
            if (normalize_entry(e))
            {
                ++reclaimed;
                continue;
            }

            // Only a clean Free entry or an Active one with a key reaches here.
            uint64_t pid = e->owner_pid.load(std::memory_order_relaxed);
            if (pid == 0)
            {
                // Released on purpose, still holding its value -- not residue.
                continue;
            }
            if (not owner_is_dead(pid, e->owner_starttime.load(std::memory_order_relaxed)))
            {
                continue;
            }
            e->tenancy.fetch_add(1, std::memory_order_relaxed);
            publish_free(e);
            ++reclaimed;
        }
        return reclaimed;
    }

    uint32_t Blackboard::sweep_stale()
    {
        require_open(base_);
        auto* h = header();

        BoardGuard guard{base_, h, 4096};
        if (not guard)
        {
            return 0;
        }

        uint32_t reclaimed = sweep_locked(h);
        if (reclaimed > 0)
        {
            notify_change(h);
        }
        return reclaimed;
    }
}
