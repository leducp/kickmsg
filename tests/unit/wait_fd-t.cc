#include <gtest/gtest.h>

#include <cstring>
#include <utility>

#include <thread>

#include "kickmsg/os/Time.h"
#include "kickmsg/Node.h"
#include "kickmsg/Publisher.h"
#include "kickmsg/Waker.h"
#include "kickmsg/WaitSet.h"
#include "kickmsg/Subscriber.h"

using namespace kickmsg;

class WaitFdTest : public ::testing::Test
{
public:
    static constexpr char const* SHM_NAME  = "/kickmsg_test_wait_fd";
    static constexpr char const* SHM_OTHER = "/kickmsg_test_wait_fd_other";

    void SetUp() override
    {
        kickmsg::SharedMemory::unlink(SHM_NAME);
        kickmsg::SharedMemory::unlink(SHM_OTHER);
    }

    void TearDown() override
    {
        kickmsg::SharedMemory::unlink(SHM_NAME);
        kickmsg::SharedMemory::unlink(SHM_OTHER);
    }

    UdpMulticastBackend backend{SHM_NAME};
    UdpMulticastBackend other_backend{SHM_OTHER};
    Waker               waker{backend};
    Waker               other_waker{other_backend};

    kickmsg::channel::Config bare_cfg()
    {
        kickmsg::channel::Config cfg;
        cfg.max_subscribers   = 4;
        cfg.sub_ring_capacity = 8;
        cfg.pool_size         = 16;
        cfg.max_payload_size  = 64;
        return cfg;
    }

    static bool readable(int fd, nanoseconds timeout)
    {
        kickmsg::WaitSet set;
        set.add_native(fd);
        return set.wait(timeout);
    }
};
TEST_F(WaitFdTest, UnusedSubscriberLeavesTheRingOnTheFutexPath)
{
    auto region = SharedRegion::create(SHM_NAME, channel::PubSub, bare_cfg());
    Subscriber sub(region);

    auto* ring = sub_ring_at(region.base(), region.header(), sub.ring_index());

    std::thread publisher([&]()
    {
        kickmsg::sleep(20ms);
        Publisher pub(region, &backend);
        uint8_t   payload = 42;
        pub.send(&payload, sizeof(payload));
    });

    auto sample = sub.receive(2s);
    publisher.join();

    ASSERT_TRUE(sample.has_value());
    EXPECT_EQ(ring::WaiterNone, ring->has_waiter.load());
}

TEST_F(WaitFdTest, TwoBackendsForOneNameAgreeWithoutCoordinating)
{
    UdpMulticastBackend publisher_side{SHM_NAME};
    UdpMulticastBackend subscriber_side{SHM_NAME};
    EXPECT_EQ(publisher_side.group(), subscriber_side.group());
    EXPECT_EQ(publisher_side.port(), subscriber_side.port());

    // Administratively scoped: routers never forward it off this host.
    EXPECT_EQ(0xEFFF0000u, publisher_side.group() & 0xFFFF0000u);
    EXPECT_GE(publisher_side.port(), UdpMulticastBackend::DEFAULT_PORT_BASE);
    EXPECT_LT(publisher_side.port(),
              UdpMulticastBackend::DEFAULT_PORT_BASE + UdpMulticastBackend::PORT_SPAN);

    // A different channel lands on a different port, which is what isolates them.
    UdpMulticastBackend elsewhere{SHM_OTHER};
    EXPECT_NE(publisher_side.port(), elsewhere.port());
}

TEST_F(WaitFdTest, TheUdpBackendRejectsAnAddressItCannotDeliverTo)
{
    // Port 0 binds an ephemeral port but is a literal sendto destination.
    EXPECT_THROW(UdpMulticastBackend(0xEFFF1234u, 0), std::invalid_argument);

    // Unicast: binds and sends, but membership is what fans a wake out.
    EXPECT_THROW(UdpMulticastBackend(0x08080808u, 27182), std::invalid_argument);

    // No room left for PORT_SPAN: the port would wrap through uint16_t.
    EXPECT_THROW(UdpMulticastBackend(SHM_NAME, 65535), std::invalid_argument);
    EXPECT_THROW(UdpMulticastBackend(SHM_NAME, 0), std::invalid_argument);

    // The boundary that still fits.
    EXPECT_NO_THROW(UdpMulticastBackend(SHM_NAME,
        static_cast<uint16_t>(65535 - UdpMulticastBackend::PORT_SPAN + 1)));
    EXPECT_NO_THROW(UdpMulticastBackend(0xE0000001u, 1));
}

