#include <gtest/gtest.h>

#include <atomic>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

#include "kickmsg/os/Time.h"
#include "kickmsg/Publisher.h"
#include "kickmsg/Subscriber.h"
#include "kickmsg/Waker.h"

using namespace kickmsg;

class WaitAnyTest : public ::testing::Test
{
public:
    static constexpr char const* SHM_A = "/kickmsg_test_wait_any_a";
    static constexpr char const* SHM_B = "/kickmsg_test_wait_any_b";

    UdpMulticastBackend backend_a{SHM_A};
    UdpMulticastBackend backend_b{SHM_B};

    void SetUp() override
    {
        kickmsg::SharedMemory::unlink(SHM_A);
        kickmsg::SharedMemory::unlink(SHM_B);
    }

    void TearDown() override
    {
        kickmsg::SharedMemory::unlink(SHM_A);
        kickmsg::SharedMemory::unlink(SHM_B);
    }

    kickmsg::channel::Config default_cfg(char const* = SHM_A)
    {
        kickmsg::channel::Config cfg;
        cfg.max_subscribers   = 4;
        cfg.sub_ring_capacity = 8;
        cfg.pool_size         = 16;
        cfg.max_payload_size  = 64;
        return cfg;
    }
};

TEST_F(WaitAnyTest, AnEmptySetReportsTimeout)
{
    EXPECT_FALSE(wait_any(nullptr, 0, milliseconds{1}));
}

TEST_F(WaitAnyTest, TimesOutWhenEveryChannelIsQuiet)
{
    auto cfg = default_cfg();
    auto a   = SharedRegion::create(SHM_A, channel::PubSub, cfg);
    auto b   = SharedRegion::create(SHM_B, channel::PubSub, default_cfg(SHM_B));

    Subscriber  sub_a(a);
    Subscriber  sub_b(b);
    Subscriber* subs[] = {&sub_a, &sub_b};

    nanoseconds start = kickmsg::monotonic_ns();
    EXPECT_FALSE(wait_any(subs, 2, milliseconds{40}));
    EXPECT_GE(kickmsg::elapsed_time(start), milliseconds{35});
}

TEST_F(WaitAnyTest, ReturnsImmediatelyWhenASampleIsAlreadyQueued)
{
    auto cfg = default_cfg();
    auto a   = SharedRegion::create(SHM_A, channel::PubSub, cfg);
    auto b   = SharedRegion::create(SHM_B, channel::PubSub, default_cfg(SHM_B));

    Subscriber  sub_a(a);
    Subscriber  sub_b(b);
    Subscriber* subs[] = {&sub_a, &sub_b};

    Publisher pub_b(b, &backend_b);
    uint8_t   payload = 4;
    ASSERT_GT(pub_b.send(&payload, sizeof(payload)), 0);

    nanoseconds start = kickmsg::monotonic_ns();
    EXPECT_TRUE(wait_any(subs, 2, seconds{2}));
    EXPECT_LT(kickmsg::elapsed_time(start), milliseconds{20});

    EXPECT_FALSE(sub_a.try_receive().has_value());
    EXPECT_TRUE(sub_b.try_receive().has_value());
}

TEST_F(WaitAnyTest, AZeroTimeoutStillReportsAQueuedSample)
{
    auto cfg = default_cfg();
    auto a   = SharedRegion::create(SHM_A, channel::PubSub, cfg);
    auto b   = SharedRegion::create(SHM_B, channel::PubSub, default_cfg(SHM_B));

    Subscriber  sub_a(a);
    Subscriber  sub_b(b);
    Subscriber* subs[] = {&sub_a, &sub_b};

    EXPECT_FALSE(wait_any(subs, 2, nanoseconds{0}));

    Publisher pub_b(b, &backend_b);
    uint8_t   payload = 4;
    ASSERT_GT(pub_b.send(&payload, sizeof(payload)), 0);

    EXPECT_TRUE(wait_any(subs, 2, nanoseconds{0}));
    EXPECT_TRUE(sub_b.try_receive().has_value());
}

TEST_F(WaitAnyTest, ALargeQuietSetStillRespectsTheDeadline)
{
    constexpr std::size_t SUBS = 128;

    auto cfg = default_cfg();
    cfg.max_subscribers = SUBS;
    auto region = SharedRegion::create(SHM_A, channel::PubSub, cfg);

    std::vector<std::unique_ptr<Subscriber>> owned;
    std::vector<Subscriber*>                 subs;
    Waker                                    waker(backend_a);
    for (std::size_t i = 0; i < SUBS; ++i)
    {
        owned.push_back(std::make_unique<Subscriber>(region));
        ASSERT_GE(owned.back()->attach(waker), 0);
        subs.push_back(owned.back().get());
    }

    nanoseconds start = kickmsg::monotonic_ns();
    EXPECT_FALSE(wait_any(subs.data(), subs.size(), milliseconds{50}));
    nanoseconds took = kickmsg::elapsed_time(start);
    EXPECT_GE(took, milliseconds{45});
    EXPECT_LT(took, milliseconds{200});
}

