#include "kickmsg/os/Time.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace kickmsg
{
    void sleep(nanoseconds ns)
    {
        auto ms = duration_cast<milliseconds>(ns);
        if (ms.count() <= 0)
        {
            yield();
            return;
        }
        Sleep(static_cast<DWORD>(ms.count()));
    }

    void yield()
    {
        ::SwitchToThread();
    }

    nanoseconds monotonic_ns()
    {
        static LARGE_INTEGER freq{};
        if (freq.QuadPart == 0)
        {
            QueryPerformanceFrequency(&freq);
        }

        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);

        auto secs  = now.QuadPart / freq.QuadPart;
        auto frac  = now.QuadPart % freq.QuadPart;
        auto nanos = (frac * 1'000'000'000) / freq.QuadPart;

        return seconds{secs} + nanoseconds{nanos};
    }

    nanoseconds since_epoch()
    {
        // FILETIME is 100-ns intervals since 1601-01-01 UTC.  Shift the
        // epoch to 1970-01-01 UTC: 11644473600 seconds.
        FILETIME ft;
        GetSystemTimePreciseAsFileTime(&ft);
        ULARGE_INTEGER u;
        u.LowPart  = ft.dwLowDateTime;
        u.HighPart = ft.dwHighDateTime;
        constexpr uint64_t epoch_offset_100ns = 116444736000000000ULL;
        uint64_t ns100 = u.QuadPart - epoch_offset_100ns;
        return nanoseconds{ns100 * 100};
    }

    nanoseconds elapsed_time(nanoseconds start)
    {
        return monotonic_ns() - start;
    }
}
