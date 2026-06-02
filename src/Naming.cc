#include "kickmsg/Naming.h"
#include "kickmsg/Hash.h"

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdint>
#include <limits.h>
#include <stdexcept>
#include <string>
#include <system_error>

#if defined(__APPLE__) || defined(__DARWIN__)
    #include <sys/posix_shm.h>
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
        // shm_open accepts '/' + up to NAME_MAX bytes for the filename portion on tmpfs.
        std::string out = "/";
        out += ns;
        out += '_';
        out += suffix;
        // -1 because / is not taken into account
        if (out.size() - 1 > static_cast<std::size_t>(NAME_MAX))
        {
            throw std::system_error(ENAMETOOLONG, std::generic_category(),
                "kickmsg::compose_shm_name: shm name exceeds NAME_MAX");
        }
        return out;
#endif
    }
}
