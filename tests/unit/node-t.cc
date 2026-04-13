#include <cstring>
#include <string>

#include <gtest/gtest.h>

#include "kickmsg/Node.h"

class NodeTest : public ::testing::Test
{
protected:
    void TearDown() override
    {
        for (auto const& name : shm_names_)
        {
            kickmsg::SharedMemory::unlink(name);
        }
    }

    void track(std::string name)
    {
        shm_names_.push_back(std::move(name));
    }

    kickmsg::channel::Config small_cfg()
    {
        kickmsg::channel::Config cfg;
        cfg.max_subscribers   = 4;
        cfg.sub_ring_capacity = 8;
        cfg.pool_size         = 16;
        cfg.max_payload_size  = 64;
        return cfg;
    }

private:
    std::vector<std::string> shm_names_;
};

TEST_F(NodeTest, AdvertiseAndSubscribe)
{
    // Topic-centric: SHM name is /{prefix}_{topic}, no node name in path
    track("/test_data");

    kickmsg::Node pub_node("pubnode", "test");
    auto pub = pub_node.advertise("data", small_cfg());

    // Any node can subscribe by topic name alone
    kickmsg::Node sub_node("subnode", "test");
    auto sub = sub_node.subscribe("data");

    uint32_t val = 42;
    ASSERT_GE(pub.send(&val, sizeof(val)), 0);

    auto sample = sub.try_receive();
    ASSERT_TRUE(sample.has_value());

    uint32_t got = 0;
    std::memcpy(&got, sample->data(), sizeof(got));
    EXPECT_EQ(got, 42u);
}

TEST_F(NodeTest, NamingConventions)
{
    kickmsg::Node node("mynode", "app");
    EXPECT_EQ(node.name(), "mynode");
    EXPECT_EQ(node.prefix(), "app");
}

TEST_F(NodeTest, JoinBroadcastTwoNodes)
{
    track("/test_broadcast_events");

    auto cfg = small_cfg();

    kickmsg::Node node_a("nodeA", "test");
    auto [pub_a, sub_a] = node_a.join_broadcast("events", cfg);

    kickmsg::Node node_b("nodeB", "test");
    auto [pub_b, sub_b] = node_b.join_broadcast("events", cfg);

    std::string msg_a = "hello from A";
    ASSERT_GE(pub_a.send(msg_a.data(), msg_a.size()), 0);

    auto recv_b = sub_b.try_receive();
    ASSERT_TRUE(recv_b.has_value());
    EXPECT_EQ(std::string(static_cast<char const*>(recv_b->data()), recv_b->len()), msg_a);

    auto recv_a = sub_a.try_receive();
    ASSERT_TRUE(recv_a.has_value());
    EXPECT_EQ(std::string(static_cast<char const*>(recv_a->data()), recv_a->len()), msg_a);

    std::string msg_b = "reply from B";
    ASSERT_GE(pub_b.send(msg_b.data(), msg_b.size()), 0);

    auto recv_a2 = sub_a.try_receive();
    ASSERT_TRUE(recv_a2.has_value());
    EXPECT_EQ(std::string(static_cast<char const*>(recv_a2->data()), recv_a2->len()), msg_b);
}

TEST_F(NodeTest, MailboxPattern)
{
    track("/test_nodeA_mbx_inbox");

    auto cfg = small_cfg();

    kickmsg::Node node_a("nodeA", "test");
    auto inbox = node_a.create_mailbox("inbox", cfg);

    kickmsg::Node node_b("nodeB", "test");
    auto reply_pub = node_b.open_mailbox("nodeA", "inbox");

    std::string reply = "version 1.0";
    ASSERT_GE(reply_pub.send(reply.data(), reply.size()), 0);

    auto msg = inbox.try_receive();
    ASSERT_TRUE(msg.has_value());
    EXPECT_EQ(std::string(static_cast<char const*>(msg->data()), msg->len()), reply);
}

// --- Schema descriptor through the Node API -------------------------------

