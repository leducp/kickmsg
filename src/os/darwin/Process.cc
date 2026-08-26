#include "kickmsg/os/Process.h"

#include <cstddef>
#include <sys/proc.h>
#include <sys/sysctl.h>
#include <sys/types.h>

namespace kickmsg
{
    ProcessProbe process_probe(uint64_t pid) noexcept
    {
        ProcessProbe out;
        if (pid == 0)
        {
            return out;
        }
        // sysctl({CTL_KERN, KERN_PROC, KERN_PROC_PID, pid}) → kinfo_proc.
        // kp_proc.p_starttime is a struct timeval set at fork time; pack
        // into microseconds-since-epoch for a single uint64_t.
        int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID,
                      static_cast<int>(pid)};
        struct kinfo_proc kp;
        std::size_t len = sizeof(kp);
        if (::sysctl(mib, 4, &kp, &len, nullptr, 0) != 0 or len == 0)
        {
            return out;
        }
        out.exited = kp.kp_proc.p_stat == SZOMB;
        auto const& tv = kp.kp_proc.p_starttime;
        out.starttime = static_cast<uint64_t>(tv.tv_sec) * 1'000'000ull
                      + static_cast<uint64_t>(tv.tv_usec);
        return out;
    }
}
