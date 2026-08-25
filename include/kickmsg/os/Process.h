#ifndef KICKMSG_OS_PROCESS_H
#define KICKMSG_OS_PROCESS_H

#include <cstdint>

namespace kickmsg
{
    /// PID of the current process.
    uint64_t current_pid() noexcept;

    /// One observation of a process: its start time, and whether it has
    /// already terminated without being reaped.
    ///
    /// A zombie answers kill(pid, 0) and still reports its start time, so
    /// `exited` is the only thing separating it from a running process.
    /// `exited` stays false when the platform could not read the process.
    struct ProcessProbe
    {
        uint64_t starttime{0};
        bool     exited{false};
    };

    /// Probe \p pid.  Returns a zeroed probe when \p pid is 0 or unreadable.
    ///
    /// Linux: /proc/<pid>/stat fields 3 (state) and 22 (starttime).
    /// Darwin: sysctl kinfo_proc (p_stat, p_starttime).
    /// Windows: GetProcessTimes creation time, plus a zero-timeout wait.
    ProcessProbe process_probe(uint64_t pid) noexcept;

    /// Return true if a process with \p pid currently exists on this host.
    /// Inherently racy: the process may exit between the probe and any
    /// action taken on the result.  A zombie counts as existing; use
    /// owner_is_dead() to reclaim from one.
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

    /// PID and start time of the current process, both constant for its
    /// lifetime.
    struct SelfIdentity
    {
        uint64_t pid{0};
        uint64_t starttime{0};
    };

    /// Cached self_identity: process_starttime() is a /proc read on Linux and
    /// a sysctl on Darwin, and the blackboard lock stamps this pair on every
    /// acquisition.  The cache is dropped in the child of a fork(); a child
    /// made by clone() or vfork() keeps the parent's identity.
    SelfIdentity self_identity() noexcept;

    /// Return true when \p pid is provably gone: no such process exists, one
    /// exists but has exited without being reaped, or one exists whose start
    /// time differs from \p recorded_starttime (PID reuse after wraparound).
    /// A zero on either start time degrades the verdict to pid-alone, and
    /// \p pid == 0 (an owner field not yet written) is never reported dead.
    ///
    /// This is the guard every reclaim path must use before taking a slot
    /// from its owner: Registry::sweep_stale, SharedRegion's ring reclaim,
    /// and Blackboard key takeover.
    bool owner_is_dead(uint64_t pid, uint64_t recorded_starttime) noexcept;
}

#endif
