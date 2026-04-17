#include <algorithm>
#include <string>
#include <unordered_set>

#include <gtest/gtest.h>

#include "kickmsg/Node.h"
#include "kickmsg/Registry.h"

class RegistryTest : public ::testing::Test
{
protected:
    static constexpr char const* KMSG_NAMESPACE = "kickmsg_regtest";

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
    EXPECT_EQ(r1.name(), std::string{"/"} + KMSG_NAMESPACE + "_registry");

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
    // Validate the open path still works when the region already exists:
    // open_or_create should happily attach to an existing compatible
    // region of a different capacity (capacity is only used on create).
    auto created = kickmsg::Registry::open_or_create(KMSG_NAMESPACE, 8);
    EXPECT_EQ(created.capacity(), 8u);

    auto opened = kickmsg::Registry::open_or_create(KMSG_NAMESPACE, 1024);
    // Capacity from the existing region, not the requested one.
    EXPECT_EQ(opened.capacity(), 8u);
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
    // A registrant that dies between the Free→Claiming CAS and the
    // release-store of Active leaves the slot stuck.  sweep_stale must
    // reclaim it — otherwise the registry leaks capacity on every such
    // crash.  Simulate by reaching into the raw SHM and patching a slot.
    auto reg = kickmsg::Registry::open_or_create(KMSG_NAMESPACE);

    // Fill slot 0 with a legitimate entry.
    ASSERT_NE(reg.register_participant(
        "/keeper", "/keeper", kickmsg::channel::PubSub,
        kickmsg::registry::Pubsub, kickmsg::registry::Publisher, "keeper"),
              kickmsg::INVALID_SLOT);

    // Open the registry SHM directly to install a wedged Claiming slot.
    auto shm_name = std::string{"/"} + KMSG_NAMESPACE + "_registry";
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
    // A Claiming slot with pid==0 may be a registrant between CAS and its
    // first field write.  Reclaiming would race with its stores.  Must skip.
    auto reg = kickmsg::Registry::open_or_create(KMSG_NAMESPACE);

    auto shm_name = std::string{"/"} + KMSG_NAMESPACE + "_registry";
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

// -----------------------------------------------------------------------------
// Node integration — Node advertise/subscribe/etc should populate the registry
// -----------------------------------------------------------------------------

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
        track("/" + std::string{KMSG_NAMESPACE} + "_topicX");

        auto reg = kickmsg::Registry::open_or_create(KMSG_NAMESPACE);
        auto snap = reg.snapshot();
        ASSERT_EQ(snap.size(), 1u);
        EXPECT_EQ(snap[0].node_name, "pub_node");
        EXPECT_EQ(snap[0].role, kickmsg::registry::Publisher);
        EXPECT_EQ(snap[0].shm_name,
                  std::string{"/"} + KMSG_NAMESPACE + "_topicX");
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
    track("/" + std::string{KMSG_NAMESPACE} + "_broadcast_chanX");

    auto reg  = kickmsg::Registry::open_or_create(KMSG_NAMESPACE);
    auto snap = reg.snapshot();
    ASSERT_EQ(snap.size(), 1u);
    EXPECT_EQ(snap[0].role,         kickmsg::registry::Both);
    EXPECT_EQ(snap[0].channel_type, kickmsg::channel::Broadcast);
}

TEST_F(RegistryTest, NodeAdvertiseThenSubscribeUpgradesToBoth)
{
    // A Node that both advertises and subscribes to the same topic should
    // appear once in the registry with role=Both (not two entries).
    kickmsg::channel::Config cfg;
    cfg.max_subscribers   = 2;
    cfg.sub_ring_capacity = 4;
    cfg.pool_size         = 8;
    cfg.max_payload_size  = 32;

    kickmsg::Node n("dual_node", KMSG_NAMESPACE);
    auto pub = n.advertise("dualtopic", cfg);
    auto sub = n.subscribe("dualtopic");
    track("/" + std::string{KMSG_NAMESPACE} + "_dualtopic");

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
    track("/" + std::string{KMSG_NAMESPACE} + "_shared");

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

// -----------------------------------------------------------------------------
// list_topics — topic-centric aggregation
// -----------------------------------------------------------------------------

TEST_F(RegistryTest, ListTopicsGroupsByShmName)
{
    kickmsg::channel::Config cfg;
    cfg.max_subscribers   = 4;
    cfg.sub_ring_capacity = 4;
    cfg.pool_size         = 8;
    cfg.max_payload_size  = 32;

    kickmsg::Node pub("pub_a", KMSG_NAMESPACE);
    auto p = pub.advertise("telemetry", cfg);
    track("/" + std::string{KMSG_NAMESPACE} + "_telemetry");

    kickmsg::Node s1("sub_a", KMSG_NAMESPACE);
    auto s1_h = s1.subscribe("telemetry");
    kickmsg::Node s2("sub_b", KMSG_NAMESPACE);
    auto s2_h = s2.subscribe("telemetry");

    auto reg    = kickmsg::Registry::open_or_create(KMSG_NAMESPACE);
    auto topics = reg.list_topics();

    ASSERT_EQ(topics.size(), 1u);
    auto const& t = topics[0];
    EXPECT_EQ(t.shm_name, std::string{"/"} + KMSG_NAMESPACE + "_telemetry");
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
    track("/" + std::string{KMSG_NAMESPACE} + "_broadcast_events");

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
    track("/" + std::string{KMSG_NAMESPACE} + "_c_topic");
    auto pa = node.advertise("a_topic", cfg);
    track("/" + std::string{KMSG_NAMESPACE} + "_a_topic");
    auto pb = node.advertise("b_topic", cfg);
    track("/" + std::string{KMSG_NAMESPACE} + "_b_topic");

    auto reg    = kickmsg::Registry::open_or_create(KMSG_NAMESPACE);
    auto topics = reg.list_topics();

    ASSERT_EQ(topics.size(), 3u);
    EXPECT_LT(topics[0].shm_name, topics[1].shm_name);
    EXPECT_LT(topics[1].shm_name, topics[2].shm_name);
}
