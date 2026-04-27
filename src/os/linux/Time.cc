// Linux-specific sleep().  Other Time entry points live in
// src/os/posix/Time.cc.
#include "kickmsg/os/Time.h"

#include <cerrno>
#include <ctime>
#include <stdexcept>
#include <system_error>

namespace kickmsg
{
    void sleep(nanoseconds ns)
    {
        auto secs = duration_cast<seconds>(ns);
        nanoseconds nsecs = (ns - secs);
        timespec remaining{secs.count(), nsecs.count()};

        while (true)
        {
            timespec required = remaining;
            int result = ::clock_nanosleep(CLOCK_MONOTONIC, 0, &required, &remaining);
            if (result == 0)
            {
                return;
            }
            if (result == EINTR)
            {
                continue;
            }
            throw std::system_error(result, std::system_category(), "clock_nanosleep()");
        }
    }
}
