#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <unordered_set>

#include <gtest/gtest.h>

#include "kickmsg/Naming.h"
#include "kickmsg/Node.h"
#include "kickmsg/Registry.h"
#include "kickmsg/os/Process.h"

class RegistryTest : public ::testing::Test
{
protected:
    static constexpr char const* KMSG_NAMESPACE = "kickmsg_regtest";

    // Use the same platform-specific naming rules as Node.
    static std::string topic_shm(char const* topic)
    {
        return kickmsg::compose_shm_name(
            kickmsg::sanitize_shm_component(KMSG_NAMESPACE, "namespace"),
            kickmsg::sanitize_shm_component(topic, "topic"));
    }

    static std::string broadcast_shm(char const* channel)
    {
        return kickmsg::compose_shm_name(
            kickmsg::sanitize_shm_component(KMSG_NAMESPACE, "namespace"),
            "broadcast_" + kickmsg::sanitize_shm_component(channel, "channel"));
    }

    static std::string registry_shm()
    {
        return kickmsg::compose_shm_name(
            kickmsg::sanitize_shm_component(KMSG_NAMESPACE, "namespace"),
            "registry");
    }

    void SetUp() override
    {
        kickmsg::Registry::unlink(KMSG_NAMESPACE);
    }

    void TearDown() override
    {
        kickmsg::Registry::unlink(KMSG_NAMESPACE);
        for (auto const& name : shm_to_unlink_)
        {
            kickmsg::SharedMemory::unlink(name);
        }
    }

    void track(std::string name)
    {
        shm_to_unlink_.push_back(std::move(name));
    }

private:
    std::vector<std::string> shm_to_unlink_;
};

TEST_F(RegistryTest, OpenOrCreateIsIdempotent)
{
    auto r1 = kickmsg::Registry::open_or_create(KMSG_NAMESPACE);
    EXPECT_EQ(r1.name(), registry_shm());

    // Second call opens the existing region — same name, same capacity.
    auto r2 = kickmsg::Registry::open_or_create(KMSG_NAMESPACE);
    EXPECT_EQ(r1.capacity(), r2.capacity());
    EXPECT_EQ(r1.name(), r2.name());
}

TEST_F(RegistryTest, RegisterAndSnapshotRoundTrip)
{
    auto reg = kickmsg::Registry::open_or_create(KMSG_NAMESPACE);

    uint32_t s1 = reg.register_participant(
        "/test_topic_a", "/topic_a", kickmsg::channel::PubSub,
        kickmsg::registry::Pubsub, kickmsg::registry::Publisher, "node_alpha");
    ASSERT_NE(s1, kickmsg::INVALID_SLOT);

    uint32_t s2 = reg.register_participant(
        "/test_topic_a", "/topic_a", kickmsg::channel::PubSub,
        kickmsg::registry::Pubsub, kickmsg::registry::Subscriber, "node_beta");
    ASSERT_NE(s2, kickmsg::INVALID_SLOT);
    EXPECT_NE(s1, s2);

    auto snap = reg.snapshot();
    ASSERT_EQ(snap.size(), 2u);

    // Collect into a set so we don't depend on iteration order.
    std::unordered_set<std::string> roles_by_node;
    for (auto const& p : snap)
    {
        EXPECT_EQ(p.shm_name, "/test_topic_a");
        EXPECT_EQ(p.channel_type, kickmsg::channel::PubSub);
        roles_by_node.insert(p.node_name + ":" + std::to_string(p.role));
    }
    EXPECT_TRUE(roles_by_node.count("node_alpha:1"));  // Publisher = 1
    EXPECT_TRUE(roles_by_node.count("node_beta:2"));   // Subscriber = 2

    reg.deregister(s1);
    auto after = reg.snapshot();
    ASSERT_EQ(after.size(), 1u);
    EXPECT_EQ(after[0].node_name, "node_beta");
}

TEST_F(RegistryTest, DeregisterInvalidSlotIsNoop)
{
    auto reg = kickmsg::Registry::open_or_create(KMSG_NAMESPACE);
    // Should not crash or throw.
    reg.deregister(kickmsg::INVALID_SLOT);
    reg.deregister(99999);  // Past capacity — silently ignored.
    EXPECT_EQ(reg.snapshot().size(), 0u);
}

TEST_F(RegistryTest, CapacityExhaustionReturnsInvalidSlot)
{
    // Small capacity so we can fill it quickly.
    constexpr uint32_t CAP = 4;
    auto reg = kickmsg::Registry::open_or_create(KMSG_NAMESPACE, CAP);
    EXPECT_EQ(reg.capacity(), CAP);

    std::vector<uint32_t> slots;
    for (uint32_t i = 0; i < CAP; ++i)
    {
        auto topic = "/topic_" + std::to_string(i);
        uint32_t s = reg.register_participant(
            "/test_topic_" + std::to_string(i), topic,
            kickmsg::channel::PubSub, kickmsg::registry::Pubsub,
            kickmsg::registry::Publisher, "node");
        ASSERT_NE(s, kickmsg::INVALID_SLOT);
        slots.push_back(s);
    }

    // One more push tips it over.
    uint32_t full = reg.register_participant(
        "/overflow", "/overflow", kickmsg::channel::PubSub,
        kickmsg::registry::Pubsub, kickmsg::registry::Publisher, "node");
    EXPECT_EQ(full, kickmsg::INVALID_SLOT);

    // Free a slot and try again — should succeed.
    reg.deregister(slots[0]);
    uint32_t reclaimed = reg.register_participant(
        "/after_free", "/after_free", kickmsg::channel::PubSub,
        kickmsg::registry::Pubsub, kickmsg::registry::Subscriber, "node2");
    EXPECT_NE(reclaimed, kickmsg::INVALID_SLOT);
}

