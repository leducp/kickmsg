/// @file hello_schema.cc
/// @brief Kickmsg payload schema descriptor via the Node API.
///
/// Kickmsg carries opaque bytes — the library never inspects payload
/// layout. That's exactly what the hot path needs, but it leaves a real
/// IPC problem: two processes attached to the same topic can disagree on
/// what the bytes mean, because they were built at different times or
/// against different message definitions.
///
/// The optional SchemaInfo descriptor gives users a place to publish a
/// fingerprint of the payload format so receivers can detect mismatches.
/// The library only provides the slot, an atomic claim protocol, and
/// a small pure diff helper; the caller chooses what bytes go in and
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

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>

#include <kickmsg/Hash.h>
#include <kickmsg/Node.h>

using namespace kickmsg;

// ---------------------------------------------------------------------------
// The message type and its schema descriptor.  Bumping the layout (adding
// a field, reordering, changing types) should bump SCHEMA_VERSION AND
// change the descriptor string so the identity hash differs too.
// ---------------------------------------------------------------------------

struct ImuSample
{
    uint64_t timestamp_ns;
    float    ax, ay, az;
    float    gx, gy, gz;
};

constexpr char const* SCHEMA_NAME       = "demo/Imu";
constexpr char const* SCHEMA_DESCRIPTOR =
    "demo.Imu(timestamp_ns:u64, ax:f32, ay:f32, az:f32, "
    "gx:f32, gy:f32, gz:f32)";
constexpr uint32_t    SCHEMA_VERSION    = 2;

// User-defined tag so tooling can tell "these bytes are an FNV-1a" vs
// some other hash family — library ignores it.
constexpr uint32_t    ALGO_FNV1A_64     = 1;

SchemaInfo make_imu_schema(std::string const& descriptor, uint32_t version)
{
    SchemaInfo info{};
    info.identity      = hash::identity_from_fnv1a(descriptor);
    // layout unused in this demo — a real app might fill it with a
    // Fletcher checksum over member (offset, size, kind) tuples so ABI
    // skew can be distinguished from "different type altogether".
    std::snprintf(info.name, sizeof(info.name), "%s", SCHEMA_NAME);
    info.version       = version;
    info.identity_algo = ALGO_FNV1A_64;
    return info;
}

// ---------------------------------------------------------------------------
// Subscriber-side mismatch policy.  Library never enforces — caller does.
// We treat identity and version as fatal, everything else as advisory.
// ---------------------------------------------------------------------------

char const* describe_diff(uint32_t d)
{
    if (d == schema::Equal)          return "OK";
    if (d & schema::Identity)        return "identity mismatch (different type)";
    if (d & schema::Version)         return "version mismatch (same type, different rev)";
    if (d & schema::Layout)          return "layout mismatch (ABI skew)";
    if (d & schema::IdentityAlgo
        || d & schema::LayoutAlgo)   return "algo tag mismatch";
    if (d & schema::Name)            return "name mismatch (advisory)";
    return "?";
}

bool is_fatal(uint32_t d)
{
    // User policy: identity and version are fatal; other diffs are logged
    // but tolerated.  Another app might treat Layout as fatal too.
    return (d & (schema::Identity | schema::Version)) != 0;
}

int main()
{
    constexpr char const* PREFIX = "demo_schema";
    constexpr char const* TOPIC  = "imu";

    // ----- Publisher: advertise "imu" with v2 schema baked in -----
    Node pub_node("imu_driver", PREFIX);
    pub_node.unlink_topic(TOPIC);  // clean any leftover SHM from prior runs

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
            std::cout << "[good_sub]  refused: no schema published\n";
            return 1;
        }
        uint32_t d = schema::diff(*got, expected);
        std::cout << "[good_sub]  observed schema '" << got->name
                  << "' v" << got->version
                  << " — " << describe_diff(d) << "\n";
        if (is_fatal(d))
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

        uint32_t d           = schema::Equal;
        uint32_t got_version = 0;
        if (got.has_value())
        {
            d           = schema::diff(*got, expected);
            got_version = got->version;
        }

        std::cout << "[bad_sub]   expected v1, observed v" << got_version
                  << " — " << describe_diff(d) << "\n";
        if (got.has_value() and is_fatal(d))
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
    // Note: bad_sub's ring still received the messages at the SHM layer
    // — the publisher delivers to every Live ring unconditionally.  The
    // "refusal" is purely application-level: bad_sub chooses not to pop
    // the ring (which it would otherwise drain with try_receive).
    if (not bad_should_consume)
    {
        std::cout << "[bad_sub]   consumed 0 sample(s) (refused on schema mismatch)\n";
    }

    pub_node.unlink_topic(TOPIC);
    std::cout << "Done.\n";
    return 0;
}
