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

    /// Opaque start time of \p pid, or 0 if unavailable.  The value is
    /// only meaningful for equality: two reads of the same live process
    /// return the same value, and a PID-reuse after wraparound almost
    /// always yields a different one.  Used by sweep_stale as a PID-
    /// reuse guard.
    ///
    /// Linux: clock ticks since boot (/proc/<pid>/stat field 22).
    /// Darwin: microseconds since epoch (sysctl kinfo_proc.p_starttime).
    /// Windows: 100-ns intervals since 1601 (GetProcessTimes creation).
    uint64_t process_starttime(uint64_t pid) noexcept;
}

#endif