namespace foreign
{
    /// A type from another library: no kickmsg base, no kickmsg header, private
    /// descriptor. Opting in is one hidden friend.
    struct Source
    {
        explicit Source(int fd = 7) : fd_{fd} {}

        friend int wait_descriptor(Source const& s) { return s.fd_; }

        int fd_;
    };

    /// Waitable, but with nothing to offer right now.
    struct Absent
    {
        friend int wait_descriptor(Absent const&) { return -1; }
    };

    struct NotWaitable {};
}

static_assert(kickmsg::Waitable<kickmsg::Subscriber>);
static_assert(kickmsg::Waitable<kickmsg::Waker>);
static_assert(kickmsg::Waitable<foreign::Source>);
static_assert(not kickmsg::Waitable<foreign::NotWaitable>);

TEST(WaitSetTest, AForeignTypeOptsInWithoutInheritingAnything)
{
    WaitSet set;
    set.add(foreign::Source{});
    EXPECT_EQ(1u, set.size());

    // Same descriptor: polling one twice lets one reader consume another's wake.
    set.add(foreign::Source{});
    EXPECT_EQ(1u, set.size());

    set.add(foreign::Absent{});
    EXPECT_EQ(1u, set.size());
}

TEST(WaitSetTest, AnEmptySetDoesNotBlock)
{
    WaitSet set;
    set.add(foreign::Absent{});
    ASSERT_TRUE(set.empty());

    nanoseconds start = kickmsg::monotonic_ns();
    EXPECT_FALSE(set.wait(2s));
    EXPECT_LT(kickmsg::elapsed_time(start), 100ms);
}

TEST_F(WaitFdTest, AGenericWaiterFindsTheDescriptorThroughAdl)
{
    auto region = SharedRegion::create(SHM_NAME, channel::PubSub, bare_cfg());
    Subscriber sub(region);

    // Unqualified, as WaitSet::add calls it.
    EXPECT_EQ(-1, wait_descriptor(sub));

    ASSERT_TRUE(sub.attach(waker));
    int const fd = wait_descriptor(sub);
    ASSERT_GE(fd, 0);

    Waker shared(backend);
    EXPECT_GE(wait_descriptor(shared), 0);
    EXPECT_NE(fd, wait_descriptor(shared));

    // One set holds a kickmsg source and a foreign one, neither library knowing about
    // the other and no descriptor named here.
    // Distinct by construction: a fixed literal can collide with the real descriptor,
    // and the set would dedupe them into one.
    WaitSet set;
    set.add(sub);
    set.add(foreign::Source{fd + 1});
    EXPECT_EQ(2u, set.size());
}

TEST_F(WaitFdTest, ArmMarksTheRingAsCarrierArmed)
{
    auto region = SharedRegion::create(SHM_NAME, channel::PubSub, bare_cfg());
    Subscriber sub(region);
    ASSERT_TRUE(sub.attach(waker));

    auto* ring = sub_ring_at(region.base(), region.header(), sub.ring_index());
    EXPECT_EQ(ring::WaiterNone, ring->has_waiter.load());
    ASSERT_EQ(Subscriber::Wait::Armed, sub.arm_wait());
    EXPECT_EQ(ring::WaiterCarrier, ring->has_waiter.load());
    sub.disarm_wait();
    EXPECT_EQ(ring::WaiterNone, ring->has_waiter.load());
}

TEST_F(WaitFdTest, DisarmIsIdempotent)
{
    auto region = SharedRegion::create(SHM_NAME, channel::PubSub, bare_cfg());
    Subscriber sub(region);
    ASSERT_TRUE(sub.attach(waker));

    auto* ring = sub_ring_at(region.base(), region.header(), sub.ring_index());
    ASSERT_EQ(Subscriber::Wait::Armed, sub.arm_wait());
    sub.disarm_wait();
    sub.disarm_wait();
    sub.disarm_wait();
    EXPECT_EQ(ring::WaiterNone, ring->has_waiter.load());
}

TEST_F(WaitFdTest, ArmOnEmptyRingParksAndDoesNotSignal)
{
    auto region = SharedRegion::create(SHM_NAME, channel::PubSub, bare_cfg());
    Subscriber sub(region);
    ASSERT_TRUE(sub.attach(waker));

    EXPECT_EQ(Subscriber::Wait::Armed, sub.arm_wait());
    EXPECT_FALSE(readable(wait_descriptor(sub), 20ms));
    sub.disarm_wait();
}

