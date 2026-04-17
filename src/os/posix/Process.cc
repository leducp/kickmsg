#include "kickmsg/os/Process.h"

#include <cerrno>
#include <csignal>
#include <sys/types.h>
#include <unistd.h>

namespace kickmsg
{
    uint64_t current_pid() noexcept
    {
        return static_cast<uint64_t>(::getpid());
    }

    bool process_exists(uint64_t pid) noexcept
    {
        if (pid == 0)
        {
            return false;
        }
        if (::kill(static_cast<pid_t>(pid), 0) == 0)
        {
            return true;
        }
        // EPERM means the process exists but we can't signal it.
        return errno == EPERM;
    }
}
