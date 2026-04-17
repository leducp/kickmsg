#ifndef KICKMSG_REGISTRY_H
#define KICKMSG_REGISTRY_H

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "kickmsg/types.h"
#include "kickmsg/os/SharedMemory.h"

namespace kickmsg
{
    namespace registry
    {
        constexpr uint32_t VERSION          = 1;
        constexpr uint64_t MAGIC            = 0x214745524B43494BULL; // "KICKREG!"
        constexpr uint32_t DEFAULT_CAPACITY = 1024;
        constexpr std::size_t SHM_NAME_MAX  = 128;
        constexpr std::size_t NODE_NAME_MAX = 64;

        enum Role : uint32_t
        {
            Publisher  = 1,
            Subscriber = 2,
            Both       = 3,  ///< Node is both producer and consumer on this channel
        };

        /// Per-slot state.  Claiming is the window between CAS-claim and
        /// the release-store of field bytes — readers skip it.
        enum SlotState : uint32_t
        {
            Free     = 0,
            Claiming = 1,
            Active   = 2,
        };
    }

    /// In-SHM entry, 256 B.  Readers go through `Registry::snapshot()`
    /// because `state` is a std::atomic and this type is therefore not
    /// copyable.  Do not reorder fields without bumping `registry::VERSION`.
    struct ParticipantEntry
    {
        std::atomic<uint32_t> state;
        uint32_t              channel_type;
        uint32_t              role;
        uint32_t              _padding1;
        uint64_t              pid;
        uint64_t              created_at_ns;
        char                  shm_name[registry::SHM_NAME_MAX];
        char                  node_name[registry::NODE_NAME_MAX];
        uint8_t               _padding2[32];
    };
    static_assert(sizeof(ParticipantEntry) == 256,
        "ParticipantEntry layout is part of the registry ABI");

    /// Plain copyable snapshot of one participant.
    struct Participant
    {
        uint64_t    pid;
        uint64_t    created_at_ns;
        uint32_t    channel_type;
        uint32_t    role;
        std::string shm_name;
        std::string node_name;
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

        static void unlink(std::string const& kmsg_namespace);

        /// Returns the claimed slot index, or `INVALID_SLOT` if the
        /// registry is full.
        uint32_t register_participant(std::string const& shm_name,
                                      channel::Type      channel_type,
                                      registry::Role     role,
                                      std::string const& node_name);

        /// Idempotent — `INVALID_SLOT` or already-Free slots are no-ops.
        void deregister(uint32_t slot_index);

        /// Copy of all `Active` entries.  Does not filter by process
        /// liveness — callers use `process_exists()` if they need that.
        std::vector<Participant> snapshot() const;

        /// CAS-resets `Active` slots whose `pid` no longer exists.
        /// Returns the number of slots freed.
        uint32_t sweep_stale();

        std::string const& name() const { return name_; }
        uint32_t           capacity() const;

    private:
        static std::size_t region_size(uint32_t capacity);
        static std::string make_shm_name(std::string const& kmsg_namespace);
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
