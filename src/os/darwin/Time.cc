// macOS-specific sleep().  clock_nanosleep is unavailable; nanosleep is the
// POSIX fallback.  Other Time entry points live in src/os/posix/Time.cc.
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
            int result = ::nanosleep(&required, &remaining);
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
}
