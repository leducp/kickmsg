#include "kickmsg/os/Process.h"

#include <cstdio>
#include <cstring>

namespace kickmsg
{
    uint64_t process_starttime(uint64_t pid) noexcept
    {
        if (pid == 0)
        {
            return 0;
        }
        // /proc/<pid>/stat field 22 (`starttime`, clock ticks since boot).
        // The `comm` field (2) can contain spaces and parens — skip to
        // the last ')' and parse space-separated fields from there.
        char path[64];
        std::snprintf(path, sizeof(path), "/proc/%llu/stat",
                      static_cast<unsigned long long>(pid));
        std::FILE* f = std::fopen(path, "r");
        if (f == nullptr)
        {
            return 0;
        }
        char buf[512];
        std::size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
        std::fclose(f);
        if (n == 0)
        {
            return 0;
        }
        buf[n] = '\0';
        char const* close_paren = std::strrchr(buf, ')');
        if (close_paren == nullptr)
        {
            return 0;
        }
        char const* p = close_paren + 1;
        for (int i = 0; i < 19; ++i)
        {
            while (*p == ' ') ++p;
            while (*p != '\0' and *p != ' ') ++p;
        }
        while (*p == ' ') ++p;
        unsigned long long starttime = 0;
        if (std::sscanf(p, "%llu", &starttime) != 1)
        {
            return 0;
        }
        return static_cast<uint64_t>(starttime);
    }
}
