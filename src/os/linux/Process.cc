#include "kickmsg/os/Process.h"

#include <cstdio>
#include <cstring>

namespace kickmsg
{
    ProcessProbe process_probe(uint64_t pid) noexcept
    {
        ProcessProbe out;
        if (pid == 0)
        {
            return out;
        }
        // /proc/<pid>/stat fields 3 (`state`) and 22 (`starttime`, clock ticks
        // since boot).  The `comm` field (2) can contain spaces and parens --
        // skip to the last ')' and parse space-separated fields from there.
        char path[64];
        std::snprintf(path, sizeof(path), "/proc/%llu/stat",
                      static_cast<unsigned long long>(pid));
        std::FILE* f = std::fopen(path, "r");
        if (f == nullptr)
        {
            return out;
        }
        char buf[512];
        std::size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
        std::fclose(f);
        if (n == 0)
        {
            return out;
        }
        buf[n] = '\0';
        char const* close_paren = std::strrchr(buf, ')');
        if (close_paren == nullptr)
        {
            return out;
        }
        char const* p = close_paren + 1;
        while (*p == ' ') ++p;
        out.exited = *p == 'Z';

        for (int i = 0; i < 19; ++i)
        {
            while (*p != '\0' and *p != ' ') ++p;
            while (*p == ' ') ++p;
        }
        unsigned long long starttime = 0;
        if (std::sscanf(p, "%llu", &starttime) != 1)
        {
            return out;
        }
        out.starttime = static_cast<uint64_t>(starttime);
        return out;
    }
}
