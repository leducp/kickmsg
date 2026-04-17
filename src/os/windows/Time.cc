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

    nanoseconds since_epoch()
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

    nanoseconds elapsed_time(nanoseconds start)
    {
        return since_epoch() - start;
    }
}
