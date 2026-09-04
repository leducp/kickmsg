#include "common.h"

#include "kickmsg/WaitSet.h"
#include "kickmsg/Waker.h"

// Soaks for the pollable wake path: duration- and throughput-shaped, so not unit tests.

namespace
{
    bool readable(int fd, nanoseconds timeout)
    {
        WaitSet set;
        set.add_native(fd);
        return set.wait(timeout);
    }

    /// Lockstep: message i goes out only once i-1 has been consumed. Nothing can be
    /// overwritten, so a lossless oracle means something, and the consumer reaches an
    /// empty ring every round -- the only state that makes it wait on the descriptor.
    ///
    /// Credit, not a sleep: sub-millisecond sleeps round to zero on Windows, so a paced
    /// publisher becomes a free-running one and the run measures ring overrun instead of
    /// wakes.
    constexpr uint32_t BACKLOG = 0;

    channel::Config wake_cfg(char const*)
    {
        channel::Config cfg;
        cfg.max_subscribers   = 4;
        // Deep enough that a consumer hiccup costs latency, not samples: a conservation
        // oracle over a shallow ring only measures overrun.
        cfg.sub_ring_capacity = 1024;
        cfg.pool_size         = 2048;
        cfg.max_payload_size  = 64;
        return cfg;
    }
}

// Races arm_wait() against a publisher: a waiting subscriber must never hold a committed
// sample no wake will follow.
//
// This does NOT reliably reproduce the ordering it guards -- the window is two
// instructions, and a publish from another core cannot be steered into it. Catches a
// gross regression, nothing finer.
bool run_wake_parking()
{
    std::printf("--- Wake backend: 40k arm/wait/disarm rounds against a live publisher ---\n");

    char const* shm_name = "/kickmsg_wake_parking";
    kickmsg::SharedMemory::unlink(shm_name);
    auto region = kickmsg::SharedRegion::create(
        shm_name, kickmsg::channel::PubSub, wake_cfg(shm_name), "wake_parking");

    UdpMulticastBackend backend{shm_name};
    Subscriber          sub(region);
    int const           fd = sub.wait_fd(backend);
    if (fd < 0)
    {
        std::printf("  carrier unavailable on this host -- skipped\n");
        kickmsg::SharedMemory::unlink(shm_name);
        return true;
    }

    int const ROUNDS = 40000 / TSAN_SCALE;

    std::atomic<int>  spin{0};
    std::atomic<bool> arm{false};
    std::atomic<bool> published{false};
    std::atomic<bool> stop{false};

    std::thread publisher([&]()
    {
        Publisher pub(region, &backend);
        while (not stop.load(std::memory_order_acquire))
        {
            if (not arm.load(std::memory_order_acquire))
            {
                continue;
            }
            for (int i = spin.load(std::memory_order_relaxed); i > 0; --i)
            {
                std::atomic_signal_fence(std::memory_order_seq_cst);
            }
            uint32_t value = 1;
            pub.send(&value, sizeof(value));
            published.store(true, std::memory_order_release);
            arm.store(false, std::memory_order_release);
        }
    });

    int waited_without_wake = 0;
    for (int round = 0; round < ROUNDS; ++round)
    {
        published.store(false, std::memory_order_relaxed);
        spin.store(round % 96, std::memory_order_relaxed);
        arm.store(true, std::memory_order_release);

        Subscriber::Wait const state     = sub.arm_wait();
        bool                   signalled = false;
        if (state == Subscriber::Wait::Parked)
        {
            signalled = readable(fd, milliseconds{2});
        }

        while (not published.load(std::memory_order_acquire))
        {
            kickmsg::yield();
        }
        if (state == Subscriber::Wait::Parked and not signalled)
        {
            signalled = readable(fd, milliseconds{50});
        }
        sub.disarm_wait();

        // The publisher is done for this round: a sample now, with no wake delivered,
        // is one the ring waited straight past.
        if (state == Subscriber::Wait::Parked and not signalled
            and sub.peek() == Subscriber::Wait::Ready)
        {
            ++waited_without_wake;
        }
        while (sub.try_receive())
        {
        }
    }

    stop.store(true, std::memory_order_release);
    publisher.join();
    kickmsg::SharedMemory::unlink(shm_name);

    if (waited_without_wake != 0)
    {
        std::printf("  FAIL: %d of %d rounds waited on a committed sample "
                    "the publisher never signalled\n", waited_without_wake, ROUNDS);
        return false;
    }
    std::printf("  %d rounds, every waiting round woken\n", ROUNDS);
    return true;
}

