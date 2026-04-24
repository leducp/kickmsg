#ifndef KICKMSG_REGISTRY_H
#define KICKMSG_REGISTRY_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "kickmsg/types.h"
#include "kickmsg/os/SharedMemory.h"

namespace kickmsg
{
    namespace registry
    {
        constexpr uint32_t VERSION          = 3;
        constexpr uint64_t MAGIC            = 0x214745524B43494BULL; // "KICKREG!"
        // Supports up to ~200-400 topics with a few participants each,
        // plus headroom for transient tasks.  4096 × 512 B = 2 MB per
        // namespace.
        constexpr uint32_t DEFAULT_CAPACITY = 4096;
        constexpr std::size_t SHM_NAME_MAX   = 128;
        constexpr std::size_t TOPIC_NAME_MAX = 128;
        constexpr std::size_t NODE_NAME_MAX  = 64;

        enum Role : uint32_t
        {
            Publisher  = 1,
            Subscriber = 2,
            Both       = 3,  ///< Node is both producer and consumer on this channel
        };

        /// What the channel is used for, from the user-facing API's point
        /// of view.  channel_type (in types.h) is the low-level ring
        /// geometry (PubSub vs Broadcast); Kind distinguishes Mailbox
        /// from PubSub even though both share channel::PubSub geometry.
        enum Kind : uint32_t
        {
            Pubsub    = 1,
            Broadcast = 2,
            Mailbox   = 3,
        };

        /// Only `Active` slots are visible to snapshot readers.
        /// `Reclaiming` is the exclusive lock held by `sweep_stale` to
        /// prevent ABA on the state CAS.
        enum SlotState : uint32_t
        {
            Free       = 0,
            Claiming   = 1,
            Active     = 2,
            Reclaiming = 3,
        };
    }

    /// In-SHM entry, 512 B.  Readers go through `Registry::snapshot()`.
    /// Scalar fields are atomic so a snapshot reader racing with a new-
    /// tenant writer never hits a C++ data race; the seqlock (generation
    /// + state) discards torn copies.  Do not reorder fields without
    /// bumping `registry::VERSION`.
    struct ParticipantEntry
    {
        std::atomic<uint32_t> state;
        std::atomic<uint32_t> channel_type;
        std::atomic<uint32_t> role;
        std::atomic<uint32_t> kind;
        std::atomic<uint32_t> generation;     ///< seqlock version, bumped on every mutation
        std::atomic<uint64_t> pid;            ///< release/acquire-accessed; inspected while state==Claiming
        std::atomic<uint64_t> pid_starttime;  ///< OS-reported start time, or 0 if unavailable
        std::atomic<uint64_t> created_at_ns;
        char                  shm_name[registry::SHM_NAME_MAX];
        char                  topic_name[registry::TOPIC_NAME_MAX];
        char                  node_name[registry::NODE_NAME_MAX];
        uint8_t               _padding[144];
    };
    static_assert(sizeof(ParticipantEntry) == 512,
        "ParticipantEntry layout is part of the registry ABI");
    static_assert(offsetof(ParticipantEntry, _padding) == 368,
        "ParticipantEntry field offsets must match expected 368 B prefix");

    /// Plain copyable snapshot of one participant.
    struct Participant
    {
        uint64_t    pid;
        uint64_t    pid_starttime;   ///< OS-reported boot-relative start time (0 if unknown)
        uint64_t    created_at_ns;
        uint32_t    channel_type;
        uint32_t    role;
        uint32_t    kind;            ///< registry::Kind
        std::string shm_name;        ///< POSIX SHM path — implementation detail
        std::string topic_name;      ///< User-facing logical path, leading '/'
        std::string node_name;
    };

    /// Topic-centric grouping of registry entries: all participants on
    /// one shm_name, split by role (producer / consumer) and by pid
    /// liveness (alive / stall).  A Role::Both participant appears in
    /// both producers and consumers.
    struct TopicSummary
    {
        std::string              shm_name;
        std::string              topic_name;     ///< User-facing logical path
        uint32_t                 channel_type;   ///< channel::Type
        uint32_t                 kind;           ///< registry::Kind
        std::vector<Participant> producers;
        std::vector<Participant> consumers;
        std::vector<Participant> stall_producers;
        std::vector<Participant> stall_consumers;
    };

    struct RegistryHeader
    {
        std::atomic<uint64_t> magic;
        uint32_t              version;
        uint32_t              capacity;
        uint8_t               _padding[48];
    };
    static_assert(sizeof(RegistryHeader) == CACHE_LINE,
        "RegistryHeader must be exactly one cache line");

    /// Shared-memory participant registry.  One per namespace at
    /// `/{namespace}_registry`.  Persists beyond any single process;
    /// remove with `unlink()`.
    class Registry
    {
    public:
        Registry() = default;
        ~Registry() = default;
        Registry(Registry const&) = delete;
        Registry& operator=(Registry const&) = delete;
        Registry(Registry&&) noexcept = default;
        Registry& operator=(Registry&&) noexcept = default;

        /// `capacity` is only used on the create branch; an existing
        /// registry keeps its creator's capacity.
        static Registry open_or_create(std::string const& kmsg_namespace,
                                       uint32_t capacity = registry::DEFAULT_CAPACITY);

        /// Returns nullopt if the region doesn't exist.  For read-only
        /// tools that must not create a 2 MB SHM as a side effect of
        /// inspection.  Throws on version mismatch.
        static std::optional<Registry> try_open(std::string const& kmsg_namespace);

        static void unlink(std::string const& kmsg_namespace);

        /// Returns the claimed slot index, or `INVALID_SLOT` if the
        /// registry is full.
        uint32_t register_participant(std::string const& shm_name,
                                      std::string const& topic_name,
                                      channel::Type      channel_type,
                                      registry::Kind     kind,
                                      registry::Role     role,
                                      std::string const& node_name);

        /// Idempotent — `INVALID_SLOT` or already-Free slots are no-ops.
        void deregister(uint32_t slot_index);

        /// Copy of all `Active` entries.  Does not filter by process
        /// liveness — callers use `process_exists()` if they need that.
        std::vector<Participant> snapshot() const;

        /// Topic-centric view of the snapshot: groups participants by
        /// shm_name and splits each group into producer/consumer and
        /// alive/stall lists (alive checked via `process_exists()`).
        /// Results are sorted by shm_name for stable output.
        std::vector<TopicSummary> list_topics() const;

        /// CAS-resets `Active` slots whose `pid` no longer exists.
        /// Returns the number of slots freed.
        uint32_t sweep_stale();

        std::string const& name() const { return name_; }
        uint32_t           capacity() const;

    private:
        static std::size_t region_size(uint32_t capacity);
        static std::string make_shm_name(std::string const& kmsg_namespace);
        static std::optional<Registry> spin_open(std::string const& name);
        void init_as_creator(uint32_t capacity);

        RegistryHeader*       header();
        RegistryHeader const* header() const;
        ParticipantEntry*     entries();
        ParticipantEntry const* entries() const;

        SharedMemory shm_;
        std::string  name_;
    };
}

#endif
