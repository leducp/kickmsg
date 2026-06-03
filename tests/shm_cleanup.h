#ifndef KICKMSG_TESTS_SHM_CLEANUP_H
#define KICKMSG_TESTS_SHM_CLEANUP_H

// Best-effort shm cleanup for the test binaries: on SIGINT/SIGTERM, unlink the
// registered segments so an interrupted run leaves no stale /dev/shm entry for
// the next run to trip over.  Names must be string literals (static storage);
// the handler only calls shm_unlink + _exit, both async-signal-safe.
// SIGKILL (-9) cannot be caught -- those leftovers are handled by each
// scenario's unlink-before-create instead.

#ifndef _WIN32
#include <csignal>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

namespace kickmsg_test
{
    inline constexpr int MAX_CLEANUP = 32;
    // volatile: the elements are read from a signal handler.
    inline char const* volatile g_cleanup_names[MAX_CLEANUP] = {};
    inline volatile sig_atomic_t g_cleanup_count = 0;

    inline void shm_cleanup_handler(int sig)
    {
        for (sig_atomic_t i = 0; i < g_cleanup_count; ++i)
        {
            ::shm_unlink(g_cleanup_names[i]);
        }
        ::_exit(128 + sig);
    }

    inline void register_cleanup_shm(char const* name)
    {
        if (g_cleanup_count < MAX_CLEANUP)
        {
            g_cleanup_names[g_cleanup_count] = name;
            g_cleanup_count = g_cleanup_count + 1;
        }
    }

    inline void install_signal_cleanup()
    {
        struct sigaction sa;
        std::memset(&sa, 0, sizeof(sa));
        sa.sa_handler = shm_cleanup_handler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;
        ::sigaction(SIGINT, &sa, nullptr);
        ::sigaction(SIGTERM, &sa, nullptr);
    }
}
#else
namespace kickmsg_test
{
    inline void register_cleanup_shm(char const*) {}
    inline void install_signal_cleanup() {}
}
#endif

#endif
