#include <gtest/gtest.h>

#include "kickmsg/os/Time.h"
#include "kickmsg/Publisher.h"
#include "kickmsg/Subscriber.h"

#include <thread>

using namespace kickmsg;

class SubscriberTest : public ::testing::Test
{
public:
    static constexpr char const* SHM_NAME = "/kickmsg_test_subscriber";

    void SetUp() override
    {
        kickmsg::SharedMemory::unlink(SHM_NAME);
    }

    void TearDown() override
    {
        kickmsg::SharedMemory::unlink(SHM_NAME);
    }

    kickmsg::channel::Config default_cfg()
    {
        kickmsg::channel::Config cfg;
        cfg.max_subscribers   = 4;
        cfg.sub_ring_capacity = 8;
        cfg.pool_size         = 16;
        cfg.max_payload_size  = 64;
        return cfg;
    }
};

TEST_F(SubscriberTest, ReceiveEmptyReturnsNullopt)
{
    auto cfg    = default_cfg();
    auto region = kickmsg::SharedRegion::create(SHM_NAME, kickmsg::channel::PubSub, cfg);
    kickmsg::Subscriber sub(region);

    EXPECT_FALSE(sub.try_receive().has_value());
    EXPECT_FALSE(sub.try_receive_view().has_value());
}

TEST_F(SubscriberTest, MovedFromReceiveReturnsNulloptNotCrash)
{
    auto cfg    = default_cfg();
    auto region = kickmsg::SharedRegion::create(SHM_NAME, kickmsg::channel::PubSub, cfg);
    kickmsg::Subscriber src(region);
    kickmsg::Subscriber dst(std::move(src));

    // src is moved-from (ring_idx_ == UINT32_MAX). All four receive paths
    // must return nullopt instead of dereferencing a wild ring pointer.
    EXPECT_FALSE(src.try_receive().has_value());
    EXPECT_FALSE(src.try_receive_view().has_value());
    EXPECT_FALSE(src.receive(0ms).has_value());
    EXPECT_FALSE(src.receive_view(0ms).has_value());
}

TEST_F(SubscriberTest, SampleViewMoveSemantics)
{
    auto cfg    = default_cfg();
    auto region = kickmsg::SharedRegion::create(SHM_NAME, kickmsg::channel::PubSub, cfg);

    kickmsg::Subscriber sub(region);
    kickmsg::Publisher  pub(region);

    uint32_t val = 99;
    ASSERT_GE(pub.send(&val, sizeof(val)), 0);

    auto view1 = sub.try_receive_view();
    ASSERT_TRUE(view1.has_value());

    auto view2 = std::move(*view1);
    EXPECT_EQ(view2.len(), sizeof(val));

    uint32_t got = 0;
    std::memcpy(&got, view2.data(), sizeof(got));
    EXPECT_EQ(got, 99u);

    kickmsg::Subscriber::SampleView view3;
    view3 = std::move(view2);
    EXPECT_EQ(view3.len(), sizeof(val));
}

TEST_F(SubscriberTest, DrainReleasesSlots)
{
    kickmsg::channel::Config cfg;
    cfg.max_subscribers   = 2;
    cfg.sub_ring_capacity = 8;
    cfg.pool_size         = 16;
    cfg.max_payload_size  = 64;

    auto region = kickmsg::SharedRegion::create(
                      SHM_NAME, kickmsg::channel::PubSub, cfg);
    kickmsg::Publisher pub(region);

    auto count_free = [&]()
    {
        uint32_t count = 0;
        auto*    header = region.header();
        uint64_t top = header->free_top.load(std::memory_order_acquire);
        uint32_t idx = kickmsg::tagged_idx(top);
        while (idx != kickmsg::INVALID_SLOT)
        {
            auto* slot = kickmsg::slot_at(region.base(), region.geometry(), idx);
            idx = slot->next_free;
            ++count;
        }
        return count;
    };

    {
        kickmsg::Subscriber sub(region);
        uint32_t free_before = count_free();

        for (int i = 0; i < 5; ++i)
        {
            uint32_t val = static_cast<uint32_t>(i);
            ASSERT_GE(pub.send(&val, sizeof(val)), 0);
        }

        EXPECT_EQ(count_free(), free_before - 5);
    }

    EXPECT_EQ(count_free(), 16u);
}