// Steady state through the arm/poll/disarm loop: every message, in order, none lost.
bool run_wake_arm_loop()
{
    std::printf("--- Wake backend: 20k messages through an arm/poll/disarm loop ---\n");

    char const* shm_name = "/kickmsg_wake_arm_loop";
    kickmsg::SharedMemory::unlink(shm_name);
    auto region = kickmsg::SharedRegion::create(
        shm_name, kickmsg::channel::PubSub, wake_cfg(shm_name), "wake_arm_loop");

    UdpMulticastBackend backend{shm_name};
    Subscriber          sub(region);
    int const           fd = sub.wait_fd(backend);
    if (fd < 0)
    {
        std::printf("  carrier unavailable on this host -- skipped\n");
        kickmsg::SharedMemory::unlink(shm_name);
        return true;
    }

    uint32_t const COUNT = 20000 / TSAN_SCALE;

    std::atomic<uint32_t> consumed{0};
    std::atomic<bool>     abort_run{false};

    std::thread publisher([&]()
    {
        Publisher pub(region, &backend);
        for (uint32_t i = 0; i < COUNT; ++i)
        {
            while (i - consumed.load(std::memory_order_acquire) > BACKLOG)
            {
                if (abort_run.load(std::memory_order_acquire)) { return; }
                kickmsg::yield();
            }
            while (pub.send(&i, sizeof(i)) < 0)
            {
                if (abort_run.load(std::memory_order_acquire)) { return; }
                kickmsg::yield();
            }
        }
    });

    uint32_t    received = 0;
    uint32_t    expected = 0;
    uint32_t    waits    = 0;
    bool        ordered  = true;
    nanoseconds start    = kickmsg::monotonic_ns();
    while (received < COUNT and kickmsg::elapsed_time(start) < seconds{60})
    {
        nanoseconds budget = milliseconds{5};
        switch (sub.arm_wait())
        {
            case Subscriber::Wait::Ready:  budget = nanoseconds{0};            break;
            case Subscriber::Wait::Poll:   budget = Subscriber::poll_budget(); break;
            case Subscriber::Wait::Parked: ++waits;                            break;
        }
        if (budget > nanoseconds{0})
        {
            readable(fd, budget);
        }
        sub.disarm_wait();

        while (auto sample = sub.try_receive())
        {
            uint32_t value = 0;
            std::memcpy(&value, sample->data(), sizeof(value));
            if (value != expected)
            {
                ordered = false;
            }
            ++expected;
            ++received;
        }
        consumed.store(received, std::memory_order_release);
    }

    abort_run.store(true, std::memory_order_release);
    publisher.join();
    kickmsg::SharedMemory::unlink(shm_name);

    if (received != COUNT or sub.lost() != 0 or not ordered)
    {
        std::printf("  FAIL: received %u/%u, lost %llu, ordered=%d\n",
                    received, COUNT,
                    static_cast<unsigned long long>(sub.lost()), ordered);
        return false;
    }
    // Without this the run can pass while never once blocking on the descriptor, which
    // is the whole thing it is here to exercise.
    if (waits < COUNT / 10)
    {
        std::printf("  FAIL: only %u of %u rounds waited on the descriptor\n",
                    waits, received);
        return false;
    }
    std::printf("  %u messages, in order, none lost, %u rounds waited\n", received, waits);
    return true;
}

