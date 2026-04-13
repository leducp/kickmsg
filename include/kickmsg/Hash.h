#ifndef KICKMSG_HASH_H
#define KICKMSG_HASH_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace kickmsg
{
    /// Optional hash helpers.  Not used on any hot path — intended for
    /// users filling SchemaInfo::identity without bringing their own
    /// hash implementation, and for building up descriptor fingerprints
    /// in user code more generally.
    ///
    /// FNV-1a is non-cryptographic: suitable for identity fingerprints
    /// of honest, non-adversarial peers agreeing on a payload format,
    /// which is exactly the Kickmsg IPC use case.  Users who need
    /// collision resistance against adversaries (signatures, integrity
    /// checks) should bring their own SHA-256 / BLAKE2 / etc.
    namespace hash
    {
        /// Standard FNV-1a 64-bit offset basis.  Passing a different seed
        /// into fnv1a_64() lets callers chain multiple ranges into a
        /// single hash without intermediate concatenation:
        ///     uint64_t h = fnv1a_64(a, sizeof(a));
        ///     h = fnv1a_64(b, sizeof(b), h);
        ///     h = fnv1a_64(c, sizeof(c), h);
        constexpr uint64_t FNV1A_64_OFFSET_BASIS = 14695981039346656037ULL;

        /// 64-bit FNV-1a of a raw byte range.  `seed` defaults to the
        /// standard offset basis for one-shot hashing; pass a previous
        /// hash value to chain additional bytes into it.
        uint64_t fnv1a_64(void const* data, std::size_t len,
                          uint64_t seed = FNV1A_64_OFFSET_BASIS) noexcept;

        /// 64-bit FNV-1a of a string.  Thin wrapper around the raw-range
        /// overload; preserved as a separate entry point because
        /// descriptor-string hashing is by far the most common use.
        uint64_t fnv1a_64(std::string_view s) noexcept;

        /// Convenience: pack a 64-bit FNV-1a of `descriptor` into the
        /// leading eight bytes of a 64-byte identity slot, zero-padding
        /// the remaining 56 bytes.  Intended as a drop-in for filling
        /// SchemaInfo::identity when a wider hash isn't needed.
        std::array<uint8_t, 64> identity_from_fnv1a(std::string_view descriptor) noexcept;
    }
}

#endif
