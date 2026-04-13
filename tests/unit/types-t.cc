#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "kickmsg/types.h"
#include "kickmsg/Hash.h"

TEST(Helpers, AlignUp)
{
    EXPECT_EQ(kickmsg::align_up(0, 64), 0u);
    EXPECT_EQ(kickmsg::align_up(1, 64), 64u);
    EXPECT_EQ(kickmsg::align_up(64, 64), 64u);
    EXPECT_EQ(kickmsg::align_up(65, 64), 128u);
    EXPECT_EQ(kickmsg::align_up(127, 128), 128u);
}

TEST(Helpers, IsPowerOfTwo)
{
    EXPECT_FALSE(kickmsg::is_power_of_two(0));
    EXPECT_TRUE(kickmsg::is_power_of_two(1));
    EXPECT_TRUE(kickmsg::is_power_of_two(2));
    EXPECT_FALSE(kickmsg::is_power_of_two(3));
    EXPECT_TRUE(kickmsg::is_power_of_two(64));
    EXPECT_FALSE(kickmsg::is_power_of_two(100));
    EXPECT_TRUE(kickmsg::is_power_of_two(1024));
}

TEST(Helpers, TaggedPackRoundtrip)
{
    for (uint32_t gen : {0u, 1u, 42u, UINT32_MAX})
    {
        for (uint32_t idx : {0u, 1u, 255u, kickmsg::INVALID_SLOT})
        {
            auto packed = kickmsg::tagged_pack(gen, idx);
            EXPECT_EQ(kickmsg::tagged_gen(packed), gen);
            EXPECT_EQ(kickmsg::tagged_idx(packed), idx);
        }
    }
}

// --- hash::fnv1a_64 --------------------------------------------------------

TEST(Hash, Fnv1aKnownVectors)
{
    // Canonical FNV-1a 64-bit test vectors (www.isthe.com/chongo/tech/comp/fnv/).
    // These are the byte-level reference outputs for the standard algorithm.
    EXPECT_EQ(kickmsg::hash::fnv1a_64(std::string_view{""}),
              0xCBF29CE484222325ULL);
    EXPECT_EQ(kickmsg::hash::fnv1a_64(std::string_view{"a"}),
              0xAF63DC4C8601EC8CULL);
    EXPECT_EQ(kickmsg::hash::fnv1a_64(std::string_view{"foobar"}),
              0x85944171F73967E8ULL);
}

TEST(Hash, Fnv1aRawAndStringViewAgree)
{
    std::string const s = "demo.Imu(timestamp_ns:u64, ax:f32)";
    uint64_t from_sv  = kickmsg::hash::fnv1a_64(s);
    uint64_t from_raw = kickmsg::hash::fnv1a_64(s.data(), s.size());
    EXPECT_EQ(from_sv, from_raw);
}

TEST(Hash, Fnv1aEmptyRange)
{
    // Explicit check that the offset basis is returned for zero bytes.
    EXPECT_EQ(kickmsg::hash::fnv1a_64(nullptr, 0), 0xCBF29CE484222325ULL);
    EXPECT_EQ(kickmsg::hash::fnv1a_64(std::string_view{}), 0xCBF29CE484222325ULL);
}

TEST(Hash, IdentityFromFnv1aPacksLeadingBytesAndZeroPads)
{
    auto id = kickmsg::hash::identity_from_fnv1a("demo/Imu");

    // Leading 8 bytes: FNV-1a in host byte order (same-host IPC).
    uint64_t expected = kickmsg::hash::fnv1a_64(std::string_view{"demo/Imu"});
    uint64_t got      = 0;
    std::memcpy(&got, id.data(), sizeof(got));
    EXPECT_EQ(got, expected);

    // Remaining 56 bytes must be zero.
    for (std::size_t i = 8; i < id.size(); ++i)
    {
        EXPECT_EQ(id[i], 0u) << "byte " << i << " not zero-padded";
    }
}

