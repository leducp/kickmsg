#include <gtest/gtest.h>

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
    EXPECT_EQ(kickmsg::hash::fnv1a_64(nullptr, 0), 0xCBF29CE484222325ULL);
    EXPECT_EQ(kickmsg::hash::fnv1a_64(std::string_view{}), 0xCBF29CE484222325ULL);
}

TEST(Hash, Fnv1aRawAndStringViewAgree)
{
    std::string const s = "demo.Imu(timestamp_ns:u64, ax:f32)";
    uint64_t from_sv  = kickmsg::hash::fnv1a_64(s);
    uint64_t from_raw = kickmsg::hash::fnv1a_64(s.data(), s.size());
    EXPECT_EQ(from_sv, from_raw);
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
        s.identity_algo = kickmsg::schema::fnv1a_64;
        s.layout_algo   = kickmsg::schema::xxh64;
        s.flags         = 0;
        s.blob_hash     = kickmsg::hash::identity_from_fnv1a("message Imu { ... }");
        s.blob_size     = 19;
        s.blob_algo     = kickmsg::schema::fnv1a_64;
        return s;
    }
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
    {
        auto b = base;
        b.blob_hash[63] ^= 0xFF;
        EXPECT_EQ(kickmsg::schema::diff(base, b),
                  static_cast<uint32_t>(kickmsg::schema::Blob));
    }
    {
        auto b = base;
        b.blob_algo = kickmsg::schema::sha256;
        EXPECT_EQ(kickmsg::schema::diff(base, b),
                  static_cast<uint32_t>(kickmsg::schema::BlobAlgo));
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

TEST(SchemaDiff, FlagsReservedAndBlobSizeAreIgnored)
{
    // Forward-compatibility contract: changes to `flags` or `reserved[]`
    // must not register as a diff so future field additions don't
    // silently break users of diff().  blob_size is library-written, so a
    // reader's expected descriptor never carries it.
    auto a = sample_schema();
    auto b = a;
    b.flags        = 0xDEADBEEF;
    b.blob_size    = a.blob_size + 1;
    b.reserved[0]  = 0x42;
    b.reserved[sizeof(b.reserved) - 1] = 0x24;
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

TEST(SchemaAlgo, DigestSizesFitTheFingerprintSlots)
{
    struct Expected
    {
        uint32_t    algo;
        std::size_t size;
        char const* name;
    };
    Expected const table[] = {
        {kickmsg::schema::fnv1a_64,    8,  "fnv1a_64"},
        {kickmsg::schema::xxh64,       8,  "xxh64"},
        {kickmsg::schema::xxh3_64,     8,  "xxh3_64"},
        {kickmsg::schema::xxh3_128,    16, "xxh3_128"},
        {kickmsg::schema::crc32c,      4,  "crc32c"},
        {kickmsg::schema::md5,         16, "md5"},
        {kickmsg::schema::sha1,        20, "sha1"},
        {kickmsg::schema::sha256,      32, "sha256"},
        {kickmsg::schema::sha384,      48, "sha384"},
        {kickmsg::schema::sha512,      64, "sha512"},
        {kickmsg::schema::sha3_256,    32, "sha3_256"},
        {kickmsg::schema::sha3_512,    64, "sha3_512"},
        {kickmsg::schema::blake2s_256, 32, "blake2s_256"},
        {kickmsg::schema::blake2b_256, 32, "blake2b_256"},
        {kickmsg::schema::blake2b_512, 64, "blake2b_512"},
        {kickmsg::schema::blake3_256,  32, "blake3_256"},
    };
    for (auto const& expected : table)
    {
        EXPECT_EQ(kickmsg::schema::digest_size(expected.algo), expected.size) << expected.name;
        EXPECT_LE(kickmsg::schema::digest_size(expected.algo), sizeof(kickmsg::SchemaInfo::blob_hash));
        ASSERT_NE(kickmsg::schema::algo_name(expected.algo), nullptr);
        EXPECT_STREQ(kickmsg::schema::algo_name(expected.algo), expected.name);
    }
}

TEST(SchemaAlgo, UnknownAndUserTagsHaveNoSizeOrName)
{
    uint32_t const tags[] = {kickmsg::schema::unspecified, 6, 15, 27, 0x7FFFFFFFu,
                             kickmsg::schema::user_base, kickmsg::schema::user_base + 7};
    for (uint32_t tag : tags)
    {
        EXPECT_EQ(kickmsg::schema::digest_size(tag), 0u) << tag;
        EXPECT_EQ(kickmsg::schema::algo_name(tag), nullptr) << tag;
    }
}