namespace
{
    kickmsg::SchemaInfo make_node_schema(char const* name, uint32_t version,
                                         uint8_t identity_fill)
    {
        kickmsg::SchemaInfo s{};
        std::fill(s.identity.begin(), s.identity.end(), identity_fill);
        std::snprintf(s.name, sizeof(s.name), "%s", name);
        s.version       = version;
        s.identity_algo = 1;
        return s;
    }
}

TEST_F(NodeTest, TopicSchemaBakedViaAdvertise)
{
    track("/test_imu");

    auto cfg = small_cfg();
    cfg.schema = make_node_schema("app/Imu", 2, 0xAA);

    kickmsg::Node pub_node("driver", "test");
    auto pub = pub_node.advertise("imu", cfg);

    kickmsg::Node sub_node("logger", "test");
    auto sub = sub_node.subscribe("imu");

    auto got = sub_node.topic_schema("imu");
    ASSERT_TRUE(got.has_value());
    EXPECT_STREQ(got->name, "app/Imu");
    EXPECT_EQ(got->version, 2u);
    EXPECT_EQ(got->identity[0], 0xAA);
}

TEST_F(NodeTest, TopicSchemaReturnsNulloptForUnknownTopic)
{
    // Guard on find_region: unknown topic produces nullopt, not a throw
    // or segfault.  Also covers the default-constructed SchemaInfo path.
    kickmsg::Node node("orphan", "test");
    EXPECT_FALSE(node.topic_schema("never_joined").has_value());
}

TEST_F(NodeTest, TryClaimTopicSchemaLateBinding)
{
    // Late-arrival flow: subscriber creates the region, publisher arrives
    // and claims the schema via the Node API.
    track("/test_telemetry");

    auto cfg = small_cfg();

    kickmsg::Node listener("listener", "test");
    auto sub = listener.subscribe_or_create("telemetry", cfg);
    EXPECT_FALSE(listener.topic_schema("telemetry").has_value());

    kickmsg::Node driver("driver", "test");
    auto pub = driver.advertise_or_join("telemetry", cfg);

    auto info = make_node_schema("app/Telemetry", 1, 0xCC);
    EXPECT_TRUE(driver.try_claim_topic_schema("telemetry", info));

    auto observed = listener.topic_schema("telemetry");
    ASSERT_TRUE(observed.has_value());
    EXPECT_STREQ(observed->name, "app/Telemetry");

    // Second claimant on the same topic is rejected; first claim stands.
    auto other = make_node_schema("other/Type", 99, 0xDD);
    EXPECT_FALSE(driver.try_claim_topic_schema("telemetry", other));
    EXPECT_STREQ(driver.topic_schema("telemetry")->name, "app/Telemetry");
}

TEST_F(NodeTest, TryClaimTopicSchemaUnknownTopicIsFalse)
{
    kickmsg::Node node("orphan", "test");
    auto info = make_node_schema("stray/Type", 1, 0x00);
    EXPECT_FALSE(node.try_claim_topic_schema("never_joined", info));
}

TEST_F(NodeTest, UnlinkTopicRemovesShm)
{
    // Platform note: POSIX shm_unlink removes the name immediately even
    // while mappings are held; Windows named mappings auto-destroy only
    // when the last handle closes, making unlink() a no-op there.  To
    // keep the assertion portable we release every handle BEFORE calling
    // unlink, then verify a fresh open() fails.  On Linux this exercises
    // the unlink path (without it, the /dev/shm entry would persist); on
    // Windows the last-handle-close already removed the mapping and
    // unlink is a harmless no-op — both produce the same post-condition.
    track("/test_ephemeral");

    {
        kickmsg::Node node("node", "test");
        auto pub = node.advertise("ephemeral", small_cfg());
        (void)pub;
    }  // all handles released here

    kickmsg::Node cleanup("cleanup", "test");
    cleanup.unlink_topic("ephemeral");

    // Fresh open() on the unlinked name must fail.
    EXPECT_THROW(kickmsg::SharedRegion::open("/test_ephemeral"),
                 std::runtime_error);
}

