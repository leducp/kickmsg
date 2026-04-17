#include "kickmsg/os/Time.h"

#include <cerrno>
#include <ctime>
#include <sched.h>
#include <stdexcept>
#include <system_error>
#include <string>

namespace kickmsg
{
    void yield()
    {
        // sched_yield never fails in practice on Linux; ignore the return.
        ::sched_yield();
    }

    void sleep(nanoseconds ns)
    {
        auto secs = duration_cast<seconds>(ns);
        nanoseconds nsecs = (ns - secs);
        timespec remaining{secs.count(), nsecs.count()};

        while (true)
        {
            timespec required = remaining;
            int result = clock_nanosleep(CLOCK_MONOTONIC, 0, &required, &remaining);
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

    nanoseconds since_epoch()
    {
        timespec ts;
        if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
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
