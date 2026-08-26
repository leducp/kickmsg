#include "kickmsg/os/Process.h"

#include <atomic>

#ifndef _WIN32
#include <pthread.h>
#endif

namespace kickmsg
{
    namespace
    {
        std::atomic<uint64_t> g_self_pid{0};
        std::atomic<uint64_t> g_self_starttime{0};

#ifndef _WIN32
        void forget_self_identity() noexcept
        {
            g_self_pid.store(0, std::memory_order_relaxed);
        }

        struct AtForkHook
        {
            AtForkHook() { ::pthread_atfork(nullptr, nullptr, &forget_self_identity); }
        };
#endif
    }

    uint64_t process_starttime(uint64_t pid) noexcept
    {
        return process_probe(pid).starttime;
    }

    SelfIdentity self_identity() noexcept
    {
#ifndef _WIN32
        static AtForkHook const hook;
        (void)hook;
#endif
        uint64_t pid = g_self_pid.load(std::memory_order_acquire);
        if (pid == 0)
        {
            pid = current_pid();
            g_self_starttime.store(process_starttime(pid), std::memory_order_relaxed);
            g_self_pid.store(pid, std::memory_order_release);
        }
        return {pid, g_self_starttime.load(std::memory_order_relaxed)};
    }

    bool owner_is_dead(uint64_t pid, uint64_t recorded_starttime) noexcept
    {
        if (pid == 0)
        {
            return false;
        }
        if (not process_exists(pid))
        {
            return true;
        }
        ProcessProbe live = process_probe(pid);
        // A zombie still answers kill(pid, 0) and still reports the start time
        // it was recorded with, so nothing above this catches it.
        if (live.exited)
        {
            return true;
        }
        // A zero on either side means the platform could not answer (or the
        // process vanished mid-probe): degrade to the pid-alone verdict rather
        // than declaring a live process dead.
        if (recorded_starttime == 0 or live.starttime == 0)
        {
            return false;
        }
        return recorded_starttime != live.starttime;
    }
}
