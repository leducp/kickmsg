#include "kickmsg/os/Process.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace kickmsg
{
    uint64_t current_pid() noexcept
    {
        return static_cast<uint64_t>(::GetCurrentProcessId());
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
