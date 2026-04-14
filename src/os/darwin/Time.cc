// macOS uses POSIX clock_gettime (available since macOS 10.12).
// clock_nanosleep is NOT available on macOS — use nanosleep instead.
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
            int result = nanosleep(&required, &remaining);
            if (result == 0)
            {
                return;
            }
            if (errno == EINTR)
            {
                continue;
            }
            throw std::system_error(errno, std::system_category(), "nanosleep()");
        }
    }

    nanoseconds since_epoch()
    {
        timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return seconds{ts.tv_sec} + nanoseconds{ts.tv_nsec};
    }

    nanoseconds elapsed_time(nanoseconds start)
    {
        return since_epoch() - start;
    }
}