TEST_F(SubscriberTest, BlockingReceiveTimesOut)
{
    auto cfg    = default_cfg();
    auto region = kickmsg::SharedRegion::create(SHM_NAME, kickmsg::channel::PubSub, cfg);
    kickmsg::Subscriber sub(region);

    nanoseconds start  = kickmsg::monotonic_ns();
    auto        sample = sub.receive(50ms);
    nanoseconds elapsed = kickmsg::monotonic_ns() - start;

    EXPECT_FALSE(sample.has_value());
    EXPECT_GE(elapsed, 40ms);
}

TEST_F(SubscriberTest, BlockingReceiveWakesOnPublish)
{
    auto cfg    = default_cfg();
    auto region = kickmsg::SharedRegion::create(SHM_NAME, kickmsg::channel::PubSub, cfg);

    kickmsg::Subscriber sub(region);
    kickmsg::Publisher  pub(region);

    std::thread sender([&]()
    {
        kickmsg::sleep(20ms);
        uint32_t val = 123;
        pub.send(&val, sizeof(val));
    });

    nanoseconds start  = kickmsg::monotonic_ns();
    auto        sample = sub.receive(2s);
    EXPECT_LT(kickmsg::elapsed_time(start), 1s);
    ASSERT_TRUE(sample.has_value());

    uint32_t got = 0;
    std::memcpy(&got, sample->data(), sizeof(got));
    EXPECT_EQ(got, 123u);

    sender.join();
}

TEST_F(SubscriberTest, DrainDoesNotDoubleDecrementOnChurn)
{
    // Minimal reproducer: create subscriber, publish, destroy subscriber,
    // create new subscriber on same ring, publish more, destroy.
    // Verify all refcounts reach zero — no double-decrement.

    kickmsg::channel::Config cfg;
    cfg.max_subscribers   = 1;
    cfg.sub_ring_capacity = 4;
    cfg.pool_size         = 8;
    cfg.max_payload_size  = 8;

    auto region = kickmsg::SharedRegion::create(
        SHM_NAME, kickmsg::channel::PubSub, cfg, "drain_test");

    kickmsg::Publisher pub(region);

    // Round 1: subscriber joins, publisher publishes, subscriber destructs
    {
        kickmsg::Subscriber sub(region);
        uint32_t val = 1;
        ASSERT_GE(pub.send(&val, sizeof(val)), 0);
        ASSERT_GE(pub.send(&val, sizeof(val)), 0);
        // sub destructs here — drain releases refcounts
    }

    // Round 2: new subscriber on same ring, more publishes, destructs
    {
        kickmsg::Subscriber sub(region);
        uint32_t val = 2;
        ASSERT_GE(pub.send(&val, sizeof(val)), 0);
        ASSERT_GE(pub.send(&val, sizeof(val)), 0);
        // sub destructs here — drain must not double-decrement round 1's entries
    }

    // Round 3: one more cycle
    {
        kickmsg::Subscriber sub(region);
        uint32_t val = 3;
        ASSERT_GE(pub.send(&val, sizeof(val)), 0);
    }

    // All slots should have refcount 0
    auto* base = region.base();
    for (uint32_t i = 0; i < cfg.pool_size; ++i)
    {
        auto* slot = kickmsg::slot_at(base, region.geometry(), i);
        uint32_t rc = slot->refcount;
        EXPECT_EQ(rc, 0u) << "slot " << i << " has refcount " << rc;
    }
}

TEST_F(SubscriberTest, StuckPublisherRetiresTheRingUntilReset)
{
    kickmsg::channel::Config cfg;
    cfg.max_subscribers   = 1;
    cfg.sub_ring_capacity = 4;
    cfg.pool_size         = 8;
    cfg.max_payload_size  = 8;
    cfg.commit_timeout    = 1ms;

    auto region = kickmsg::SharedRegion::create(SHM_NAME, kickmsg::channel::PubSub, cfg);

    kickmsg::Publisher pub(region);
    auto* ring = kickmsg::sub_ring_at(region.base(), region.geometry(), 0);

    {
        kickmsg::Subscriber sub(region);

        for (int i = 0; i < 3; ++i)
        {
            uint32_t val = static_cast<uint32_t>(i);
            ASSERT_GE(pub.send(&val, sizeof(val)), 0);
        }

        // Simulate a crashed publisher still admitted on ring 0.
        ring->state_flight.fetch_add(kickmsg::ring::IN_FLIGHT_ONE,
                                     std::memory_order_acq_rel);
        // sub destructs here -- timeout, drain skipped
    }

    uint32_t packed = ring->state_flight.load(std::memory_order_acquire);
    EXPECT_EQ(kickmsg::ring::get_state(packed), kickmsg::ring::Free);
    EXPECT_GT(kickmsg::ring::get_in_flight(packed), 0u);

    EXPECT_GE(region.reset_retired_rings(), 1u);

    kickmsg::Subscriber sub(region);
    uint32_t val = 42;
    ASSERT_GE(pub.send(&val, sizeof(val)), 0);

    auto sample = sub.try_receive();
    ASSERT_TRUE(sample.has_value());

    uint32_t got = 0;
    std::memcpy(&got, sample->data(), sizeof(got));
    EXPECT_EQ(got, 42u);
}