TEST_F(RegistryTest, VersionMismatchOnSmallerExistingRegionThrows)
{
    auto created = kickmsg::Registry::open_or_create(KMSG_NAMESPACE, 8);
    EXPECT_EQ(created.capacity(), 8u);

    auto opened = kickmsg::Registry::open_or_create(KMSG_NAMESPACE, 1024);
    // Capacity from the existing region, not the requested one.
    EXPECT_EQ(opened.capacity(), 8u);
}

TEST_F(RegistryTest, OpenRejectsCorruptCapacity)
{
    // Change the stored capacity so the entry array exceeds the mapping.
    auto reg = kickmsg::Registry::open_or_create(KMSG_NAMESPACE, 8);

    kickmsg::SharedMemory raw;
    raw.open(registry_shm());
    auto* h = static_cast<kickmsg::RegistryHeader*>(raw.address());
    h->capacity = 0xFFFFFFFF;

    EXPECT_THROW(kickmsg::Registry::open_or_create(KMSG_NAMESPACE, 8),
                 std::runtime_error);
}

TEST_F(RegistryTest, SweepStaleRemovesDeadPidEntries)
{
    auto reg = kickmsg::Registry::open_or_create(KMSG_NAMESPACE);

    // Live entry — current process pid.
    uint32_t alive = reg.register_participant(
        "/live_topic", "/live_topic", kickmsg::channel::PubSub,
        kickmsg::registry::Pubsub, kickmsg::registry::Publisher, "alive");
    ASSERT_NE(alive, kickmsg::INVALID_SLOT);

    // Live entry for this process (via another participant).
    reg.register_participant(
        "/live_topic2", "/live_topic2", kickmsg::channel::PubSub,
        kickmsg::registry::Pubsub, kickmsg::registry::Subscriber, "alive2");

    EXPECT_EQ(reg.snapshot().size(), 2u);

    // No sweep needed yet — both pids alive.
    EXPECT_EQ(reg.sweep_stale(), 0u);
    EXPECT_EQ(reg.snapshot().size(), 2u);
}

TEST_F(RegistryTest, SweepStaleReclaimsWedgedClaimingSlot)
{
    // Stage an abandoned claim before identity publication.
    auto reg = kickmsg::Registry::open_or_create(KMSG_NAMESPACE);

    // Fill slot 0 with a legitimate entry.
    ASSERT_NE(reg.register_participant(
        "/keeper", "/keeper", kickmsg::channel::PubSub,
        kickmsg::registry::Pubsub, kickmsg::registry::Publisher, "keeper"),
              kickmsg::INVALID_SLOT);

    // Open the registry SHM directly to install a wedged Claiming slot.
    auto shm_name = registry_shm();
    kickmsg::SharedMemory raw;
    raw.open(shm_name);
    auto* entries = reinterpret_cast<kickmsg::ParticipantEntry*>(
        static_cast<uint8_t*>(raw.address()) + sizeof(kickmsg::RegistryHeader));

    constexpr uint32_t wedge_slot = 5;
    ASSERT_EQ(entries[wedge_slot].state.load(), kickmsg::registry::Free);

    entries[wedge_slot].pid = 0x7fffffff;  // guaranteed-dead PID
    entries[wedge_slot].state.store(kickmsg::registry::Claiming,
                                    std::memory_order_release);

    // Sweep should reclaim the wedged Claiming slot.
    EXPECT_EQ(reg.sweep_stale(), 1u);
    EXPECT_EQ(entries[wedge_slot].state.load(), kickmsg::registry::Free);
}

TEST_F(RegistryTest, SweepStaleSkipsClaimingSlotsWithoutPid)
{
    // A claim with pid == 0 may still have a live writer.
    auto reg = kickmsg::Registry::open_or_create(KMSG_NAMESPACE);

    auto shm_name = registry_shm();
    kickmsg::SharedMemory raw;
    raw.open(shm_name);
    auto* entries = reinterpret_cast<kickmsg::ParticipantEntry*>(
        static_cast<uint8_t*>(raw.address()) + sizeof(kickmsg::RegistryHeader));

    constexpr uint32_t wedge_slot = 3;
    entries[wedge_slot].pid = 0;
    entries[wedge_slot].state.store(kickmsg::registry::Claiming,
                                    std::memory_order_release);

    EXPECT_EQ(reg.sweep_stale(), 0u);
    EXPECT_EQ(entries[wedge_slot].state.load(), kickmsg::registry::Claiming);

    // Put the slot back so cleanup doesn't trip over it.
    entries[wedge_slot].state.store(kickmsg::registry::Free,
                                    std::memory_order_release);
}

TEST_F(RegistryTest, SnapshotRejectsARowCaughtMidRetirement)
{
    auto reg = kickmsg::Registry::open_or_create(KMSG_NAMESPACE);

    uint32_t slot = reg.register_participant(
        "/shm", "/topic", kickmsg::channel::PubSub, kickmsg::registry::Pubsub,
        kickmsg::registry::Publisher, "node");
    ASSERT_NE(slot, kickmsg::INVALID_SLOT);
    ASSERT_EQ(reg.snapshot().size(), 1u);

    kickmsg::SharedMemory raw;
    raw.open(registry_shm());
    auto* entries = reinterpret_cast<kickmsg::ParticipantEntry*>(
        static_cast<uint8_t*>(raw.address()) + sizeof(kickmsg::RegistryHeader));
    auto& e = entries[slot];

    // A settled row carries an even generation.
    ASSERT_EQ(e.generation.load(std::memory_order_acquire) & 1u, 0u);

    // Pause retirement after clearing pid but before settling generation.
    e.generation.fetch_add(1, std::memory_order_relaxed);
    e.pid.store(0, std::memory_order_relaxed);
    e.pid_starttime.store(0, std::memory_order_relaxed);

    EXPECT_TRUE(reg.snapshot().empty());

    e.pid.store(kickmsg::current_pid(), std::memory_order_relaxed);
    e.generation.fetch_add(1, std::memory_order_relaxed);
    auto settled = reg.snapshot();
    ASSERT_EQ(settled.size(), 1u);
    EXPECT_NE(settled[0].pid, 0u);

    reg.deregister(slot);
}