// wait_any over two channels, each with its own carrier: conservation on both.
bool run_wake_wait_any()
{
    std::printf("--- Wake backend: wait_any over 2 channels, 10k messages each ---\n");

    char const* shm_a = "/kickmsg_wake_any_a";
    char const* shm_b = "/kickmsg_wake_any_b";
    kickmsg::SharedMemory::unlink(shm_a);
    kickmsg::SharedMemory::unlink(shm_b);
    auto a = kickmsg::SharedRegion::create(
        shm_a, kickmsg::channel::PubSub, wake_cfg(shm_a), "wake_any_a");
    auto b = kickmsg::SharedRegion::create(
        shm_b, kickmsg::channel::PubSub, wake_cfg(shm_b), "wake_any_b");

    Subscriber  sub_a(a);
    Subscriber  sub_b(b);
    Subscriber* subs[] = {&sub_a, &sub_b};

    // One backend per channel: the port keeps their wakes apart.
    UdpMulticastBackend backend_a{shm_a};
    UdpMulticastBackend backend_b{shm_b};
    Waker               waker_a(backend_a);
    Waker               waker_b(backend_b);
    if (sub_a.attach(waker_a) < 0 or sub_b.attach(waker_b) < 0)
    {
        std::printf("  carrier unavailable on this host -- skipped\n");
        kickmsg::SharedMemory::unlink(shm_a);
        kickmsg::SharedMemory::unlink(shm_b);
        return true;
    }

    uint32_t const COUNT = 10000 / TSAN_SCALE;

    std::atomic<uint32_t> consumed_a{0};
    std::atomic<uint32_t> consumed_b{0};
    std::atomic<bool>     abort_run{false};

    auto publish = [&](SharedRegion& region, WakeBackend& backend,
                       std::atomic<uint32_t>& consumed)
    {
        Publisher pub(region, &backend);
        for (uint32_t i = 0; i < COUNT; ++i)
        {
            while (i - consumed.load(std::memory_order_acquire) > BACKLOG)
            {
                if (abort_run.load(std::memory_order_acquire)) { return; }
                kickmsg::yield();
            }
            while (pub.send(&i, sizeof(i)) < 0)
            {
                if (abort_run.load(std::memory_order_acquire)) { return; }
                kickmsg::yield();
            }
        }
    };

    std::thread first([&]() { publish(a, backend_a, consumed_a); });
    std::thread second([&]() { publish(b, backend_b, consumed_b); });

    uint32_t    got_a   = 0;
    uint32_t    got_b   = 0;
    uint32_t    waits   = 0;
    bool        ordered = true;
    nanoseconds start   = kickmsg::monotonic_ns();
    while ((got_a < COUNT or got_b < COUNT)
           and kickmsg::elapsed_time(start) < seconds{60})
    {
        // Nothing ready on entry means this call has to block, which is what the run is
        // here to exercise; counting it keeps the soak from passing as a busy poll.
        if (sub_a.peek() != Subscriber::Wait::Ready
            and sub_b.peek() != Subscriber::Wait::Ready)
        {
            ++waits;
        }
        wait_any(subs, 2, milliseconds{5});
        while (auto sample = sub_a.try_receive())
        {
            uint32_t value = 0;
            std::memcpy(&value, sample->data(), sizeof(value));
            if (value != got_a)
            {
                ordered = false;
            }
            ++got_a;
        }
        while (auto sample = sub_b.try_receive())
        {
            uint32_t value = 0;
            std::memcpy(&value, sample->data(), sizeof(value));
            if (value != got_b)
            {
                ordered = false;
            }
            ++got_b;
        }
        consumed_a.store(got_a, std::memory_order_release);
        consumed_b.store(got_b, std::memory_order_release);
    }

    abort_run.store(true, std::memory_order_release);
    first.join();
    second.join();
    kickmsg::SharedMemory::unlink(shm_a);
    kickmsg::SharedMemory::unlink(shm_b);

    if (got_a != COUNT or got_b != COUNT or not ordered
        or sub_a.lost() != 0 or sub_b.lost() != 0)
    {
        std::printf("  FAIL: a=%u/%u b=%u/%u ordered=%d lost=%llu/%llu\n",
                    got_a, COUNT, got_b, COUNT, ordered,
                    static_cast<unsigned long long>(sub_a.lost()),
                    static_cast<unsigned long long>(sub_b.lost()));
        return false;
    }
    if (waits < COUNT / 10)
    {
        std::printf("  FAIL: only %u rounds entered wait_any with nothing ready\n", waits);
        return false;
    }
    std::printf("  %u + %u messages, in order, none lost, %u rounds waited\n",
                got_a, got_b, waits);
    return true;
}