TEST_F(SubscriberTest, MoveAssignmentRetiresAStuckOldRing)
{
    kickmsg::channel::Config cfg;
    cfg.max_subscribers   = 2;
    cfg.sub_ring_capacity = 4;
    cfg.pool_size         = 8;
    cfg.max_payload_size  = 8;
    cfg.commit_timeout    = 1ms;

    auto region = kickmsg::SharedRegion::create(SHM_NAME, kickmsg::channel::PubSub, cfg);

    kickmsg::Publisher pub(region);

    kickmsg::Subscriber sub(region);  // takes ring 0
    uint32_t val = 1;
    ASSERT_GE(pub.send(&val, sizeof(val)), 0);

    // Simulate a crashed publisher still admitted on ring 0.
    auto* ring0 = kickmsg::sub_ring_at(region.base(), region.geometry(), 0);
    ring0->state_flight.fetch_add(kickmsg::ring::IN_FLIGHT_ONE,
                                  std::memory_order_acq_rel);

    kickmsg::Subscriber fresh(region);  // takes ring 1
    sub = std::move(fresh);

    EXPECT_GE(region.diagnose().retired_rings, 1u);
    EXPECT_GE(region.reset_retired_rings(), 1u);
}

TEST_F(SubscriberTest, ConcurrentChurnRefcountIntegrity)
{
    // Publisher runs continuously while a subscriber churns on ring 0.
    // After everything stops, all refcounts must be zero.

    kickmsg::channel::Config cfg;
    cfg.max_subscribers   = 1;
    cfg.sub_ring_capacity = 8;
    cfg.pool_size         = 16;
    cfg.max_payload_size  = 8;

    auto region = kickmsg::SharedRegion::create(
        SHM_NAME, kickmsg::channel::PubSub, cfg, "churn_test");

    std::atomic<bool> done{false};

    std::thread pub_thread([&]()
    {
        kickmsg::Publisher pub(region);
        uint32_t val = 0;
        while (not done)
        {
            if (pub.send(&val, sizeof(val)) >= 0)
            {
                ++val;
            }
            else
            {
                std::this_thread::yield();
            }
        }
    });

    // Churn: create/destroy subscriber 10 times
    for (int round = 0; round < 10; ++round)
    {
        kickmsg::Subscriber sub(region);
        for (int j = 0; j < 5; ++j)
        {
            sub.try_receive();
        }
    }

    done = true;
    pub_thread.join();

    // Verify all refcounts are zero
    auto* base = region.base();
    for (uint32_t i = 0; i < cfg.pool_size; ++i)
    {
        auto* slot = kickmsg::slot_at(base, region.geometry(), i);
        uint32_t rc = slot->refcount;
        EXPECT_EQ(rc, 0u) << "slot " << i << " has refcount " << rc
                          << " (round completed, all should be 0)";
    }
}

TEST_F(SubscriberTest, SaturatedRegionRejectsAnExtraSubscriberUntilOneLeaves)
{
    auto cfg    = default_cfg();
    auto region = kickmsg::SharedRegion::create(SHM_NAME, kickmsg::channel::PubSub, cfg);

    std::vector<kickmsg::Subscriber> subs;
    for (std::size_t i = 0; i < cfg.max_subscribers; ++i)
    {
        subs.emplace_back(region);
    }
    EXPECT_THROW(kickmsg::Subscriber extra(region), std::runtime_error);

    subs.pop_back();
    EXPECT_NO_THROW(kickmsg::Subscriber replacement(region));
}
