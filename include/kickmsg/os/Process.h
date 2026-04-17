#ifndef KICKMSG_OS_PROCESS_H
#define KICKMSG_OS_PROCESS_H

#include <cstdint>

namespace kickmsg
{
    /// PID of the current process.
    uint64_t current_pid() noexcept;

    /// Return true if a process with \p pid currently exists on this host.
    /// Inherently racy: the process may exit between the probe and any
    /// action taken on the result.
    bool process_exists(uint64_t pid) noexcept;
}

#endif