TEST_F(NodeTest, UnlinkBroadcastAndMailboxUseRightNames)
{
    // Regression guard: unlink_* must format names the same way as the
    // create/open side.  Formatting drift would silently leak SHM
    // entries between test runs.  Same platform-portability structure
    // as UnlinkTopicRemovesShm above.
    {
        kickmsg::Node node("owner", "test");
        auto bc = node.join_broadcast("events", small_cfg());
        (void)bc;
    }
    {
        kickmsg::Node cleanup("cleanup", "test");
        cleanup.unlink_broadcast("events");
    }
    EXPECT_THROW(kickmsg::SharedRegion::open("/test_broadcast_events"),
                 std::runtime_error);

    {
        kickmsg::Node owner("owner", "test");
        auto mbx = owner.create_mailbox("inbox", small_cfg());
        (void)mbx;
    }
    {
        // Unlink from a different node — unlink_mailbox takes an explicit
        // owner, which is also what a cleanup tool in a separate process
        // would have to supply.
        kickmsg::Node cleanup("cleanup", "test");
        cleanup.unlink_mailbox("inbox", "owner");
    }
    EXPECT_THROW(kickmsg::SharedRegion::open("/test_owner_mbx_inbox"),
                 std::runtime_error);
}

TEST_F(NodeTest, SubscribeOrCreateTwiceReusesSameRegion)
{
    // Idempotency contract: repeated subscribe_or_create / advertise_or_join
    // on the same topic yields independent handles that wrap the SAME
    // underlying mmap (emplace_or_reuse dedupes).  A publisher on one
    // handle must be visible to a subscriber on either.
    track("/test_shared");

    kickmsg::Node node("node", "test");
    auto cfg = small_cfg();

    auto sub1 = node.subscribe_or_create("shared", cfg);
    auto pub  = node.advertise_or_join("shared",   cfg);
    auto sub2 = node.subscribe_or_create("shared", cfg);

    uint32_t val = 99;
    ASSERT_GE(pub.send(&val, sizeof(val)), 0);

    auto s1 = sub1.try_receive();
    ASSERT_TRUE(s1.has_value());
    uint32_t got1 = 0;
    std::memcpy(&got1, s1->data(), sizeof(got1));
    EXPECT_EQ(got1, 99u);

    auto s2 = sub2.try_receive();
    ASSERT_TRUE(s2.has_value());
    uint32_t got2 = 0;
    std::memcpy(&got2, s2->data(), sizeof(got2));
    EXPECT_EQ(got2, 99u);

    // Both handles see the same schema slot.
    auto info = make_node_schema("shared/Type", 1, 0x77);
    EXPECT_TRUE(node.try_claim_topic_schema("shared", info));

    auto schema_view = node.topic_schema("shared");
    ASSERT_TRUE(schema_view.has_value());
    EXPECT_STREQ(schema_view->name, "shared/Type");
}

TEST_F(NodeTest, MailboxMultipleWriters)
{
    track("/test_owner_mbx_inbox");

    auto cfg = small_cfg();

    kickmsg::Node owner("owner", "test");
    auto inbox = owner.create_mailbox("inbox", cfg);

    kickmsg::Node writer1("w1", "test");
    auto pub1 = writer1.open_mailbox("owner", "inbox");

    kickmsg::Node writer2("w2", "test");
    auto pub2 = writer2.open_mailbox("owner", "inbox");

    std::string m1 = "from w1";
    std::string m2 = "from w2";
    ASSERT_GE(pub1.send(m1.data(), m1.size()), 0);
    ASSERT_GE(pub2.send(m2.data(), m2.size()), 0);

    auto r1 = inbox.try_receive();
    ASSERT_TRUE(r1.has_value());
    std::string got1(static_cast<char const*>(r1->data()), r1->len());

    auto r2 = inbox.try_receive();
    ASSERT_TRUE(r2.has_value());
    std::string got2(static_cast<char const*>(r2->data()), r2->len());

    EXPECT_TRUE((got1 == m1 && got2 == m2) || (got1 == m2 && got2 == m1));
}
