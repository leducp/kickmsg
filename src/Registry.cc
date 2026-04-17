#include "kickmsg/Registry.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <unordered_map>

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

        {
            Registry r;
            r.name_ = name;
            if (r.shm_.try_create(name, bytes))
            {
                r.init_as_creator(capacity);
                return r;
            }
        }

        // Spin-wait on MAGIC — the creator publishes it last.
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
                if (not es[i].state.compare_exchange_strong(
                        expected, registry::Claiming,
                        std::memory_order_acq_rel,
                        std::memory_order_relaxed))
                {
                    continue;
                }

                // pid_starttime must be written before pid's release-store
                // so a sweeper's acquire-load of pid sees a matching
                // starttime.
                es[i].pid_starttime.store(my_starttime, std::memory_order_relaxed);
                es[i].pid.store(my_pid, std::memory_order_release);

                es[i].generation.fetch_add(1, std::memory_order_relaxed);

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
                return i;
            }
            return INVALID_SLOT;
        };

        uint32_t slot = try_claim();
        if (slot != INVALID_SLOT)
        {
            return slot;
        }
        // Registry full — sweep dead-pid residue and retry.  Bounded to
        // avoid livelock when many registrants race on a full registry.
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
        // Fields are intentionally not zeroed: a concurrent snapshot may
        // still be reading them, and partial zeroing before state=Free
        // would create torn reads that the seqlock can't catch.  The
        // next claim overwrites every field.
        es[slot_index].generation.fetch_add(1, std::memory_order_relaxed);
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
            uint32_t s1 = es[i].state.load(std::memory_order_acquire);
            if (s1 != registry::Active)
            {
                continue;
            }
            uint32_t g1 = es[i].generation.load(std::memory_order_acquire);

            Participant p{};
            p.pid           = es[i].pid.load(std::memory_order_relaxed);
            p.pid_starttime = es[i].pid_starttime.load(std::memory_order_relaxed);
            p.created_at_ns = es[i].created_at_ns.load(std::memory_order_relaxed);
            p.channel_type  = es[i].channel_type.load(std::memory_order_relaxed);
            p.role          = es[i].role.load(std::memory_order_relaxed);
            p.kind          = es[i].kind.load(std::memory_order_relaxed);
            p.shm_name.assign(
                es[i].shm_name,
                ::strnlen(es[i].shm_name, sizeof(es[i].shm_name)));
            p.topic_name.assign(
                es[i].topic_name,
                ::strnlen(es[i].topic_name, sizeof(es[i].topic_name)));
            p.node_name.assign(
                es[i].node_name,
                ::strnlen(es[i].node_name, sizeof(es[i].node_name)));

            // Seqlock recheck: reject if state changed or generation bumped.
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
            bool is_pub = (p.role == registry::Publisher
                        or p.role == registry::Both);
            bool is_sub = (p.role == registry::Subscriber
                        or p.role == registry::Both);

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

        // Live PID + matching boot-relative starttime = same process.
        // If either starttime is 0 (platform doesn't expose it), degrade
        // to trust-pid-alone.
        auto is_dead = [](uint64_t pid, uint64_t stored_start) -> bool
        {
            if (not process_exists(pid))
            {
                return true;
            }
            uint64_t live_start = process_starttime(pid);
            if (stored_start == 0 or live_start == 0)
            {
                return false;
            }
            return stored_start != live_start;
        };

        uint32_t freed = 0;
        for (uint32_t i = 0; i < cap; ++i)
        {
            uint32_t s = es[i].state.load(std::memory_order_acquire);
            if (s != registry::Active and s != registry::Claiming)
            {
                continue;
            }
            // Acquire syncs with register_participant's release-store of
            // pid, so we see a valid pid even while state==Claiming.
            uint64_t pid = es[i].pid.load(std::memory_order_acquire);
            if (pid == 0)
            {
                // Registrant hasn't stored its pid yet — reclaiming here
                // would race with its pending field writes.
                continue;
            }
            uint64_t stored_start = es[i].pid_starttime.load(
                                        std::memory_order_relaxed);
            if (not is_dead(pid, stored_start))
            {
                continue;
            }

            // Phase 1: CAS to Reclaiming to block concurrent registrants
            // and close the ABA window on a direct state→Free CAS.
            uint32_t expected = s;
            if (not es[i].state.compare_exchange_strong(
                    expected, registry::Reclaiming,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed))
            {
                continue;
            }

            // Re-verify under our exclusive hold.  A full dereg+register
            // cycle could have slipped in between our initial pid read
            // and the CAS above.
            uint64_t post_pid   = es[i].pid.load(std::memory_order_acquire);
            uint64_t post_start = es[i].pid_starttime.load(
                                      std::memory_order_relaxed);
            if (post_pid != pid or post_start != stored_start or
                not is_dead(post_pid, post_start))
            {
                // Restore via CAS, not blind store: a live tenant may
                // have legitimately dereg'd (state==Free) and a blind
                // store of `s` would resurrect the slot.
                uint32_t reclaiming_expected = registry::Reclaiming;
                es[i].state.compare_exchange_strong(
                    reclaiming_expected, s,
                    std::memory_order_release,
                    std::memory_order_relaxed);
                continue;
            }

            // Phase 2: finalize.  Fields are left untouched — same
            // reasoning as deregister().
            es[i].generation.fetch_add(1, std::memory_order_relaxed);
            es[i].state.store(registry::Free, std::memory_order_release);
            ++freed;
        }
        return freed;
    }
}
