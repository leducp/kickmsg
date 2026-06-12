#include "kickmsg/os/Time.h"

#include <cerrno>
#include <ctime>
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