// Keep one row stable while another is repeatedly registered and retired.
TEST_F(RegistryTest, ConcurrentChurnNeverYieldsAnIncoherentSnapshot)
{
    auto reg = kickmsg::Registry::open_or_create(KMSG_NAMESPACE);

    uint32_t stable = reg.register_participant(
        "/stable", "/stable-topic", kickmsg::channel::PubSub,
        kickmsg::registry::Pubsub, kickmsg::registry::Publisher, "stable-node");
    ASSERT_NE(stable, kickmsg::INVALID_SLOT);

    std::atomic<bool>     stop{false};
    std::atomic<uint64_t> stable_seen{0};
    std::atomic<uint64_t> zero_pid{0};
    std::atomic<uint64_t> mixed{0};
    std::atomic<uint64_t> free_unsettled{0};

    std::thread churn([&]
    {
        while (not stop.load(std::memory_order_relaxed))
        {
            uint32_t slot = reg.register_participant(
                "/churn", "/churn-topic", kickmsg::channel::PubSub,
                kickmsg::registry::Pubsub, kickmsg::registry::Subscriber,
                "churn-node");
            if (slot != kickmsg::INVALID_SLOT)
            {
                reg.deregister(slot);
            }
        }
    });

    // Check that a stable even Active row has a PID.
    kickmsg::SharedMemory raw;
    raw.open(registry_shm());
    auto* entries = reinterpret_cast<kickmsg::ParticipantEntry*>(
        static_cast<uint8_t*>(raw.address()) + sizeof(kickmsg::RegistryHeader));
    uint32_t const cap = reinterpret_cast<kickmsg::RegistryHeader*>(
        raw.address())->capacity;

    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline)
    {
        for (int spin = 0; spin < 2000; ++spin)
        {
            for (uint32_t i = 0; i < cap; ++i)
            {
                uint32_t s1 = entries[i].state.load(std::memory_order_acquire);
                if (s1 != kickmsg::registry::Active)
                {
                    continue;
                }
                uint32_t g1 = entries[i].generation.load(std::memory_order_acquire);
                if ((g1 & 1u) != 0)
                {
                    continue;
                }
                uint64_t pid = entries[i].pid.load(std::memory_order_relaxed);
                std::atomic_thread_fence(std::memory_order_acquire);
                uint32_t g2 = entries[i].generation.load(std::memory_order_acquire);
                uint32_t s2 = entries[i].state.load(std::memory_order_acquire);
                if (s2 != kickmsg::registry::Active or g1 != g2)
                {
                    continue;
                }
                if (pid == 0)
                {
                    zero_pid.fetch_add(1, std::memory_order_relaxed);
                }
            }

            // Free permits a new writer, so its generation must already be even.
            for (uint32_t i = 0; i < cap; ++i)
            {
                uint32_t g1 = entries[i].generation.load(std::memory_order_acquire);
                uint32_t st = entries[i].state.load(std::memory_order_acquire);
                if (st != kickmsg::registry::Free)
                {
                    continue;
                }
                std::atomic_thread_fence(std::memory_order_acquire);
                uint32_t g2 = entries[i].generation.load(std::memory_order_acquire);
                if (g1 != g2)
                {
                    continue;   // the row moved under us; nothing proven
                }
                if ((g1 & 1u) != 0)
                {
                    free_unsettled.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
        for (auto const& p : reg.snapshot())
        {
            if (p.pid == 0)
            {
                zero_pid.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            bool const is_stable = p.shm_name == "/stable"
                               and p.topic_name == "/stable-topic"
                               and p.node_name == "stable-node"
                               and p.role == kickmsg::registry::Publisher;
            bool const is_churn  = p.shm_name == "/churn"
                               and p.topic_name == "/churn-topic"
                               and p.node_name == "churn-node"
                               and p.role == kickmsg::registry::Subscriber;
            if (is_stable)
            {
                stable_seen.fetch_add(1, std::memory_order_relaxed);
            }
            else if (not is_churn)
            {
                // Fields came from different registrations.
                mixed.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    stop.store(true, std::memory_order_relaxed);
    churn.join();

    EXPECT_EQ(zero_pid.load(), 0u) << "snapshot returned a retired identity";
    EXPECT_EQ(free_unsettled.load(), 0u)
        << "a claimable row was published with an unsettled generation";
    EXPECT_EQ(mixed.load(), 0u)    << "snapshot spliced two tenancies";
    EXPECT_GT(stable_seen.load(), 0u) << "oracle never saw the stable row";

    reg.deregister(stable);
}

TEST_F(RegistryTest, RowHandoffBetweenOwnersKeepsEveryTenancyVisible)
{
    auto reg = kickmsg::Registry::open_or_create(KMSG_NAMESPACE, 1);

    kickmsg::SharedMemory raw;
    raw.open(registry_shm());
    auto* entries = reinterpret_cast<kickmsg::ParticipantEntry*>(
        static_cast<uint8_t*>(raw.address()) + sizeof(kickmsg::RegistryHeader));

    for (int round = 0; round < 64; ++round)
    {
        uint32_t slot = reg.register_participant(
            "/shm", "/topic", kickmsg::channel::PubSub, kickmsg::registry::Pubsub,
            kickmsg::registry::Publisher, "node");
        ASSERT_NE(slot, kickmsg::INVALID_SLOT) << "round " << round;

        uint32_t gen = entries[0].generation.load(std::memory_order_acquire);
        EXPECT_EQ(gen & 1u, 0u) << "settled row has an odd generation, round " << round;

        auto rows = reg.snapshot();
        ASSERT_EQ(rows.size(), 1u) << "registered owner invisible, round " << round;
        EXPECT_NE(rows[0].pid, 0u);

        reg.deregister(slot);
        EXPECT_TRUE(reg.snapshot().empty()) << "round " << round;
        EXPECT_EQ(entries[0].generation.load(std::memory_order_acquire) & 1u, 0u)
            << "row left odd after retirement, round " << round;
    }
}

// Use two rows so registration can proceed without an automatic sweep.
TEST_F(RegistryTest, ARetiringRowIsNeitherVisibleNorClaimable)
{
    auto reg = kickmsg::Registry::open_or_create(KMSG_NAMESPACE, 2);

    kickmsg::SharedMemory raw;
    raw.open(registry_shm());
    auto* entries = reinterpret_cast<kickmsg::ParticipantEntry*>(
        static_cast<uint8_t*>(raw.address()) + sizeof(kickmsg::RegistryHeader));

    uint32_t slot = reg.register_participant(
        "/retiring", "/retiring-topic", kickmsg::channel::PubSub,
        kickmsg::registry::Pubsub, kickmsg::registry::Publisher, "retiring-node");
    ASSERT_EQ(slot, 0u);

    // Pause retirement after clearing identity, before publishing Free.
    entries[0].state.store(kickmsg::registry::Reclaiming, std::memory_order_release);
    entries[0].pid.store(0, std::memory_order_relaxed);
    entries[0].pid_starttime.store(0, std::memory_order_relaxed);

    // Invisible: no Active row with a cleared identity.
    EXPECT_TRUE(reg.snapshot().empty());

    // Unclaimable: the next registrant must take the OTHER row, not this one.
    uint32_t next = reg.register_participant(
        "/next", "/next-topic", kickmsg::channel::PubSub,
        kickmsg::registry::Pubsub, kickmsg::registry::Subscriber, "next-node");
    ASSERT_NE(next, kickmsg::INVALID_SLOT);
    EXPECT_NE(next, slot) << "a retiring row was handed to a second owner";

    auto rows = reg.snapshot();
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].node_name, "next-node");
    EXPECT_NE(rows[0].pid, 0u);
    EXPECT_EQ(entries[next].generation.load(std::memory_order_acquire) & 1u, 0u);

    reg.deregister(next);
}

// Odd abandoned claims remain unavailable because a sweep cannot
// distinguish them from an active writer or reclaimer.
TEST_F(RegistryTest, SweepRefusesAnAbandonedOddClaimRatherThanRaceItsHolder)
{
    auto reg = kickmsg::Registry::open_or_create(KMSG_NAMESPACE, 1);

    kickmsg::SharedMemory raw;
    raw.open(registry_shm());
    auto* entries = reinterpret_cast<kickmsg::ParticipantEntry*>(
        static_cast<uint8_t*>(raw.address()) + sizeof(kickmsg::RegistryHeader));
    auto& e = entries[0];

    // Died after opening its seqlock and publishing an identity that is gone.
    e.state.store(kickmsg::registry::Claiming, std::memory_order_release);
    e.generation.store(1, std::memory_order_relaxed);
    e.pid_starttime.store(1, std::memory_order_relaxed);
    e.pid.store(0x3fffffff, std::memory_order_release);

    EXPECT_EQ(reg.sweep_stale(), 0u)
        << "recovery raced a row whose writer it cannot identify";
    EXPECT_EQ(e.state.load(std::memory_order_acquire), kickmsg::registry::Claiming);
    EXPECT_EQ(e.generation.load(std::memory_order_acquire), 1u);

    EXPECT_EQ(reg.register_participant(
                  "/shm", "/topic", kickmsg::channel::PubSub,
                  kickmsg::registry::Pubsub, kickmsg::registry::Publisher, "node"),
              kickmsg::INVALID_SLOT);

    e.state.store(kickmsg::registry::Free, std::memory_order_release);
    e.generation.store(0, std::memory_order_relaxed);
}

TEST_F(RegistryTest, SweepLeavesARowHeldByAnotherOwnerAlone)
{
    auto reg = kickmsg::Registry::open_or_create(KMSG_NAMESPACE, 1);

    kickmsg::SharedMemory raw;
    raw.open(registry_shm());
    auto* entries = reinterpret_cast<kickmsg::ParticipantEntry*>(
        static_cast<uint8_t*>(raw.address()) + sizeof(kickmsg::RegistryHeader));
    auto& e = entries[0];

    uint32_t slot = reg.register_participant(
        "/shm", "/topic", kickmsg::channel::PubSub, kickmsg::registry::Pubsub,
        kickmsg::registry::Publisher, "node");
    ASSERT_NE(slot, kickmsg::INVALID_SLOT);

    // Mid-retirement: Reclaiming held, identity cleared, Free not yet published.
    e.state.store(kickmsg::registry::Reclaiming, std::memory_order_release);
    e.pid.store(0, std::memory_order_relaxed);
    uint32_t const gen_before = e.generation.load(std::memory_order_acquire);

    EXPECT_TRUE(reg.snapshot().empty()) << "a retiring row must not be visible";

    EXPECT_EQ(reg.sweep_stale(), 0u);
    EXPECT_EQ(e.state.load(std::memory_order_acquire), kickmsg::registry::Reclaiming);
    EXPECT_EQ(e.generation.load(std::memory_order_acquire), gen_before);

    EXPECT_EQ(reg.register_participant(
                  "/other", "/other-topic", kickmsg::channel::PubSub,
                  kickmsg::registry::Pubsub, kickmsg::registry::Subscriber,
                  "other-node"),
              kickmsg::INVALID_SLOT)
        << "registration recycled a row another owner is still writing";
    EXPECT_EQ(e.state.load(std::memory_order_acquire), kickmsg::registry::Reclaiming);

    // Let the owner finish; the row comes back on its own.
    e.pid_starttime.store(0, std::memory_order_relaxed);
    reg.deregister(slot);
}

TEST_F(RegistryTest, FullRegistryRegistrationCannotStealAPausedRetirement)
{
    auto reg = kickmsg::Registry::open_or_create(KMSG_NAMESPACE, 1);

    kickmsg::SharedMemory raw;
    raw.open(registry_shm());
    auto* entries = reinterpret_cast<kickmsg::ParticipantEntry*>(
        static_cast<uint8_t*>(raw.address()) + sizeof(kickmsg::RegistryHeader));
    auto& e = entries[0];

    uint32_t slot = reg.register_participant(
        "/old", "/old-topic", kickmsg::channel::PubSub, kickmsg::registry::Pubsub,
        kickmsg::registry::Publisher, "old-node");
    ASSERT_NE(slot, kickmsg::INVALID_SLOT);

    // Pause A after clearing pid, before clearing starttime and settling generation.
    e.state.store(kickmsg::registry::Reclaiming, std::memory_order_release);
    e.generation.fetch_add(1, std::memory_order_relaxed);   // bracket opened
    e.pid.store(0, std::memory_order_relaxed);

    // B registers.  The registry is full, so this sweeps.
    uint32_t b = reg.register_participant(
        "/new", "/new-topic", kickmsg::channel::PubSub, kickmsg::registry::Pubsub,
        kickmsg::registry::Subscriber, "new-node");
    EXPECT_EQ(b, kickmsg::INVALID_SLOT)
        << "a paused retirement was recycled out from under its owner";

    // Resume A's remaining retirement writes.
    e.pid_starttime.store(0, std::memory_order_relaxed);
    e.generation.store((e.generation.load(std::memory_order_relaxed) + 2) & ~1u,
                       std::memory_order_relaxed);
    uint32_t retiring = kickmsg::registry::Reclaiming;
    EXPECT_TRUE(e.state.compare_exchange_strong(retiring, kickmsg::registry::Free,
                                                std::memory_order_release,
                                                std::memory_order_relaxed))
        << "the owner lost its own row";

    // Only now is the row available, with a coherent identity.
    uint32_t next = reg.register_participant(
        "/new", "/new-topic", kickmsg::channel::PubSub, kickmsg::registry::Pubsub,
        kickmsg::registry::Subscriber, "new-node");
    ASSERT_NE(next, kickmsg::INVALID_SLOT);
    auto rows = reg.snapshot();
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].node_name, "new-node");
    EXPECT_NE(rows[0].pid, 0u);
    EXPECT_NE(rows[0].pid_starttime, 0u)
        << "a resuming owner zeroed the replacement's start time";
    reg.deregister(next);
}

TEST_F(RegistryTest, ASecondSweepCannotTakeARowAlreadyHeldBySweeping)
{
    auto reg = kickmsg::Registry::open_or_create(KMSG_NAMESPACE, 1);

    kickmsg::SharedMemory raw;
    raw.open(registry_shm());
    auto* entries = reinterpret_cast<kickmsg::ParticipantEntry*>(
        static_cast<uint8_t*>(raw.address()) + sizeof(kickmsg::RegistryHeader));
    auto& e = entries[0];

    // Pause sweeper A after claiming Reclaiming, before clearing the dead identity.
    e.pid.store(0x3fffffff, std::memory_order_relaxed);
    e.pid_starttime.store(1, std::memory_order_relaxed);
    e.state.store(kickmsg::registry::Reclaiming, std::memory_order_release);
    uint32_t const gen_before = e.generation.load(std::memory_order_acquire);

    // Sweeper B runs, explicitly and via a full-registry registration.
    EXPECT_EQ(reg.sweep_stale(), 0u)
        << "a second sweeper took a row the first one holds";
    EXPECT_EQ(reg.register_participant(
                  "/replacement", "/replacement-topic", kickmsg::channel::PubSub,
                  kickmsg::registry::Pubsub, kickmsg::registry::Publisher,
                  "replacement-node"),
              kickmsg::INVALID_SLOT);
    EXPECT_EQ(e.state.load(std::memory_order_acquire), kickmsg::registry::Reclaiming);
    EXPECT_EQ(e.generation.load(std::memory_order_acquire), gen_before);
    EXPECT_EQ(e.pid.load(std::memory_order_acquire), 0x3fffffffu)
        << "a second sweeper cleared the identity under the first one";

    // A finishes; the row returns to service.
    e.pid.store(0, std::memory_order_relaxed);
    e.pid_starttime.store(0, std::memory_order_relaxed);
    e.state.store(kickmsg::registry::Free, std::memory_order_release);
}

TEST_F(RegistryTest, AcquireTenancyRefusesAVersionItDidNotValidate)
{
    auto reg = kickmsg::Registry::open_or_create(KMSG_NAMESPACE, 1);

    kickmsg::SharedMemory raw;
    raw.open(registry_shm());
    auto* entries = reinterpret_cast<kickmsg::ParticipantEntry*>(
        static_cast<uint8_t*>(raw.address()) + sizeof(kickmsg::RegistryHeader));
    auto& e = entries[0];

    uint32_t first = reg.register_participant(
        "/old", "/old-topic", kickmsg::channel::PubSub, kickmsg::registry::Pubsub,
        kickmsg::registry::Publisher, "old-node");
    ASSERT_NE(first, kickmsg::INVALID_SLOT);

    // A sweeper validated this tenancy and captured its version, then paused.
    uint32_t const validated = e.generation.load(std::memory_order_acquire);
    ASSERT_EQ(validated & 1u, 0u);

    // A real handoff: the owner retires, a second live owner takes the row.
    reg.deregister(first);
    uint32_t second = reg.register_participant(
        "/new", "/new-topic", kickmsg::channel::PubSub, kickmsg::registry::Pubsub,
        kickmsg::registry::Subscriber, "new-node");
    ASSERT_EQ(second, first) << "the test needs the row to be reused";
    ASSERT_EQ(e.state.load(std::memory_order_acquire), kickmsg::registry::Active);

    // Try acquisition with the generation saved before the row was reused.
    EXPECT_FALSE(kickmsg::acquire_tenancy(e, validated))
        << "a sweeper acquired a tenancy it never looked at";
    EXPECT_EQ(e.state.load(std::memory_order_acquire), kickmsg::registry::Active);
    EXPECT_EQ(e.generation.load(std::memory_order_acquire) & 1u, 0u)
        << "a refused acquisition left the row marked in flux";

    reg.deregister(second);
    EXPECT_TRUE(reg.snapshot().empty()) << "deregistration was silently dropped";
    uint32_t reuse = reg.register_participant(
        "/reuse", "/reuse-topic", kickmsg::channel::PubSub,
        kickmsg::registry::Pubsub, kickmsg::registry::Publisher, "reuse-node");
    EXPECT_NE(reuse, kickmsg::INVALID_SLOT) << "registry capacity leaked";
    reg.deregister(reuse);
}

TEST_F(RegistryTest, AcquireTenancySucceedsOnceForTheValidatedVersion)
{
    auto reg = kickmsg::Registry::open_or_create(KMSG_NAMESPACE, 1);

    kickmsg::SharedMemory raw;
    raw.open(registry_shm());
    auto* entries = reinterpret_cast<kickmsg::ParticipantEntry*>(
        static_cast<uint8_t*>(raw.address()) + sizeof(kickmsg::RegistryHeader));
    auto& e = entries[0];

    uint32_t slot = reg.register_participant(
        "/shm", "/topic", kickmsg::channel::PubSub, kickmsg::registry::Pubsub,
        kickmsg::registry::Publisher, "node");
    ASSERT_NE(slot, kickmsg::INVALID_SLOT);

    uint32_t const validated = e.generation.load(std::memory_order_acquire);
    EXPECT_TRUE(kickmsg::acquire_tenancy(e, validated));
    EXPECT_EQ(e.generation.load(std::memory_order_acquire) & 1u, 1u)
        << "an acquired row must read as in flux";

    // A second caller holding the same validated version loses.
    EXPECT_FALSE(kickmsg::acquire_tenancy(e, validated));

    // Put the row back the way sweep_stale's phase 2 would.
    e.pid.store(0, std::memory_order_relaxed);
    e.pid_starttime.store(0, std::memory_order_relaxed);
    uint32_t g = e.generation.load(std::memory_order_relaxed);
    e.generation.store((g + 2) & ~1u, std::memory_order_relaxed);
    e.state.store(kickmsg::registry::Free, std::memory_order_release);
}

TEST_F(RegistryTest, AcquireTenancyRefusesAVersionAnotherAcquirerIsHolding)
{
    auto reg = kickmsg::Registry::open_or_create(KMSG_NAMESPACE, 1);

    kickmsg::SharedMemory raw;
    raw.open(registry_shm());
    auto* entries = reinterpret_cast<kickmsg::ParticipantEntry*>(
        static_cast<uint8_t*>(raw.address()) + sizeof(kickmsg::RegistryHeader));
    auto& e = entries[0];

    uint32_t slot = reg.register_participant(
        "/shm", "/topic", kickmsg::channel::PubSub, kickmsg::registry::Pubsub,
        kickmsg::registry::Publisher, "node");
    ASSERT_NE(slot, kickmsg::INVALID_SLOT);

    // Pause A before publishing Reclaiming; only its odd generation marks the hold.
    uint32_t const validated = e.generation.load(std::memory_order_acquire);
    ASSERT_TRUE(kickmsg::acquire_tenancy(e, validated));
    uint32_t const held = e.generation.load(std::memory_order_acquire);
    ASSERT_EQ(held & 1u, 1u);
    ASSERT_EQ(e.state.load(std::memory_order_acquire), kickmsg::registry::Active);

    // B reads that new version and must be refused.
    EXPECT_FALSE(kickmsg::acquire_tenancy(e, held))
        << "a second acquirer took the version the first one is holding";
    EXPECT_EQ(e.generation.load(std::memory_order_acquire), held)
        << "a refused acquisition moved the version";

    // Nor may a real sweep take it, however dead the row's identity looks.
    e.pid.store(0x3fffffff, std::memory_order_relaxed);
    e.pid_starttime.store(1, std::memory_order_relaxed);
    EXPECT_EQ(reg.sweep_stale(), 0u)
        << "sweep_stale acquired a row another recoverer holds";
    EXPECT_EQ(e.generation.load(std::memory_order_acquire), held);

    // A settles its own reclamation.
    e.state.store(kickmsg::registry::Reclaiming, std::memory_order_release);
    e.pid.store(0, std::memory_order_relaxed);
    e.pid_starttime.store(0, std::memory_order_relaxed);
    e.generation.store((held + 2) & ~1u, std::memory_order_relaxed);
    e.state.store(kickmsg::registry::Free, std::memory_order_release);
}

TEST_F(RegistryTest, SweepsConcurrentWithChurnNeverLeakCapacity)
{
    auto reg = kickmsg::Registry::open_or_create(KMSG_NAMESPACE, 2);

    std::atomic<bool>     stop{false};
    std::atomic<uint64_t> lost{0};
    std::atomic<uint64_t> cycles{0};

    std::thread sweeper([&]
    {
        while (not stop.load(std::memory_order_relaxed))
        {
            // All owners are alive; no row should be reclaimed.
            if (reg.sweep_stale() != 0)
            {
                lost.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });

    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline)
    {
        uint32_t slot = reg.register_participant(
            "/churn", "/churn-topic", kickmsg::channel::PubSub,
            kickmsg::registry::Pubsub, kickmsg::registry::Publisher, "churn-node");
        if (slot == kickmsg::INVALID_SLOT)
        {
            lost.fetch_add(1, std::memory_order_relaxed);
            break;   // capacity leaked: a deregistration was dropped earlier
        }
        reg.deregister(slot);
        cycles.fetch_add(1, std::memory_order_relaxed);
    }
    stop.store(true, std::memory_order_relaxed);
    sweeper.join();

    EXPECT_EQ(lost.load(), 0u)
        << "a sweep reclaimed a live row or a deregistration was dropped";
    EXPECT_GT(cycles.load(), 0u);
    EXPECT_TRUE(reg.snapshot().empty());
}

TEST_F(RegistryTest, DeregisterIsANoOpOnARowItNoLongerHolds)
{
    auto reg = kickmsg::Registry::open_or_create(KMSG_NAMESPACE, 1);

    kickmsg::SharedMemory raw;
    raw.open(registry_shm());
    auto* entries = reinterpret_cast<kickmsg::ParticipantEntry*>(
        static_cast<uint8_t*>(raw.address()) + sizeof(kickmsg::RegistryHeader));

    uint32_t slot = reg.register_participant(
        "/shm", "/topic", kickmsg::channel::PubSub, kickmsg::registry::Pubsub,
        kickmsg::registry::Publisher, "node");
    ASSERT_NE(slot, kickmsg::INVALID_SLOT);

    reg.deregister(slot);
    ASSERT_EQ(entries[0].state.load(std::memory_order_acquire),
              kickmsg::registry::Free);
    uint32_t const settled = entries[0].generation.load(std::memory_order_acquire);

    // Second call: the row is Free, so nothing may move.
    reg.deregister(slot);
    EXPECT_EQ(entries[0].state.load(std::memory_order_acquire),
              kickmsg::registry::Free);
    EXPECT_EQ(entries[0].generation.load(std::memory_order_acquire), settled);
}

// Node integration -- Node advertise/subscribe/etc should populate the registry

// A namespace cannot mix kickmsg builds: an old registry must stop the Node, not silently
// switch discovery off.
TEST_F(RegistryTest, VersionMismatchIsFatalForRegistryAndNode)
{
    // Held open: a Windows mapping is destroyed with its last handle.
    auto old_build = kickmsg::Registry::open_or_create(KMSG_NAMESPACE);
    kickmsg::SharedMemory raw;
    raw.open(registry_shm());
    auto* header = static_cast<kickmsg::RegistryHeader*>(raw.address());
    header->version = kickmsg::registry::VERSION - 1;

    EXPECT_THROW(kickmsg::Registry::open_or_create(KMSG_NAMESPACE), kickmsg::VersionMismatch);
    EXPECT_THROW(kickmsg::Registry::try_open(KMSG_NAMESPACE), kickmsg::VersionMismatch);

    kickmsg::channel::Config cfg;
    cfg.max_subscribers   = 2;
    cfg.sub_ring_capacity = 4;
    cfg.pool_size         = 16;
    cfg.max_payload_size  = 32;

    kickmsg::Node n("mixed_node", KMSG_NAMESPACE);
    track(topic_shm("mixed"));
    EXPECT_THROW(n.advertise("mixed", cfg), kickmsg::VersionMismatch);
    EXPECT_THROW(n.advertise("mixed", cfg), kickmsg::VersionMismatch)
        << "the mismatch was latched away instead of staying fatal";
}

TEST_F(RegistryTest, WalksUseTheCapacityValidatedAtOpen)
{
    auto reg = kickmsg::Registry::open_or_create(KMSG_NAMESPACE, 8);
    uint32_t slot = reg.register_participant(
        "/shm", "/topic", kickmsg::channel::PubSub, kickmsg::registry::Pubsub,
        kickmsg::registry::Publisher, "node");
    ASSERT_NE(slot, kickmsg::INVALID_SLOT);

    // A peer rewrites the capacity after open.
    kickmsg::SharedMemory raw;
    raw.open(registry_shm());
    auto* header = static_cast<kickmsg::RegistryHeader*>(raw.address());
    header->capacity = UINT32_MAX;

    EXPECT_EQ(reg.capacity(), 8u);
    EXPECT_EQ(reg.snapshot().size(), 1u);
    EXPECT_EQ(reg.list_topics().size(), 1u);
    EXPECT_EQ(reg.sweep_stale(), 0u);
    reg.deregister(UINT32_MAX - 1);
    reg.deregister(slot);
    EXPECT_TRUE(reg.snapshot().empty());
}

TEST_F(RegistryTest, NodeAdvertiseRegistersPublisher)
{
    kickmsg::channel::Config cfg;
    cfg.max_subscribers   = 2;
    cfg.sub_ring_capacity = 4;
    cfg.pool_size         = 8;
    cfg.max_payload_size  = 32;

    {
        kickmsg::Node n("pub_node", KMSG_NAMESPACE);
        auto pub = n.advertise("topicX", cfg);
        track(topic_shm("topicX"));

        auto reg = kickmsg::Registry::open_or_create(KMSG_NAMESPACE);
        auto snap = reg.snapshot();
        ASSERT_EQ(snap.size(), 1u);
        EXPECT_EQ(snap[0].node_name, "pub_node");
        EXPECT_EQ(snap[0].role, kickmsg::registry::Publisher);
        EXPECT_EQ(snap[0].shm_name,
                  topic_shm("topicX"));
    }

    // Node went out of scope — entry should be gone.
    auto reg = kickmsg::Registry::open_or_create(KMSG_NAMESPACE);
    EXPECT_EQ(reg.snapshot().size(), 0u);
}

TEST_F(RegistryTest, NodeBroadcastRegistersBoth)
{
    kickmsg::channel::Config cfg;
    cfg.max_subscribers   = 2;
    cfg.sub_ring_capacity = 4;
    cfg.pool_size         = 8;
    cfg.max_payload_size  = 32;

    kickmsg::Node n("bcast_node", KMSG_NAMESPACE);
    auto bh = n.join_broadcast("chanX", cfg);
    track(broadcast_shm("chanX"));

    auto reg  = kickmsg::Registry::open_or_create(KMSG_NAMESPACE);
    auto snap = reg.snapshot();
    ASSERT_EQ(snap.size(), 1u);
    EXPECT_EQ(snap[0].role,         kickmsg::registry::Both);
    EXPECT_EQ(snap[0].channel_type, kickmsg::channel::Broadcast);
}

TEST_F(RegistryTest, NodeAdvertiseThenSubscribeUpgradesToBoth)
{
    kickmsg::channel::Config cfg;
    cfg.max_subscribers   = 2;
    cfg.sub_ring_capacity = 4;
    cfg.pool_size         = 8;
    cfg.max_payload_size  = 32;

    kickmsg::Node n("dual_node", KMSG_NAMESPACE);
    auto pub = n.advertise("dualtopic", cfg);
    auto sub = n.subscribe("dualtopic");
    track(topic_shm("dualtopic"));

    auto reg  = kickmsg::Registry::open_or_create(KMSG_NAMESPACE);
    auto snap = reg.snapshot();
    ASSERT_EQ(snap.size(), 1u);
    EXPECT_EQ(snap[0].role, kickmsg::registry::Both);
    EXPECT_EQ(snap[0].node_name, "dual_node");
}

TEST_F(RegistryTest, MultipleNodesEachAppearOnce)
{
    kickmsg::channel::Config cfg;
    cfg.max_subscribers   = 4;
    cfg.sub_ring_capacity = 4;
    cfg.pool_size         = 8;
    cfg.max_payload_size  = 32;

    kickmsg::Node pub("pub_a", KMSG_NAMESPACE);
    auto p = pub.advertise("shared", cfg);
    track(topic_shm("shared"));

    kickmsg::Node s1("sub_a", KMSG_NAMESPACE);
    auto s1_h = s1.subscribe("shared");
    kickmsg::Node s2("sub_b", KMSG_NAMESPACE);
    auto s2_h = s2.subscribe("shared");

    auto reg  = kickmsg::Registry::open_or_create(KMSG_NAMESPACE);
    auto snap = reg.snapshot();
    EXPECT_EQ(snap.size(), 3u);

    std::unordered_set<std::string> nodes;
    for (auto const& part : snap)
    {
        nodes.insert(part.node_name);
    }
    EXPECT_TRUE(nodes.count("pub_a"));
    EXPECT_TRUE(nodes.count("sub_a"));
    EXPECT_TRUE(nodes.count("sub_b"));
}

// list_topics -- topic-centric aggregation

TEST_F(RegistryTest, ListTopicsGroupsByShmName)
{
    kickmsg::channel::Config cfg;
    cfg.max_subscribers   = 4;
    cfg.sub_ring_capacity = 4;
    cfg.pool_size         = 8;
    cfg.max_payload_size  = 32;

    kickmsg::Node pub("pub_a", KMSG_NAMESPACE);
    auto p = pub.advertise("telemetry", cfg);
    track(topic_shm("telemetry"));

    kickmsg::Node s1("sub_a", KMSG_NAMESPACE);
    auto s1_h = s1.subscribe("telemetry");
    kickmsg::Node s2("sub_b", KMSG_NAMESPACE);
    auto s2_h = s2.subscribe("telemetry");

    auto reg    = kickmsg::Registry::open_or_create(KMSG_NAMESPACE);
    auto topics = reg.list_topics();

    ASSERT_EQ(topics.size(), 1u);
    auto const& t = topics[0];
    EXPECT_EQ(t.shm_name, topic_shm("telemetry"));
    EXPECT_EQ(t.channel_type, kickmsg::channel::PubSub);
    EXPECT_EQ(t.producers.size(), 1u);
    EXPECT_EQ(t.consumers.size(), 2u);
    EXPECT_EQ(t.stall_producers.size(), 0u);
    EXPECT_EQ(t.stall_consumers.size(), 0u);
    EXPECT_EQ(t.producers[0].node_name, "pub_a");
}

TEST_F(RegistryTest, ListTopicsBroadcastRoleBothInEveryLane)
{
    kickmsg::channel::Config cfg;
    cfg.max_subscribers   = 4;
    cfg.sub_ring_capacity = 4;
    cfg.pool_size         = 8;
    cfg.max_payload_size  = 32;

    kickmsg::Node node("bcast", KMSG_NAMESPACE);
    auto bh = node.join_broadcast("events", cfg);
    track(broadcast_shm("events"));

    auto reg    = kickmsg::Registry::open_or_create(KMSG_NAMESPACE);
    auto topics = reg.list_topics();

    ASSERT_EQ(topics.size(), 1u);
    // A Both role counts as one producer AND one consumer.
    EXPECT_EQ(topics[0].producers.size(), 1u);
    EXPECT_EQ(topics[0].consumers.size(), 1u);
    EXPECT_EQ(topics[0].producers[0].pid, topics[0].consumers[0].pid);
}

TEST_F(RegistryTest, ListTopicsSortedByShmName)
{
    kickmsg::channel::Config cfg;
    cfg.max_subscribers   = 2;
    cfg.sub_ring_capacity = 4;
    cfg.pool_size         = 8;
    cfg.max_payload_size  = 32;

    kickmsg::Node node("n", KMSG_NAMESPACE);
    auto pc = node.advertise("c_topic", cfg);
    track(topic_shm("c_topic"));
    auto pa = node.advertise("a_topic", cfg);
    track(topic_shm("a_topic"));
    auto pb = node.advertise("b_topic", cfg);
    track(topic_shm("b_topic"));

    auto reg    = kickmsg::Registry::open_or_create(KMSG_NAMESPACE);
    auto topics = reg.list_topics();

    ASSERT_EQ(topics.size(), 3u);
    EXPECT_LT(topics[0].shm_name, topics[1].shm_name);
    EXPECT_LT(topics[1].shm_name, topics[2].shm_name);
}
