#include "kickmsg/Naming.h"

#include <cctype>
#include <stdexcept>
#include <string>

namespace kickmsg
{
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
}
