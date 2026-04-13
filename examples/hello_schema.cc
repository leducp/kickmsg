/// @file hello_schema.cc
/// @brief KickMsg payload schema descriptor via the Node API.
///
/// KickMsg carries opaque bytes — the library never inspects payload
/// layout. That's exactly what the hot path needs, but it leaves a real
/// IPC problem: two processes attached to the same topic can disagree on
/// what the bytes mean, because they were built at different times or
/// against different message definitions.
///
/// The optional SchemaInfo descriptor gives users a place to publish a
/// fingerprint of the payload format so receivers can detect mismatches.
/// The library only provides the slot and an atomic claim protocol; the
/// caller chooses what bytes go in (a SHA, an FNV, a UUID, a protobuf
/// descriptor hash, a Fletcher checksum of struct member offsets...) and
/// what to do on mismatch (refuse, log, fall back, version-negotiate).
///
/// This demo runs three nodes in one process:
///   - publisher: advertises "imu" and bakes a v2 schema
///   - good_sub:  subscribes, verifies it agrees on identity+version,
///                proceeds to receive
///   - bad_sub:   subscribes expecting a v1 schema, detects the mismatch,
///                refuses to consume
///
/// In production these would be three separate processes; the schema
/// check works the same way across process boundaries because the
/// descriptor lives in shared memory.

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>

#include <kickmsg/Node.h>

using namespace kickmsg;

// ---------------------------------------------------------------------------
// User-supplied fingerprint helpers.  The library never calls these — any
// hash family works (SHA-256, FNV, xxhash, UUID...).  We use a tiny FNV-1a
// here to keep the example self-contained.
// ---------------------------------------------------------------------------

namespace
{
    constexpr uint64_t FNV_OFFSET = 14695981039346656037ULL;
    constexpr uint64_t FNV_PRIME  = 1099511628211ULL;

    uint64_t fnv1a(std::string const& s)
    {
        uint64_t h = FNV_OFFSET;
        for (auto c : s)
        {
            h ^= static_cast<uint8_t>(c);
            h *= FNV_PRIME;
        }
        return h;
    }

    /// Pack a 64-bit hash into the leading bytes of a 64-byte slot.
    /// Real users would put a SHA-256 / SHA-512 / 128-bit hash etc here.
    std::array<uint8_t, 64> identity_bytes(std::string const& descriptor)
    {
        std::array<uint8_t, 64> out{};
        uint64_t h = fnv1a(descriptor);
        std::memcpy(out.data(), &h, sizeof(h));
        return out;
    }

    enum : uint32_t
    {
        ALGO_FNV1A_64 = 1,  // user-defined tag — library ignores it
    };
}

// ---------------------------------------------------------------------------
// The message type and its schema descriptor.  Bumping the type's layout
// (adding a field, reordering, changing types) should bump SCHEMA_VERSION
// AND change the descriptor string so the identity hash differs too.
// ---------------------------------------------------------------------------

struct ImuSample
{
    uint64_t timestamp_ns;
    float    ax, ay, az;
    float    gx, gy, gz;
};

constexpr char const* SCHEMA_NAME      = "demo/Imu";
constexpr char const* SCHEMA_DESCRIPTOR =
    "demo.Imu(timestamp_ns:u64, ax:f32, ay:f32, az:f32, "
    "gx:f32, gy:f32, gz:f32)";
constexpr uint32_t    SCHEMA_VERSION   = 2;

SchemaInfo make_imu_schema(std::string const& descriptor, uint32_t version)
{
    SchemaInfo info{};                                    // zeroes reserved[]
    info.identity      = identity_bytes(descriptor);
    info.layout        = {};                              // unused in this demo
    std::snprintf(info.name, sizeof(info.name), "%s", SCHEMA_NAME);
    info.version       = version;
    info.identity_algo = ALGO_FNV1A_64;
    info.layout_algo   = 0;
    info.flags         = 0;
    return info;
}

// ---------------------------------------------------------------------------
// Subscriber-side mismatch policy.  Library never enforces — caller does.
// ---------------------------------------------------------------------------

enum class SchemaCheck { Ok, Missing, IdentityMismatch, VersionMismatch };

SchemaCheck verify(SchemaInfo const& got, SchemaInfo const& expected)
{
    if (got.identity != expected.identity)
    {
        return SchemaCheck::IdentityMismatch;
    }
    if (got.version != expected.version)
    {
        return SchemaCheck::VersionMismatch;
    }
    return SchemaCheck::Ok;
}

