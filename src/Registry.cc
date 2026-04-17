#include "kickmsg/Registry.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

#include "kickmsg/Naming.h"
#include "kickmsg/os/Process.h"
#include "kickmsg/os/Time.h"

namespace kickmsg
{
    std::size_t Registry::region_size(uint32_t capacity)
    {
        return sizeof(RegistryHeader)
             + static_cast<std::size_t>(capacity) * sizeof(ParticipantEntry);
    }

    std::string Registry::make_shm_name(std::string const& kmsg_namespace)
    {
        return "/" + sanitize_shm_component(kmsg_namespace, "namespace")
             + "_registry";
    }

    RegistryHeader* Registry::header()
    {
        return static_cast<RegistryHeader*>(shm_.address());
    }

    RegistryHeader const* Registry::header() const
    {
        return static_cast<RegistryHeader const*>(shm_.address());
    }

    ParticipantEntry* Registry::entries()
    {
        return reinterpret_cast<ParticipantEntry*>(
            static_cast<uint8_t*>(shm_.address()) + sizeof(RegistryHeader));
    }

    ParticipantEntry const* Registry::entries() const
    {
        return reinterpret_cast<ParticipantEntry const*>(
            static_cast<uint8_t const*>(shm_.address()) + sizeof(RegistryHeader));
    }

    uint32_t Registry::capacity() const
    {
        return header()->capacity;
    }

    void Registry::init_as_creator(uint32_t capacity)
    {
        std::memset(shm_.address(), 0, region_size(capacity));

        auto* h = header();
        h->version  = registry::VERSION;
        h->capacity = capacity;

        // MAGIC published last — readers spin on it with acquire.
        h->magic.store(registry::MAGIC, std::memory_order_release);
    }

    Registry Registry::open_or_create(std::string const& kmsg_namespace, uint32_t capacity)
    {
        if (capacity == 0)
        {
            throw std::invalid_argument("Registry capacity must be > 0");
        }

        std::string name = make_shm_name(kmsg_namespace);
        std::size_t bytes = region_size(capacity);

        Registry r;
        r.name_ = name;
        if (r.shm_.try_create(name, bytes))
        {
            r.init_as_creator(capacity);
            return r;
        }
        r.name_.clear();

        // Creator lost — spin-wait for MAGIC to guarantee we don't read
        // half-initialized bytes.
        for (int i = 0; i < 200; ++i)
        {
            SharedMemory shm;
            if (shm.try_open(name))
            {
                auto const* h = static_cast<RegistryHeader const*>(shm.address());
                if (h->magic.load(std::memory_order_acquire) == registry::MAGIC)
                {
                    if (h->version != registry::VERSION)
                    {
                        throw std::runtime_error(
                            "Registry version mismatch on " + name);
                    }
                    Registry out;
                    out.name_ = name;
                    out.shm_  = std::move(shm);
                    return out;
                }
            }
            kickmsg::sleep(10ms);
        }

        throw std::runtime_error("Timed out waiting for registry init: " + name);
    }

    void Registry::unlink(std::string const& kmsg_namespace)
    {
        SharedMemory::unlink(make_shm_name(kmsg_namespace));
    }

    uint32_t Registry::register_participant(std::string const& shm_name,
                                            channel::Type      channel_type,
                                            registry::Role     role,
                                            std::string const& node_name)
    {
        auto*    h   = header();
        auto*    es  = entries();
        uint32_t cap = h->capacity;

        for (uint32_t i = 0; i < cap; ++i)
        {
            uint32_t expected = registry::Free;
            if (not es[i].state.compare_exchange_strong(
                    expected, registry::Claiming,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed))
            {
                continue;
            }

            // Field stores are relaxed; the release-store of Active below
            // is the publication fence for readers.
            es[i].channel_type  = static_cast<uint32_t>(channel_type);
            es[i].role          = static_cast<uint32_t>(role);
            es[i]._padding1     = 0;
            es[i].pid           = current_pid();
            es[i].created_at_ns = static_cast<uint64_t>(
                                      kickmsg::since_epoch().count());

            std::memset(es[i].shm_name, 0, sizeof(es[i].shm_name));
            std::size_t n = std::min(shm_name.size(), sizeof(es[i].shm_name) - 1);
            std::memcpy(es[i].shm_name, shm_name.data(), n);

            std::memset(es[i].node_name, 0, sizeof(es[i].node_name));
            n = std::min(node_name.size(), sizeof(es[i].node_name) - 1);
            std::memcpy(es[i].node_name, node_name.data(), n);

            std::memset(es[i]._padding2, 0, sizeof(es[i]._padding2));

            es[i].state.store(registry::Active, std::memory_order_release);
            return i;
        }

        return INVALID_SLOT;
    }

    void Registry::deregister(uint32_t slot_index)
    {
        if (slot_index == INVALID_SLOT)
        {
            return;
        }
        auto*    h  = header();
        auto*    es = entries();
        if (slot_index >= h->capacity)
        {
            return;
        }
        es[slot_index].state.store(registry::Free, std::memory_order_release);
    }

    std::vector<Participant> Registry::snapshot() const
    {
        auto const* h   = header();
        auto const* es  = entries();
        uint32_t    cap = h->capacity;

        std::vector<Participant> out;
        out.reserve(cap);
        for (uint32_t i = 0; i < cap; ++i)
        {
            uint32_t s = es[i].state.load(std::memory_order_acquire);
            if (s != registry::Active)
            {
                continue;
            }

            Participant p{};
            p.pid           = es[i].pid;
            p.created_at_ns = es[i].created_at_ns;
            p.channel_type  = es[i].channel_type;
            p.role          = es[i].role;
            p.shm_name.assign(
                es[i].shm_name,
                ::strnlen(es[i].shm_name, sizeof(es[i].shm_name)));
            p.node_name.assign(
                es[i].node_name,
                ::strnlen(es[i].node_name, sizeof(es[i].node_name)));

            // Seqlock-style recheck: if the slot was dereg'd and reclaimed
            // while we copied, our fields may mix two tenants.
            uint32_t s2 = es[i].state.load(std::memory_order_acquire);
            if (s2 != registry::Active)
            {
                continue;
            }
            out.push_back(std::move(p));
        }
        return out;
    }

    uint32_t Registry::sweep_stale()
    {
        auto*    h   = header();
        auto*    es  = entries();
        uint32_t cap = h->capacity;

        uint32_t freed = 0;
        for (uint32_t i = 0; i < cap; ++i)
        {
            uint32_t s = es[i].state.load(std::memory_order_acquire);
            if (s != registry::Active)
            {
                continue;
            }
            uint64_t pid = es[i].pid;
            if (process_exists(pid))
            {
                continue;
            }
            uint32_t expected = registry::Active;
            if (es[i].state.compare_exchange_strong(
                    expected, registry::Free,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed))
            {
                ++freed;
            }
        }
        return freed;
    }
}
