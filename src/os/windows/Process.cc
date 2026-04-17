#include "kickmsg/os/Process.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace kickmsg
{
    uint64_t current_pid() noexcept
    {
        return static_cast<uint64_t>(::GetCurrentProcessId());
    }

    uint64_t process_starttime(uint64_t pid) noexcept
    {
        if (pid == 0)
        {
            return 0;
        }
        HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,
                                 FALSE,
                                 static_cast<DWORD>(pid));
        if (h == nullptr)
        {
            return 0;
        }
        FILETIME creation{}, exit{}, kernel{}, user{};
        BOOL ok = ::GetProcessTimes(h, &creation, &exit, &kernel, &user);
        ::CloseHandle(h);
        if (not ok)
        {
            return 0;
        }
        // FILETIME is 100-ns intervals since 1601-01-01; packing the two
        // halves is enough for the equality comparison sweep_stale uses.
        return (static_cast<uint64_t>(creation.dwHighDateTime) << 32)
             | static_cast<uint64_t>(creation.dwLowDateTime);
    }

    bool process_exists(uint64_t pid) noexcept
    {
        if (pid == 0)
        {
            return false;
        }
        // PROCESS_QUERY_LIMITED_INFORMATION is granted even for processes
        // running under different integrity levels / sessions, which is
        // what we want for a cross-user discovery tool.
        HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,
                                 FALSE,
                                 static_cast<DWORD>(pid));
        if (h != nullptr)
        {
            ::CloseHandle(h);
            return true;
        }
        // ERROR_ACCESS_DENIED: process exists but we can't open it.
        return ::GetLastError() == ERROR_ACCESS_DENIED;
    }
}