TEST(Hash, IdentityFromFnv1aDifferentDescriptorsDiffer)
{
    auto a = kickmsg::hash::identity_from_fnv1a("demo/Imu");
    auto b = kickmsg::hash::identity_from_fnv1a("demo/Twist");
    EXPECT_NE(a, b);
}

// --- schema::diff ----------------------------------------------------------

namespace
{
    kickmsg::SchemaInfo sample_schema()
    {
        kickmsg::SchemaInfo s{};
        s.identity      = kickmsg::hash::identity_from_fnv1a("demo/Imu");
        s.layout        = kickmsg::hash::identity_from_fnv1a("layout:v2");
        std::snprintf(s.name, sizeof(s.name), "demo/Imu");
        s.version       = 2;
        s.identity_algo = 1;
        s.layout_algo   = 2;
        s.flags         = 0;
        return s;
    }
}

TEST(SchemaDiff, EqualWhenSame)
{
    auto a = sample_schema();
    auto b = a;
    EXPECT_EQ(kickmsg::schema::diff(a, b), kickmsg::schema::Equal);
}

TEST(SchemaDiff, SingleFieldBits)
{
    auto base = sample_schema();

    {
        auto b = base;
        b.identity[0] ^= 0xFF;
        EXPECT_EQ(kickmsg::schema::diff(base, b),
                  static_cast<uint32_t>(kickmsg::schema::Identity));
    }
    {
        auto b = base;
        b.layout[5] ^= 0xFF;
        EXPECT_EQ(kickmsg::schema::diff(base, b),
                  static_cast<uint32_t>(kickmsg::schema::Layout));
    }
    {
        auto b = base;
        b.version = 99;
        EXPECT_EQ(kickmsg::schema::diff(base, b),
                  static_cast<uint32_t>(kickmsg::schema::Version));
    }
    {
        auto b = base;
        std::snprintf(b.name, sizeof(b.name), "other/Imu");
        EXPECT_EQ(kickmsg::schema::diff(base, b),
                  static_cast<uint32_t>(kickmsg::schema::Name));
    }
    {
        auto b = base;
        b.identity_algo = 42;
        EXPECT_EQ(kickmsg::schema::diff(base, b),
                  static_cast<uint32_t>(kickmsg::schema::IdentityAlgo));
    }
    {
        auto b = base;
        b.layout_algo = 42;
        EXPECT_EQ(kickmsg::schema::diff(base, b),
                  static_cast<uint32_t>(kickmsg::schema::LayoutAlgo));
    }
}

TEST(SchemaDiff, MultipleFieldsAccumulate)
{
    auto a = sample_schema();
    auto b = a;
    b.version = a.version + 1;
    b.identity[0] ^= 0xFF;

    uint32_t d = kickmsg::schema::diff(a, b);
    EXPECT_TRUE(d & kickmsg::schema::Identity);
    EXPECT_TRUE(d & kickmsg::schema::Version);
    EXPECT_FALSE(d & kickmsg::schema::Layout);
    EXPECT_FALSE(d & kickmsg::schema::Name);
}

TEST(SchemaDiff, FlagsAndReservedAreIgnored)
{
    // Forward-compatibility contract: changes to `flags` or `reserved[]`
    // must not register as a diff so future field additions don't
    // silently break users of diff().
    auto a = sample_schema();
    auto b = a;
    b.flags        = 0xDEADBEEF;
    b.reserved[0]  = 0x42;
    b.reserved[239] = 0x24;
    EXPECT_EQ(kickmsg::schema::diff(a, b), kickmsg::schema::Equal);
}

TEST(SchemaDiff, NameTrailingZerosDoNotRegister)
{
    // Two descriptors that agree through the first NUL must compare equal
    // on Name regardless of whatever bytes sit past the terminator (the
    // 128-byte slot is zero-initialized, but user code may copy in a
    // shorter string over a previously-used slot).
    auto a = sample_schema();
    auto b = a;
    // Poison bytes AFTER the NUL in b.name; logical string is unchanged.
    std::size_t len = std::strlen(a.name);
    b.name[len + 1] = 'X';
    EXPECT_FALSE(kickmsg::schema::diff(a, b) & kickmsg::schema::Name);
}
