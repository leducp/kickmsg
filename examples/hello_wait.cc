/// @file hello_wait.cc
/// @brief Block on a kickmsg subscriber from a loop that also watches other sources.
///
/// A subscriber normally blocks on a futex, which composes with nothing else. Give both
/// ends a wake backend instead and the subscriber hands out a descriptor, so one loop can
/// serve it alongside a socket, a timer, or anything else the caller already polls.
///
/// Single-process for simplicity; in production the publisher and the subscriber are two
/// processes, each building its own backend from the same name.

#include <cstring>
#include <iostream>
#include <thread>

#include <kickmsg/Node.h>
#include <kickmsg/WaitSet.h>
#include <kickmsg/Waker.h>

using namespace kickmsg;

struct Reading
{
    uint32_t sensor_id;
    float    celsius;
};

int main()
{
    kickmsg::SharedMemory::unlink("/demo_wait_temperature");

    // Both ends are given the same backend. Nothing about it is negotiated through shared
    // memory: each side derives the same address from the name.
    UdpMulticastBackend backend("demo/temperature");

    Node sensor("sensor", "demo_wait");
    auto pub = sensor.advertise("temperature", {}, &backend);

    Node display("display", "demo_wait");
    auto sub = display.subscribe("temperature");

    Waker waker(backend);
    if (not sub.attach(waker))
    {
        std::cerr << "no wake carrier on this host\n";
        return 1;
    }

    // The subscriber opts in through wait_descriptor(); add_native() takes a descriptor
    // this example does not have, from a socket or a timer the caller already owns.
    WaitSet sources;
    sources.add(sub);

    std::thread producer([&]
    {
        Reading readings[] = {{1, 22.5f}, {2, 19.8f}, {1, 23.1f}};
        for (auto const& r : readings)
        {
            kickmsg::sleep(50ms);
            (void) pub.send(&r, sizeof(r));
        }
    });

    for (int received = 0; received < 3;)
    {
        // Arming publishes the intent to block. Do it before the last check for a sample,
        // or a publisher committing in between sends its wake to nobody.
        nanoseconds budget = 1s;
        switch (sub.arm_wait())
        {
            case Subscriber::Wait::Ready: { budget = 0ns;   break; }
            // Claimed but uncommitted: the commit fires no wake, so come back on our own.
            case Subscriber::Wait::Poll:  { budget = 100us; break; }
            case Subscriber::Wait::Armed: { break; }
        }
        if (budget > 0ns)
        {
            (void) sources.wait(budget);
        }
        sub.disarm_wait();
        waker.drain();

        while (auto sample = sub.try_receive())
        {
            Reading r;
            std::memcpy(&r, sample->data(), sizeof(r));
            std::cout << "Sensor " << r.sensor_id << ": " << r.celsius << " C\n";
            ++received;
        }
    }

    producer.join();
    kickmsg::SharedMemory::unlink("/demo_wait_temperature");
    std::cout << "Done.\n";
    return 0;
}