TEST_F(WaitFdTest, ArmReportsReadyWhenASampleIsAlreadyQueued)
{
    auto region = SharedRegion::create(SHM_NAME, channel::PubSub, bare_cfg());
    Subscriber sub(region);
    ASSERT_TRUE(sub.attach(waker));

    Publisher pub(region, &backend);
    uint8_t   payload = 7;
    ASSERT_GT(pub.send(&payload, sizeof(payload)), 0);

    EXPECT_EQ(Subscriber::Wait::Ready, sub.arm_wait());
    // Ready must not have armed the ring: no publisher should be sending.
    auto* ring = sub_ring_at(region.base(), region.header(), sub.ring_index());
    EXPECT_EQ(ring::WaiterNone, ring->has_waiter.load());
    sub.disarm_wait();

    ASSERT_TRUE(sub.try_receive().has_value());
}

TEST_F(WaitFdTest, PublishWhileArmedMakesTheDescriptorReadable)
{
    auto region = SharedRegion::create(SHM_NAME, channel::PubSub, bare_cfg());
    Subscriber sub(region);
    ASSERT_TRUE(sub.attach(waker));

    ASSERT_EQ(Subscriber::Wait::Armed, sub.arm_wait());

    std::thread publisher([&]()
    {
        kickmsg::sleep(20ms);
        Publisher pub(region, &backend);
        uint8_t   payload = 99;
        pub.send(&payload, sizeof(payload));
    });

    EXPECT_TRUE(readable(wait_descriptor(sub), 2s));
    publisher.join();

    sub.disarm_wait();
    auto sample = sub.try_receive();
    ASSERT_TRUE(sample.has_value());
    EXPECT_EQ(99, *static_cast<uint8_t const*>(sample->data()));
}

TEST_F(WaitFdTest, APublisherWithNoBackendSendsNoWake)
{
    auto region = SharedRegion::create(SHM_NAME, channel::PubSub, bare_cfg());
    Subscriber sub(region);
    ASSERT_TRUE(sub.attach(waker));
    int const fd = wait_descriptor(sub);
    ASSERT_EQ(Subscriber::Wait::Armed, sub.arm_wait());

    // No backend is the default: the sample lands, the waiter falls back to its
    // deadline.
    Publisher pub(region);
    uint8_t   payload = 1;
    ASSERT_GT(pub.send(&payload, sizeof(payload)), 0);

    EXPECT_FALSE(readable(fd, 50ms));
    sub.disarm_wait();
    EXPECT_TRUE(sub.try_receive().has_value());
}

// Draining is the Waker owner's job, not disarm_wait's: several Subscribers may share
// one, and disarming any of them must not swallow another's wake.
TEST_F(WaitFdTest, TheWakerOwnerDrainsIt)
{
    auto region = SharedRegion::create(SHM_NAME, channel::PubSub, bare_cfg());
    Subscriber sub(region);
    ASSERT_TRUE(sub.attach(waker));

    ASSERT_EQ(Subscriber::Wait::Armed, sub.arm_wait());
    Publisher pub(region, &backend);
    uint8_t   payload = 1;
    ASSERT_GT(pub.send(&payload, sizeof(payload)), 0);
    ASSERT_TRUE(readable(wait_descriptor(sub), 2s));

    sub.disarm_wait();
    ASSERT_TRUE(sub.try_receive().has_value());

    // Still readable: the wake is still queued until its owner takes it.
    EXPECT_TRUE(readable(wait_descriptor(sub), 20ms));
    waker.drain();
    EXPECT_FALSE(readable(wait_descriptor(sub), 20ms));
}

