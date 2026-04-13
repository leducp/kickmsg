#include "kickmsg/Hash.h"

#include <cstring>

namespace kickmsg::hash
{
    namespace
    {
        constexpr uint64_t FNV1A_64_PRIME = 1099511628211ULL;
    }

    uint64_t fnv1a_64(void const* data, std::size_t len, uint64_t seed) noexcept
    {
        auto const* p = static_cast<uint8_t const*>(data);
        uint64_t    h = seed;
        for (std::size_t i = 0; i < len; ++i)
        {
            h ^= p[i];
            h *= FNV1A_64_PRIME;
        }
        return h;
    }

    uint64_t fnv1a_64(std::string_view s) noexcept
    {
        return fnv1a_64(s.data(), s.size());
    }

    std::array<uint8_t, 64> identity_from_fnv1a(std::string_view descriptor) noexcept
    {
        std::array<uint8_t, 64> out{};     // zero-initialized
        uint64_t h = fnv1a_64(descriptor);
        // memcpy is the portable way to punch an integer into a byte array
        // without strict-aliasing violations.  Host-endian is fine here:
        // Kickmsg is same-host IPC, so producer and consumer share the
        // architecture.
        std::memcpy(out.data(), &h, sizeof(h));
        return out;
    }
}
