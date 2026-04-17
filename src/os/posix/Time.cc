// Parts of the Time API that are identical on Linux and macOS.
// Per-platform sleep() lives in src/os/{linux,darwin}/Time.cc.
#include "kickmsg/os/Time.h"

#include <cerrno>
#include <ctime>
#include <sched.h>
#include <stdexcept>
#include <system_error>

namespace kickmsg
{
    void yield()
    {
        ::sched_yield();
    }

    nanoseconds since_epoch()
    {
        timespec ts;
        if (::clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        {
            throw std::system_error(errno, std::system_category(), "clock_gettime()");
        }
        return seconds{ts.tv_sec} + nanoseconds{ts.tv_nsec};
    }

    nanoseconds elapsed_time(nanoseconds start)
    {
        return since_epoch() - start;
    }
}