TEST_F(WaitFdTest, OneWakerServesSeveralSubscribers)
{
    auto region = SharedRegion::create(SHM_NAME, channel::PubSub, bare_cfg());

    Waker      waker(backend);
    Subscriber first(region);
    Subscriber second(region);

    ASSERT_TRUE(first.attach(waker));
    ASSERT_TRUE(second.attach(waker));
    int const fd = wait_descriptor(waker);

    ASSERT_EQ(Subscriber::Wait::Armed, first.arm_wait());
    ASSERT_EQ(Subscriber::Wait::Armed, second.arm_wait());

    Publisher pub(region, &backend);
    uint8_t   payload = 3;
    ASSERT_GT(pub.send(&payload, sizeof(payload)), 0);

    EXPECT_TRUE(readable(fd, 2s));

    // A shared waker is drained by its owner, not by disarm_wait.
    first.disarm_wait();
    second.disarm_wait();
    waker.drain();

    EXPECT_TRUE(first.try_receive().has_value());
    EXPECT_TRUE(second.try_receive().has_value());
    EXPECT_FALSE(readable(fd, 20ms));
}
// A shared port would not do: a socket bound to INADDR_ANY receives every datagram on
// its port, including groups it never joined.
TEST_F(WaitFdTest, AWakeOnOneChannelDoesNotReachAnother)
{
    auto first  = SharedRegion::create(SHM_NAME,  channel::PubSub, bare_cfg());
    auto second = SharedRegion::create(SHM_OTHER, channel::PubSub, bare_cfg());

    Subscriber sub_first(first);
    Subscriber sub_second(second);
    ASSERT_TRUE(sub_first.attach(waker));
    int const quiet_fd = (sub_second.attach(other_waker), wait_descriptor(sub_second));
    ASSERT_GE(quiet_fd, 0);

    ASSERT_EQ(Subscriber::Wait::Armed, sub_first.arm_wait());
    ASSERT_EQ(Subscriber::Wait::Armed, sub_second.arm_wait());

    Publisher pub(first, &backend);
    uint8_t   payload = 5;
    ASSERT_GT(pub.send(&payload, sizeof(payload)), 0);

    EXPECT_TRUE(readable(wait_descriptor(sub_first), 2s));
    EXPECT_FALSE(readable(quiet_fd, 50ms));

    sub_first.disarm_wait();
    sub_second.disarm_wait();
}

TEST_F(WaitFdTest, ReclaimingARingClearsAStaleWaiterMode)
{
    auto region = SharedRegion::create(SHM_NAME, channel::PubSub, bare_cfg());

    SubRingHeader* ring = nullptr;
    {
        Subscriber sub(region);
        ASSERT_TRUE(sub.attach(waker));
        ring = sub_ring_at(region.base(), region.header(), sub.ring_index());
        ASSERT_EQ(Subscriber::Wait::Armed, sub.arm_wait());
        ASSERT_EQ(ring::WaiterCarrier, ring->has_waiter.load());
        // Out of scope still armed, as a killed process would be.
    }
    EXPECT_EQ(ring::WaiterNone, ring->has_waiter.load());

    // And the futex mode, down the sweeper's path rather than the owner's.
    ring->has_waiter.store(ring::WaiterFutex, std::memory_order_relaxed);
    ring->state_flight.store(ring::make_packed(ring::Free, 1), std::memory_order_release);
    ASSERT_EQ(1u, region.reset_retired_rings());
    EXPECT_EQ(ring::WaiterNone, ring->has_waiter.load());
}

TEST_F(WaitFdTest, AttachAfterWaitFdReplacesThePrivateWaker)
{
    auto region = SharedRegion::create(SHM_NAME, channel::PubSub, bare_cfg());
    Subscriber sub(region);

    ASSERT_TRUE(sub.attach(waker));
    int const private_fd = wait_descriptor(sub);
    ASSERT_GE(private_fd, 0);

    Waker shared(backend);
    ASSERT_TRUE(sub.attach(shared));
    EXPECT_EQ(wait_descriptor(shared), wait_descriptor(sub));
    EXPECT_NE(private_fd, wait_descriptor(shared));

    ASSERT_EQ(Subscriber::Wait::Armed, sub.arm_wait());
    Publisher pub(region, &backend);
    uint8_t   payload = 6;
    ASSERT_GT(pub.send(&payload, sizeof(payload)), 0);

    EXPECT_TRUE(readable(wait_descriptor(shared), 2s));
    sub.disarm_wait();
    shared.drain();
    EXPECT_TRUE(sub.try_receive().has_value());
}
namespace
{
    /// A backend a caller owning both ends supplies to each. The publisher only ever
    /// calls signal(), so proving injection needs no descriptor and no transport.
    class CountingBackend final : public WakeBackend
    {
    public:
        int  open()      override { return -1; }
        void close(int)  override {}
        void drain(int)  override {}
        void signal()    override { ++signals; }

        int signals{0};
    };
}

namespace
{
    /// Counts signals while handing the descriptor side to a backend that works, so a
    /// Subscriber can really arm and the publisher really reaches it.
    class SpyBackend final : public WakeBackend
    {
    public:
        explicit SpyBackend(WakeBackend& real) : real_{&real} {}

        int  open()         override { return real_->open(); }
        void close(int fd)  override { real_->close(fd); }
        void drain(int fd)  override { real_->drain(fd); }
        void signal()       override { ++signals; }

