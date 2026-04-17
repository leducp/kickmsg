#ifndef KICKMSG_OS_TIME_H
#define KICKMSG_OS_TIME_H

#include <chrono>

namespace kickmsg
{
    using namespace std::chrono;

    void sleep(nanoseconds ns);

    /// Yield the current thread, giving the scheduler a chance to run
    /// another runnable thread on this core.  Cooperative — no guarantee
    /// that anyone else actually runs.  Used on short-bounded spin loops
    /// (quiescence waits, commit-pending waits) to avoid burning a core
    /// while still reacting as fast as the scheduler allows.
    ///
    /// We carry our own wrapper (rather than `std::this_thread::yield`)
    /// so the library stays consistent with the `sleep` / `since_epoch`
    /// pattern of routing every scheduling primitive through the OS
    /// abstraction — cheap to retarget to platforms whose C++ runtime
    /// doesn't ship `<thread>` (RTOSes, some embedded toolchains).
    void yield();

    nanoseconds since_epoch();

    nanoseconds elapsed_time(nanoseconds start);

    constexpr timespec to_timespec(nanoseconds time)
    {
        auto secs = duration_cast<seconds>(time);
        nanoseconds nsecs = (time - secs);
        return timespec{static_cast<time_t>(secs.count()), static_cast<long>(nsecs.count())};
    }
}

#endif
