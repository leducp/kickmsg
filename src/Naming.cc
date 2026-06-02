#include "kickmsg/Naming.h"
#include "kickmsg/Hash.h"

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <limits.h>
#include <stdexcept>
#include <system_error>

#if defined(__APPLE__) || defined(__DARWIN__)
    #include <sys/posix_shm.h>
#elif defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>   // MAX_PATH
#endif

namespace kickmsg
{
    std::string to_hex(uint64_t v)
    {
        char buf[17];
        std::snprintf(buf, sizeof(buf), "%016llx",
                      static_cast<unsigned long long>(v));
        return std::string(buf, 16);
    }

    std::string sanitize_shm_component(std::string_view s, char const* what)
    {
        std::string out;
        out.reserve(s.size());
        bool leading = true;
        for (char c : s)
        {
            if (leading && c == '/')
            {
                continue;  // strip any leading '/' (including repeats)
            }
            leading = false;
            if (c == '/')
            {
                out.push_back('.');
            }
            else if (std::isalnum(static_cast<unsigned char>(c))
                     || c == '_' || c == '-' || c == '.')
            {
                out.push_back(c);
            }
            else
            {
                out.push_back('_');
            }
        }
        if (out.empty())
        {
            throw std::invalid_argument(
                std::string("kickmsg::sanitize_shm_component: ") + what
                + " name is empty after sanitization (input: '"
                + std::string(s) + "')");
        }
        return out;
    }

    std::string compose_shm_name(std::string_view ns, std::string_view suffix)
    {
#if defined(__APPLE__) || defined(__DARWIN__)
        // PSHMNAMLEN = 31 incl. NUL: cap visible name at PSHMNAMLEN - 1 chars.
        std::string out = "/";
        out += to_hex(hash::fnv1a_64(ns));
        out += to_hex(hash::fnv1a_64(suffix));
        out.resize(PSHMNAMLEN - 1);
        return out;
#else
        // Readable "/ns_suffix" name. Linux shm_open counts NAME_MAX bytes for
        // the filename portion (the leading '/' is not part of it); Windows
        // CreateFileMapping caps the whole object name at MAX_PATH.
        std::string out = "/";
        out += ns;
        out += '_';
        out += suffix;
    #if defined(_WIN32)
        std::size_t const len   = out.size();
        std::size_t const limit = static_cast<std::size_t>(MAX_PATH);
    #else
        std::size_t const len   = out.size() - 1;
        std::size_t const limit = static_cast<std::size_t>(NAME_MAX);
    #endif
        if (len > limit)
        {
            throw std::system_error(ENAMETOOLONG, std::generic_category(),
                "kickmsg::compose_shm_name: shm name exceeds platform limit");
        }
        return out;
#endif
    }
}
