#include "kickmsg/Registry.h"

#include <algorithm>
#include <stdexcept>
#include <unordered_map>

#include "kickmsg/Naming.h"
#include "kickmsg/os/Process.h"
#include "kickmsg/os/Time.h"

namespace kickmsg
{
    namespace
    {
        /// Mark the row as being written. The following release fence orders field stores.
        void open_generation(ParticipantEntry& e)
        {
            uint32_t g = e.generation.load(std::memory_order_relaxed);
            e.generation.store((g + 1) | 1u, std::memory_order_relaxed);
        }

        /// Mark the row as settled. The caller publishes preceding writes with a fence.
        void settle_generation(ParticipantEntry& e)
        {
            uint32_t g = e.generation.load(std::memory_order_relaxed);
            e.generation.store((g + 2) & ~1u, std::memory_order_relaxed);
        }
    }

    bool acquire_tenancy(ParticipantEntry& e, uint32_t generation)
    {
        // An even-to-odd CAS holds the row until its owner settles it.
        if ((generation & 1u) != 0)
        {
            return false;
        }
        uint32_t expected = generation;
        return e.generation.compare_exchange_strong(
            expected, generation + 1,
            std::memory_order_acq_rel, std::memory_order_relaxed);
    }

    std::size_t Registry::region_size(uint32_t capacity)
    {
        return sizeof(RegistryHeader)
             + static_cast<std::size_t>(capacity) * sizeof(ParticipantEntry);
    }

    std::string Registry::make_shm_name(std::string const& kmsg_namespace)
    {
        return compose_shm_name(sanitize_shm_component(kmsg_namespace, "namespace"), "registry");
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

        // MAGIC published last -- readers spin on it with acquire.
        h->magic.store(registry::MAGIC, std::memory_order_release);
    }

