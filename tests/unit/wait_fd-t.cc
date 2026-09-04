#include <gtest/gtest.h>

#include <cstring>

#include <unistd.h>

#include <thread>

#include "kickmsg/os/Time.h"
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
        kickmsg::sleep(milliseconds{20});
        Publisher pub(region, &backend);
        uint8_t   payload = 42;
        pub.send(&payload, sizeof(payload));
    });

    auto sample = sub.receive(seconds{2});
    publisher.join();

    ASSERT_TRUE(sample.has_value());
    EXPECT_EQ(ring::WaiterNone, ring->has_waiter.load());
}

TEST_F(WaitFdTest, WaitFdOpensOneDescriptorAndKeepsIt)
{
    auto region = SharedRegion::create(SHM_NAME, channel::PubSub, bare_cfg());
    Subscriber sub(region);

    int fd = sub.wait_fd(backend);
    ASSERT_GE(fd, 0);
    EXPECT_EQ(fd, sub.wait_fd(backend));
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

// A generic waiter reaches the descriptor through ADL without naming kickmsg, which is
// what lets one wait set hold kickmsg sources and foreign ones together.
namespace foreign
{
    /// A type from another library: no kickmsg base, no kickmsg header, and its descriptor
    /// stays private. Opting in is one hidden friend.
    class Pipe
    {
    public:
        Pipe()  { (void) ::pipe(fds_); }
        ~Pipe() { ::close(fds_[0]); ::close(fds_[1]); }

        void poke() { uint8_t b = 1; (void) ::write(fds_[1], &b, sizeof(b)); }

    private:
        friend int wait_descriptor(Pipe const& p) { return p.fds_[0]; }
        int fds_[2]{-1, -1};
    };

    struct NotWaitable {};
}

static_assert(kickmsg::Waitable<kickmsg::Subscriber>);
static_assert(kickmsg::Waitable<kickmsg::Waker>);
static_assert(kickmsg::Waitable<foreign::Pipe>);
static_assert(not kickmsg::Waitable<foreign::NotWaitable>);

TEST_F(WaitFdTest, AGenericWaiterFindsTheDescriptorThroughAdl)
{
    auto region = SharedRegion::create(SHM_NAME, channel::PubSub, bare_cfg());
    Subscriber sub(region);

    // Unqualified, as WaitSet::add calls it.
    EXPECT_EQ(-1, wait_descriptor(sub));

    int const fd = sub.wait_fd(backend);
    ASSERT_GE(fd, 0);
    EXPECT_EQ(fd, wait_descriptor(sub));

    Waker shared(backend);
    EXPECT_EQ(shared.fd(), wait_descriptor(shared));
}

// The point of the concept: one set holds a kickmsg source and a foreign one, neither
// library knowing about the other, and no descriptor named at the call site.
TEST_F(WaitFdTest, AWaitSetMixesKickmsgAndForeignSources)
{
    auto region = SharedRegion::create(SHM_NAME, channel::PubSub, bare_cfg());
    Subscriber sub(region);
    ASSERT_GE(sub.wait_fd(backend), 0);

    foreign::Pipe pipe;

    WaitSet set;
    set.add(sub);
    set.add(pipe);
    ASSERT_EQ(2u, set.size());

    // Quiet: neither source has anything.
    EXPECT_FALSE(set.wait(milliseconds{20}));

    // The foreign source alone ends the wait.
    pipe.poke();
    EXPECT_TRUE(set.wait(seconds{2}));

    // And so does the kickmsg one, through its publisher.
    foreign::Pipe drained;   // fresh set, so the poked pipe does not mask the result
    WaitSet only_sub;
    only_sub.add(sub);
    only_sub.add(drained);
    ASSERT_EQ(Subscriber::Wait::Parked, sub.arm_wait());
    Publisher pub(region, &backend);
    uint8_t   payload = 1;
    ASSERT_GT(pub.send(&payload, sizeof(payload)), 0);
    EXPECT_TRUE(only_sub.wait(seconds{2}));
    sub.disarm_wait();
}

// A source with nothing to offer, and a duplicate, must not join: polling one descriptor
// twice would let one reader consume another's wake.
TEST_F(WaitFdTest, AWaitSetIgnoresUnwaitableAndDuplicateSources)
{
    auto region = SharedRegion::create(SHM_NAME, channel::PubSub, bare_cfg());
    Subscriber unopened(region);   // never asked for a descriptor

    WaitSet set;
    set.add(unopened);
    EXPECT_TRUE(set.empty());
    EXPECT_FALSE(set.wait(seconds{2}));   // an empty set never blocks

    foreign::Pipe pipe;
    set.add(pipe);
    set.add(pipe);
    EXPECT_EQ(1u, set.size());
}

TEST_F(WaitFdTest, ArmMarksTheRingAsCarrierParked)
{
    auto region = SharedRegion::create(SHM_NAME, channel::PubSub, bare_cfg());
    Subscriber sub(region);
    ASSERT_GE(sub.wait_fd(backend), 0);

    auto* ring = sub_ring_at(region.base(), region.header(), sub.ring_index());
    EXPECT_EQ(ring::WaiterNone, ring->has_waiter.load());
    ASSERT_EQ(Subscriber::Wait::Parked, sub.arm_wait());
    EXPECT_EQ(ring::WaiterCarrier, ring->has_waiter.load());
    sub.disarm_wait();
    EXPECT_EQ(ring::WaiterNone, ring->has_waiter.load());
}

TEST_F(WaitFdTest, DisarmIsIdempotent)
{
    auto region = SharedRegion::create(SHM_NAME, channel::PubSub, bare_cfg());
    Subscriber sub(region);
    ASSERT_GE(sub.wait_fd(backend), 0);

    auto* ring = sub_ring_at(region.base(), region.header(), sub.ring_index());
    ASSERT_EQ(Subscriber::Wait::Parked, sub.arm_wait());
    sub.disarm_wait();
    sub.disarm_wait();
    sub.disarm_wait();
    EXPECT_EQ(ring::WaiterNone, ring->has_waiter.load());
}

TEST_F(WaitFdTest, ArmOnEmptyRingParksAndDoesNotSignal)
{
    auto region = SharedRegion::create(SHM_NAME, channel::PubSub, bare_cfg());
    Subscriber sub(region);
    ASSERT_GE(sub.wait_fd(backend), 0);

    EXPECT_EQ(Subscriber::Wait::Parked, sub.arm_wait());
    EXPECT_FALSE(readable(sub.wait_fd(backend), milliseconds{20}));
    sub.disarm_wait();
}

TEST_F(WaitFdTest, ArmReportsReadyWhenASampleIsAlreadyQueued)
{
    auto region = SharedRegion::create(SHM_NAME, channel::PubSub, bare_cfg());
    Subscriber sub(region);
    ASSERT_GE(sub.wait_fd(backend), 0);

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
    ASSERT_GE(sub.wait_fd(backend), 0);

    ASSERT_EQ(Subscriber::Wait::Parked, sub.arm_wait());

    std::thread publisher([&]()
    {
        kickmsg::sleep(milliseconds{20});
        Publisher pub(region, &backend);
        uint8_t   payload = 99;
        pub.send(&payload, sizeof(payload));
    });

    EXPECT_TRUE(readable(sub.wait_fd(backend), seconds{2}));
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
    int const  fd = sub.wait_fd(backend);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(Subscriber::Wait::Parked, sub.arm_wait());

    // No backend is the default: the sample lands, the waiter falls back to its
    // deadline.
    Publisher pub(region);
    uint8_t   payload = 1;
    ASSERT_GT(pub.send(&payload, sizeof(payload)), 0);

    EXPECT_FALSE(readable(fd, milliseconds{50}));
    sub.disarm_wait();
    EXPECT_TRUE(sub.try_receive().has_value());
}

TEST_F(WaitFdTest, DisarmLeavesTheDescriptorLevelClean)
{
    auto region = SharedRegion::create(SHM_NAME, channel::PubSub, bare_cfg());
    Subscriber sub(region);
    ASSERT_GE(sub.wait_fd(backend), 0);

    ASSERT_EQ(Subscriber::Wait::Parked, sub.arm_wait());
    Publisher pub(region, &backend);
    uint8_t   payload = 1;
    ASSERT_GT(pub.send(&payload, sizeof(payload)), 0);
    ASSERT_TRUE(readable(sub.wait_fd(backend), seconds{2}));

    sub.disarm_wait();
    ASSERT_TRUE(sub.try_receive().has_value());
    EXPECT_FALSE(readable(sub.wait_fd(backend), milliseconds{20}));
}

TEST_F(WaitFdTest, OneWakerServesSeveralSubscribers)
{
    auto region = SharedRegion::create(SHM_NAME, channel::PubSub, bare_cfg());

    Waker      waker(backend);
    Subscriber first(region);
    Subscriber second(region);

    int fd = first.attach(waker);
    ASSERT_GE(fd, 0);
    EXPECT_EQ(fd, second.attach(waker));

    ASSERT_EQ(Subscriber::Wait::Parked, first.arm_wait());
    ASSERT_EQ(Subscriber::Wait::Parked, second.arm_wait());

    Publisher pub(region, &backend);
    uint8_t   payload = 3;
    ASSERT_GT(pub.send(&payload, sizeof(payload)), 0);

    EXPECT_TRUE(readable(fd, seconds{2}));

    // A shared waker is drained by its owner, not by disarm_wait.
    first.disarm_wait();
    second.disarm_wait();
    waker.drain();

    EXPECT_TRUE(first.try_receive().has_value());
    EXPECT_TRUE(second.try_receive().has_value());
    EXPECT_FALSE(readable(fd, milliseconds{20}));
}
// A shared port would not do: a socket bound to INADDR_ANY receives every datagram on
// its port, including groups it never joined.
TEST_F(WaitFdTest, AWakeOnOneChannelDoesNotReachAnother)
{
    auto first  = SharedRegion::create(SHM_NAME,  channel::PubSub, bare_cfg());
    auto second = SharedRegion::create(SHM_OTHER, channel::PubSub, bare_cfg());

    Subscriber sub_first(first);
    Subscriber sub_second(second);
    ASSERT_GE(sub_first.wait_fd(backend), 0);
    int const quiet_fd = sub_second.wait_fd(other_backend);
    ASSERT_GE(quiet_fd, 0);

    ASSERT_EQ(Subscriber::Wait::Parked, sub_first.arm_wait());
    ASSERT_EQ(Subscriber::Wait::Parked, sub_second.arm_wait());

    Publisher pub(first, &backend);
    uint8_t   payload = 5;
    ASSERT_GT(pub.send(&payload, sizeof(payload)), 0);

    EXPECT_TRUE(readable(sub_first.wait_fd(backend), seconds{2}));
    EXPECT_FALSE(readable(quiet_fd, milliseconds{50}));

    sub_first.disarm_wait();
    sub_second.disarm_wait();
}

TEST_F(WaitFdTest, ReclaimingARingClearsAStaleWaiterMode)
{
    auto region = SharedRegion::create(SHM_NAME, channel::PubSub, bare_cfg());

    SubRingHeader* ring = nullptr;
    {
        Subscriber sub(region);
        ASSERT_GE(sub.wait_fd(backend), 0);
        ring = sub_ring_at(region.base(), region.header(), sub.ring_index());
        ASSERT_EQ(Subscriber::Wait::Parked, sub.arm_wait());
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

    int const private_fd = sub.wait_fd(backend);
    ASSERT_GE(private_fd, 0);

    Waker shared(backend);
    ASSERT_GE(sub.attach(shared), 0);
    EXPECT_EQ(shared.fd(), sub.wait_fd(backend));
    EXPECT_NE(private_fd, shared.fd());

    ASSERT_EQ(Subscriber::Wait::Parked, sub.arm_wait());
    Publisher pub(region, &backend);
    uint8_t   payload = 6;
    ASSERT_GT(pub.send(&payload, sizeof(payload)), 0);

    EXPECT_TRUE(readable(shared.fd(), seconds{2}));
    sub.disarm_wait();
    shared.drain();
    EXPECT_TRUE(sub.try_receive().has_value());
}

// A pipe is POSIX; the backend interface it demonstrates is not. Everything above runs
// everywhere, including the Winsock multicast carrier.
#ifndef _WIN32

#include <poll.h>
#include <unistd.h>

namespace
{
    /// A backend a caller owning both ends supplies to each, under the id its channel
    /// declares. Both sides are this process, so a pipe stands in for whatever transport
    /// the caller would really use.
    class PipeBackend final : public WakeBackend
    {
    public:
            int open() override
        {
            if (::pipe(fds_) != 0)
            {
                return -1;
            }
            return fds_[0];
        }

        void close(int) override
        {
            ::close(fds_[0]);
            ::close(fds_[1]);
            fds_[0] = -1;
            fds_[1] = -1;
        }

        void drain(int fd) override
        {
            uint8_t buffer[64];
            pollfd  p{fd, POLLIN, 0};
            while (::poll(&p, 1, 0) == 1)
            {
                if (::read(fd, buffer, sizeof(buffer)) <= 0)
                {
                    return;
                }
            }
        }

        void signal() override
        {
            uint8_t byte = 1;
            ++signals;
            (void) ::write(fds_[1], &byte, sizeof(byte));
        }

        int signals{0};

    private:
        int fds_[2]{-1, -1};
    };
}

TEST_F(WaitFdTest, ACallerSuppliedBackendIsUsedForTheWake)
{
    auto region = SharedRegion::create(SHM_NAME, channel::PubSub, bare_cfg());

    PipeBackend pipe;

    Subscriber sub(region);
    int        fd = sub.wait_fd(pipe);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(Subscriber::Wait::Parked, sub.arm_wait());

    Publisher pub(region, &pipe);
    uint8_t   payload = 11;
    ASSERT_GT(pub.send(&payload, sizeof(payload)), 0);

    EXPECT_EQ(1, pipe.signals);
    EXPECT_TRUE(readable(fd, seconds{2}));
    sub.disarm_wait();
    EXPECT_TRUE(sub.try_receive().has_value());
}

TEST_F(WaitFdTest, WaitFdRefusesADifferentBackendAfterTheFirst)
{
    auto region = SharedRegion::create(SHM_NAME, channel::PubSub, bare_cfg());

    PipeBackend pipe;
    Subscriber  sub(region);
    ASSERT_GE(sub.wait_fd(pipe), 0);

    // Ignoring the argument would hide two ends wired to different backends.
    EXPECT_EQ(-1, sub.wait_fd(backend));
}

#endif