char const* describe(SchemaCheck c)
{
    switch (c)
    {
        case SchemaCheck::Ok:                return "OK";
        case SchemaCheck::Missing:           return "no schema published";
        case SchemaCheck::IdentityMismatch:  return "identity mismatch (different type)";
        case SchemaCheck::VersionMismatch:   return "version mismatch (same type, different rev)";
    }
    return "?";
}

int main()
{
    constexpr char const* PREFIX = "demo_schema";
    constexpr char const* TOPIC  = "imu";

    SharedMemory::unlink((std::string{"/"} + PREFIX + "_" + TOPIC).c_str());

    // ----- Publisher: advertise "imu" with v2 schema baked in -----
    Node pub_node("imu_driver", PREFIX);

    channel::Config cfg;
    cfg.max_subscribers   = 4;
    cfg.sub_ring_capacity = 8;
    cfg.pool_size         = 16;
    cfg.max_payload_size  = sizeof(ImuSample);
    cfg.schema            = make_imu_schema(SCHEMA_DESCRIPTOR, SCHEMA_VERSION);

    auto pub = pub_node.advertise(TOPIC, cfg);
    std::cout << "[publisher] advertised '" << TOPIC
              << "' with schema " << SCHEMA_NAME
              << " v" << SCHEMA_VERSION << "\n";

    // ----- good_sub: expects v2, verifies, consumes -----
    Node good("good_sub", PREFIX);
    auto good_handle = good.subscribe(TOPIC);
    {
        SchemaInfo expected = make_imu_schema(SCHEMA_DESCRIPTOR, SCHEMA_VERSION);
        auto got = good.topic_schema(TOPIC);
        if (not got.has_value())
        {
            std::cout << "[good_sub]  refused: " << describe(SchemaCheck::Missing) << "\n";
            return 1;
        }
        SchemaCheck check = verify(*got, expected);
        std::cout << "[good_sub]  observed schema '" << got->name
                  << "' v" << got->version
                  << " — " << describe(check) << "\n";
        if (check != SchemaCheck::Ok)
        {
            return 1;
        }
    }

    // ----- bad_sub: expects v1, detects mismatch, refuses -----
    Node bad("bad_sub", PREFIX);
    auto bad_handle = bad.subscribe(TOPIC);
    bool bad_should_consume = true;
    {
        SchemaInfo expected = make_imu_schema(SCHEMA_DESCRIPTOR, /*version=*/1);
        auto got = bad.topic_schema(TOPIC);
        SchemaCheck check = got ? verify(*got, expected) : SchemaCheck::Missing;
        std::cout << "[bad_sub]   expected v1, observed v" << (got ? got->version : 0)
                  << " — " << describe(check) << "\n";
        if (check != SchemaCheck::Ok)
        {
            std::cout << "[bad_sub]   refusing to consume bytes it can't safely interpret\n";
            bad_should_consume = false;
        }
    }

    // ----- Publish a few samples -----
    for (uint64_t i = 0; i < 3; ++i)
    {
        ImuSample s{
            /*timestamp_ns=*/i * 10'000'000ULL,
            /*ax=*/0.0f, /*ay=*/0.0f, /*az=*/9.81f,
            /*gx=*/0.0f, /*gy=*/0.0f, /*gz=*/0.0f
        };
        if (pub.send(&s, sizeof(s)) < 0)
        {
            std::cerr << "[publisher] send failed at " << i << "\n";
        }
    }

    // ----- good_sub consumes -----
    int good_count = 0;
    while (auto sample = good_handle.try_receive())
    {
        ImuSample s;
        std::memcpy(&s, sample->data(), sizeof(s));
        std::cout << "[good_sub]  t=" << s.timestamp_ns
                  << "ns  az=" << s.az << "\n";
        ++good_count;
    }
    std::cout << "[good_sub]  consumed " << good_count << " sample(s)\n";

    // ----- bad_sub stays silent (its mismatch policy was: refuse) -----
    if (not bad_should_consume)
    {
        std::cout << "[bad_sub]   consumed 0 sample(s) (refused on schema mismatch)\n";
    }

    SharedMemory::unlink((std::string{"/"} + PREFIX + "_" + TOPIC).c_str());
    std::cout << "Done.\n";
    return 0;
}
