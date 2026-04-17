#ifndef KICKMSG_OS_TIME_H
#define KICKMSG_OS_TIME_H

#include <chrono>

namespace kickmsg
{
    using namespace std::chrono;

    void sleep(nanoseconds ns);

    /// Release the current timeslice back to the scheduler.
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