        int signals{0};

    private:
        WakeBackend* real_;
    };
}

// Every factory handing out a Publisher must be able to carry a backend. One that cannot
// leaves the subscriber waiting out its whole timeout with nothing to show why.
TEST_F(WaitFdTest, EveryPublisherFactoryCarriesABackend)
{
    SpyBackend spy(backend);
    Node       node("wake_factories", "wake_ns");

    auto scrub = [&]
    {
        node.unlink_topic("factory_advertise");
        node.unlink_topic("factory_join");
        node.unlink_broadcast("factory_bcast");
        node.unlink_mailbox("factory_box");
        node.unlink_mailbox("factory_box_or");
    };
    scrub();

    channel::Config cfg     = bare_cfg();
    uint8_t         payload = 1;

    auto publish_through = [&](Publisher pub, Subscriber sub)
    {
        Waker w(spy);
        ASSERT_TRUE(sub.attach(w));
        ASSERT_EQ(Subscriber::Wait::Armed, sub.arm_wait());
        ASSERT_GT(pub.send(&payload, sizeof(payload)), 0);
        sub.disarm_wait();
    };

    // Each subscriber is created in its own statement: open_mailbox() demands an existing
    // mailbox, and as call arguments the two sides evaluate in an unspecified order.
    Publisher  adv     = node.advertise("factory_advertise", cfg, &spy);
    Subscriber adv_sub = node.subscribe_or_create("factory_advertise", cfg);
    publish_through(std::move(adv), std::move(adv_sub));
    EXPECT_EQ(1, spy.signals) << "advertise() dropped the backend";

    Publisher  join     = node.advertise_or_join("factory_join", cfg, &spy);
    Subscriber join_sub = node.subscribe_or_create("factory_join", cfg);
    publish_through(std::move(join), std::move(join_sub));
    EXPECT_EQ(2, spy.signals) << "advertise_or_join() dropped the backend";

    BroadcastHandle bcast = node.join_broadcast("factory_bcast", cfg, &spy);
    publish_through(std::move(bcast.pub), std::move(bcast.sub));
    EXPECT_EQ(3, spy.signals) << "join_broadcast() dropped the backend";

    Subscriber box     = node.create_mailbox("factory_box", cfg);
    Publisher  box_pub = node.open_mailbox("wake_factories", "factory_box", &spy);
    publish_through(std::move(box_pub), std::move(box));
    EXPECT_EQ(4, spy.signals) << "open_mailbox() dropped the backend";

    Subscriber box_or     = node.create_or_open_mailbox("factory_box_or", cfg);
    Publisher  box_or_pub = node.open_or_create_mailbox("wake_factories", "factory_box_or", cfg, &spy);
    publish_through(std::move(box_or_pub), std::move(box_or));
    EXPECT_EQ(5, spy.signals) << "open_or_create_mailbox() dropped the backend";

    scrub();
}

TEST_F(WaitFdTest, ACallerSuppliedBackendIsSignalled)
{
    auto region = SharedRegion::create(SHM_NAME, channel::PubSub, bare_cfg());
    Subscriber sub(region);

    // Stands in for a subscriber armed on the carrier, which is all the publisher reads.
    auto* ring = sub_ring_at(region.base(), region.header(), sub.ring_index());
    ring->has_waiter.store(ring::WaiterCarrier, std::memory_order_relaxed);

    CountingBackend injected;
    Publisher       pub(region, &injected);
    uint8_t         payload = 11;
    ASSERT_GT(pub.send(&payload, sizeof(payload)), 0);

    EXPECT_EQ(1, injected.signals);
    ring->has_waiter.store(ring::WaiterNone, std::memory_order_relaxed);
    EXPECT_TRUE(sub.try_receive().has_value());
}

TEST_F(WaitFdTest, APublisherWithNoBackendSignalsNothing)
{
    auto region = SharedRegion::create(SHM_NAME, channel::PubSub, bare_cfg());
    Subscriber sub(region);

    auto* ring = sub_ring_at(region.base(), region.header(), sub.ring_index());
    ring->has_waiter.store(ring::WaiterCarrier, std::memory_order_relaxed);

    CountingBackend injected;
    Publisher       pub(region);
    uint8_t         payload = 1;
    ASSERT_GT(pub.send(&payload, sizeof(payload)), 0);

    EXPECT_EQ(0, injected.signals);
    ring->has_waiter.store(ring::WaiterNone, std::memory_order_relaxed);
}