    std::optional<Registry> Registry::spin_open(std::string const& name)
    {
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
                        throw std::runtime_error("Registry version mismatch on " + name);
                    }
                    // Bound the entry array by the mapped size.
                    std::size_t avail = shm.size() - sizeof(RegistryHeader);
                    if (shm.size() < sizeof(RegistryHeader)
                        or h->capacity > avail / sizeof(ParticipantEntry))
                    {
                        throw std::runtime_error("Registry capacity exceeds segment on " + name);
                    }
                    Registry out;
                    out.name_ = name;
                    out.shm_  = std::move(shm);
                    return out;
                }
            }
            kickmsg::sleep(10ms);
        }
        return std::nullopt;
    }

    Registry Registry::open_or_create(std::string const& kmsg_namespace, uint32_t capacity)
    {
        if (capacity == 0)
        {
            throw std::invalid_argument("Registry capacity must be > 0");
        }

        std::string name  = make_shm_name(kmsg_namespace);
        std::size_t bytes = region_size(capacity);

        {
            Registry r;
            r.name_ = name;
            if (r.shm_.try_create(name, bytes))
            {
                r.init_as_creator(capacity);
                return r;
            }
        }

        auto opened = spin_open(name);
        if (opened.has_value())
        {
            return std::move(*opened);
        }
        throw std::runtime_error("Timed out waiting for registry init: " + name);
    }

    std::optional<Registry> Registry::try_open(std::string const& kmsg_namespace)
    {
        std::string  name = make_shm_name(kmsg_namespace);
        SharedMemory probe;
        if (not probe.try_open(name))
        {
            return std::nullopt;
        }
        return spin_open(name);
    }

    void Registry::unlink(std::string const& kmsg_namespace)
    {
        SharedMemory::unlink(make_shm_name(kmsg_namespace));
    }

    uint32_t Registry::register_participant(std::string const& shm_name,
                                            std::string const& topic_name,
                                            channel::Type      channel_type,
                                            registry::Kind     kind,
                                            registry::Role     role,
                                            std::string const& node_name)
    {
        auto try_claim = [&]() -> uint32_t
        {
            auto*    h   = header();
            auto*    es  = entries();
            uint32_t cap = h->capacity;

            auto copy_field = [](char* dst, std::size_t dst_size,
                                 std::string const& src)
            {
                std::memset(dst, 0, dst_size);
                std::size_t n = std::min(src.size(), dst_size - 1);
                std::memcpy(dst, src.data(), n);
            };

            uint64_t my_pid       = current_pid();
            uint64_t my_starttime = process_starttime(my_pid);
            uint64_t now_ns       = static_cast<uint64_t>(
                                        kickmsg::since_epoch().count());

            for (uint32_t i = 0; i < cap; ++i)
            {
                uint32_t expected = registry::Free;
                if (not es[i].state.compare_exchange_strong(expected, registry::Claiming,
                        std::memory_order_acq_rel, std::memory_order_relaxed))
                {
                    continue;
                }

                // Mark the row odd before changing fields that a snapshot may still read.
                open_generation(es[i]);
                std::atomic_thread_fence(std::memory_order_release);

                // The release-store of pid also publishes pid_starttime.
                es[i].pid_starttime.store(my_starttime, std::memory_order_relaxed);
                es[i].pid.store(my_pid, std::memory_order_release);

                es[i].channel_type.store(static_cast<uint32_t>(channel_type),
                                         std::memory_order_relaxed);
                es[i].role.store(static_cast<uint32_t>(role),
                                 std::memory_order_relaxed);
                es[i].kind.store(static_cast<uint32_t>(kind),
                                 std::memory_order_relaxed);
                es[i].created_at_ns.store(now_ns, std::memory_order_relaxed);

                copy_field(es[i].shm_name,   sizeof(es[i].shm_name),   shm_name);
                copy_field(es[i].topic_name, sizeof(es[i].topic_name), topic_name);
                copy_field(es[i].node_name,  sizeof(es[i].node_name),  node_name);
                std::memset(es[i]._padding, 0, sizeof(es[i]._padding));

                es[i].state.store(registry::Active, std::memory_order_release);

                // Publish an even generation only after all fields and state are set.
                std::atomic_thread_fence(std::memory_order_release);
                settle_generation(es[i]);
                return i;
            }
            return INVALID_SLOT;
        };

        uint32_t slot = try_claim();
        if (slot != INVALID_SLOT)
        {
            return slot;
        }
        // Bound retries when concurrent registrations compete for freed rows.
        for (int attempt = 0; attempt < 3; ++attempt)
        {
            if (sweep_stale() == 0)
            {
                break;
            }
            slot = try_claim();
            if (slot != INVALID_SLOT)
            {
                return slot;
            }
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
        auto& e = es[slot_index];

        // Keep descriptive fields for concurrent readers. Clear identity under
        // Reclaiming and publish Free last, after all metadata writes.
        uint32_t expected = registry::Active;
        if (not e.state.compare_exchange_strong(expected, registry::Reclaiming,
                std::memory_order_acq_rel, std::memory_order_relaxed))
        {
            return;
        }

        // Order the state change before clearing identity.
        std::atomic_thread_fence(std::memory_order_release);
        open_generation(e);
        e.pid.store(0, std::memory_order_relaxed);
        e.pid_starttime.store(0, std::memory_order_relaxed);
        settle_generation(e);

        // Sweeps skip our Reclaiming hold; publish Free only after all writes.
        std::atomic_thread_fence(std::memory_order_release);
        uint32_t retiring = registry::Reclaiming;
        e.state.compare_exchange_strong(retiring, registry::Free,
                                        std::memory_order_release,
                                        std::memory_order_relaxed);
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
            uint32_t s1 = es[i].state.load(std::memory_order_acquire);
            if (s1 != registry::Active)
            {
                continue;
            }
            uint32_t g1 = es[i].generation.load(std::memory_order_acquire);
            if ((g1 & 1u) != 0)
            {
                continue;
            }

            Participant p{};
            p.pid           = es[i].pid.load(std::memory_order_relaxed);
            p.pid_starttime = es[i].pid_starttime.load(std::memory_order_relaxed);
            p.created_at_ns = es[i].created_at_ns.load(std::memory_order_relaxed);
            p.channel_type  = es[i].channel_type.load(std::memory_order_relaxed);
            p.role          = es[i].role.load(std::memory_order_relaxed);
            p.kind          = es[i].kind.load(std::memory_order_relaxed);
            p.shm_name.assign  (es[i].shm_name,   ::strnlen(es[i].shm_name, sizeof(es[i].shm_name)));
            p.topic_name.assign(es[i].topic_name, ::strnlen(es[i].topic_name, sizeof(es[i].topic_name)));
            p.node_name.assign (es[i].node_name,  ::strnlen(es[i].node_name, sizeof(es[i].node_name)));

            // Keep field reads before the generation and state recheck.
            std::atomic_thread_fence(std::memory_order_acquire);
            uint32_t g2 = es[i].generation.load(std::memory_order_acquire);
            uint32_t s2 = es[i].state.load(std::memory_order_acquire);
            if (s2 != registry::Active or g1 != g2)
            {
                continue;
            }
            out.push_back(std::move(p));
        }
        return out;
    }

    std::vector<TopicSummary> Registry::list_topics() const
    {
        auto raw = snapshot();

        std::unordered_map<std::string, TopicSummary> by_shm;
        by_shm.reserve(raw.size());

        for (auto const& p : raw)
        {
            auto [iter, inserted] = by_shm.try_emplace(p.shm_name);
            auto& sum = iter->second;
            if (inserted)
            {
                sum.shm_name     = p.shm_name;
                sum.topic_name   = p.topic_name;
                sum.channel_type = p.channel_type;
                sum.kind         = p.kind;
            }

            bool alive  = process_exists(p.pid);
            bool is_pub = (p.role == registry::Publisher  or p.role == registry::Both);
            bool is_sub = (p.role == registry::Subscriber or p.role == registry::Both);

            if (is_pub)
            {
                if (alive)
                {
                    sum.producers.push_back(p);
                }
                else
                {
                    sum.stall_producers.push_back(p);
                }
            }
            if (is_sub)
            {
                if (alive)
                {
                    sum.consumers.push_back(p);
                }
                else
                {
                    sum.stall_consumers.push_back(p);
                }
            }
        }

        std::vector<TopicSummary> out;
        out.reserve(by_shm.size());
        for (auto& [_, sum] : by_shm)
        {
            out.push_back(std::move(sum));
        }
        std::sort(out.begin(), out.end(),
                  [](TopicSummary const& a, TopicSummary const& b)
                  { return a.shm_name < b.shm_name; });
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
            // A Reclaiming owner may still be writing, even with pid == 0.
            // Sweeps leave these rows alone; a crash can strand the slot.
            if (s != registry::Active and s != registry::Claiming)
            {
                continue;
            }
            // Validate that pid and start time belong to the same generation.
            // The acquire-load of pid pairs with registration's release-store.
            uint32_t g1           = es[i].generation.load(std::memory_order_acquire);
            if ((g1 & 1u) != 0)
            {
                // An odd row may belong to a writer or another sweeper.
                continue;
            }
            uint64_t pid          = es[i].pid.load(std::memory_order_acquire);
            uint64_t stored_start = es[i].pid_starttime.load(
                                        std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (es[i].generation.load(std::memory_order_acquire) != g1)
            {
                continue;  // spliced across a tenancy change
            }

            if (pid == 0)
            {
                // The claimant has not published its identity yet.
                continue;
            }
            if (not owner_is_dead(pid, stored_start))
            {
                continue;
            }

            // Acquire only the even generation whose owner was checked.
            // A changed or already-held generation must fail without touching state.
            if (not acquire_tenancy(es[i], g1))
            {
                continue;
            }

            es[i].state.store(registry::Reclaiming, std::memory_order_release);

            // The generation is already odd. Clear identity before settling it;
            // publish Free last so another registrant cannot start during these writes.
            std::atomic_thread_fence(std::memory_order_release);
            es[i].pid.store(0, std::memory_order_relaxed);
            es[i].pid_starttime.store(0, std::memory_order_relaxed);
            settle_generation(es[i]);

            std::atomic_thread_fence(std::memory_order_release);
            uint32_t reclaiming = registry::Reclaiming;
            es[i].state.compare_exchange_strong(reclaiming, registry::Free,
                                                std::memory_order_release, std::memory_order_relaxed);
            ++freed;
        }
        return freed;
    }
}
