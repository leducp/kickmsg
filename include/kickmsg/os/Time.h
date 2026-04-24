#ifndef KICKMSG_OS_TIME_H
#define KICKMSG_OS_TIME_H

#include <chrono>

namespace kickmsg
{
    using namespace std::chrono;

    void sleep(nanoseconds ns);

    /// Release the current timeslice back to the scheduler.
    void yield();

    /// Monotonic time since an unspecified origin (typically boot).
    /// Use for duration measurements — never leaks forward across
    /// clock adjustments.  NOT suitable for display timestamps: see
    /// since_epoch().
    nanoseconds monotonic_ns();

    /// Wall-clock time since 1970-01-01 UTC (CLOCK_REALTIME).  Use
    /// for human-facing timestamps.  Subject to jumps on NTP
    /// adjustment — do NOT use for timeouts or duration math.
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