TEST_F(WaitAnyTest, WakesOnWhicheverChannelPublishes)
{
    auto cfg = default_cfg();
    auto a   = SharedRegion::create(SHM_A, channel::PubSub, cfg);
    auto b   = SharedRegion::create(SHM_B, channel::PubSub, default_cfg(SHM_B));

    Subscriber  sub_a(a);
    Subscriber  sub_b(b);
    Subscriber* subs[] = {&sub_a, &sub_b};

    Waker waker_a(backend_a);
    Waker waker_b(backend_b);
    ASSERT_GE(sub_a.attach(waker_a), 0);
    ASSERT_GE(sub_b.attach(waker_b), 0);

    for (int round = 0; round < 2; ++round)
    {
        SharedRegion* target = &b;
        if (round == 0)
        {
            target = &a;
        }

        std::thread publisher([&]()
        {
            kickmsg::sleep(milliseconds{20});
            WakeBackend* target_backend = &backend_b;
            if (round == 0)
            {
                target_backend = &backend_a;
            }
            Publisher pub(*target, target_backend);
            uint8_t   payload = static_cast<uint8_t>(round);
            pub.send(&payload, sizeof(payload));
        });

        EXPECT_TRUE(wait_any(subs, 2, seconds{2}));
        publisher.join();

        if (round == 0)
        {
            EXPECT_TRUE(sub_a.try_receive().has_value());
        }
        else
        {
            EXPECT_TRUE(sub_b.try_receive().has_value());
        }
    }
}

TEST_F(WaitAnyTest, AnUnattachedSetStillDeliversWithoutACarrier)
{
    auto cfg = default_cfg();
    auto a   = SharedRegion::create(SHM_A, channel::PubSub, cfg);

    Subscriber  sub_a(a);
    Subscriber* subs[] = {&sub_a};

    std::thread publisher([&]()
    {
        kickmsg::sleep(milliseconds{20});
        Publisher pub(a, &backend_a);
        uint8_t   payload = 8;
        pub.send(&payload, sizeof(payload));
    });

    EXPECT_TRUE(wait_any(subs, 1, seconds{2}));
    publisher.join();
    EXPECT_TRUE(sub_a.try_receive().has_value());
}

TEST_F(WaitAnyTest, PollsEveryAttachedWakerNotJustTheFirst)
{
    constexpr std::size_t PER_REGION = 2;

    auto cfg = default_cfg();
    cfg.max_subscribers = PER_REGION;
    auto cfg_b = default_cfg(SHM_B);
    cfg_b.max_subscribers = PER_REGION;
    auto a = SharedRegion::create(SHM_A, channel::PubSub, cfg);
    auto b = SharedRegion::create(SHM_B, channel::PubSub, cfg_b);

    Waker waker_a(backend_a);
    Waker waker_b(backend_b);

    std::vector<std::unique_ptr<Subscriber>> owned;
    std::vector<Subscriber*>                 subs;
    for (std::size_t i = 0; i < PER_REGION; ++i)
    {
        owned.push_back(std::make_unique<Subscriber>(a));
        ASSERT_GE(owned.back()->attach(waker_a), 0);
        subs.push_back(owned.back().get());
    }
    for (std::size_t i = 0; i < PER_REGION; ++i)
    {
        owned.push_back(std::make_unique<Subscriber>(b));
        ASSERT_GE(owned.back()->attach(waker_b), 0);
        subs.push_back(owned.back().get());
    }
    // Only the second region publishes: waiting on the first waker alone would time out.
    std::thread publisher([&]()
    {
        kickmsg::sleep(milliseconds{20});
        Publisher pub(b, &backend_b);
        uint8_t   payload = 9;
        pub.send(&payload, sizeof(payload));
    });

    EXPECT_TRUE(wait_any(subs.data(), subs.size(), seconds{5}));
    publisher.join();

    std::size_t delivered = 0;
    for (auto* sub : subs)
    {
        while (sub->try_receive())
        {
            ++delivered;
        }
    }
    EXPECT_EQ(PER_REGION, delivered);
}

TEST_F(WaitAnyTest, AWaitSetLargerThanTheStackBufferStillDelivers)
{
    constexpr std::size_t SUBS = 130;

    auto cfg = default_cfg();
    cfg.max_subscribers = SUBS;
    auto region = SharedRegion::create(SHM_A, channel::PubSub, cfg);

    std::vector<std::unique_ptr<Subscriber>> owned;
    std::vector<Subscriber*>                 subs;
    for (std::size_t i = 0; i < SUBS; ++i)
    {
        owned.push_back(std::make_unique<Subscriber>(region));
        ASSERT_GE(owned.back()->wait_fd(backend_a), 0);
        subs.push_back(owned.back().get());
    }

    std::thread publisher([&]()
    {
        kickmsg::sleep(milliseconds{20});
        Publisher pub(region, &backend_a);
        uint8_t   payload = 4;
        pub.send(&payload, sizeof(payload));
    });

    nanoseconds start = kickmsg::monotonic_ns();
    EXPECT_TRUE(wait_any(subs.data(), subs.size(), seconds{5}));
    // Bounded re-peek, not a spin to the caller's deadline.
    EXPECT_LT(kickmsg::elapsed_time(start), seconds{1});
    publisher.join();

    std::size_t delivered = 0;
    for (auto* sub : subs)
    {
        while (sub->try_receive())
        {
            ++delivered;
        }
    }
    EXPECT_EQ(SUBS, delivered);
}
