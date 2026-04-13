/// @file hello_schema_late_publisher.cc
/// @brief Schema descriptor in a "subscriber arrives first" scenario.
///
/// Companion to hello_schema.cc.  In a real deployment startup order is
/// rarely guaranteed: a listener service might come up before the data
/// source it cares about.  This example shows that flow:
///
///   1. Subscriber (listener) starts first.  It uses subscribe_or_create()
///      so it materializes the region itself when no publisher exists yet.
///      No schema is set at this point.
///
///   2. Subscriber polls topic_schema() in a loop.  Until the schema is
///      published it returns nullopt — the listener simply waits.
///
///   3. Publisher arrives later (here in a thread, in production a separate
///      process).  It uses advertise_or_join() to attach to the existing
///      region without trying to recreate it, then try_claim_topic_schema()
///      atomically publishes the descriptor (Unset → Claiming → Set).
///
///   4. Subscriber's next poll observes the schema, verifies it, and
///      starts consuming.
///
/// The library does no waiting on behalf of the user — the polling loop
/// is entirely policy.  Replace it with whatever fits the deployment
/// (timeout, exponential backoff, futex, etc).

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

#include <kickmsg/Node.h>

using namespace kickmsg;
using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// Schema bytes — same FNV-1a helper as hello_schema.cc.  Real users would
// drop in whatever hash family their tooling already uses.
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

    std::array<uint8_t, 64> identity_bytes(std::string const& descriptor)
    {
        std::array<uint8_t, 64> out{};
        uint64_t h = fnv1a(descriptor);
        std::memcpy(out.data(), &h, sizeof(h));
        return out;
    }

    enum : uint32_t { ALGO_FNV1A_64 = 1 };

    SchemaInfo make_schema(char const* name, std::string const& descriptor,
                           uint32_t version)
    {
        SchemaInfo info{};
        info.identity      = identity_bytes(descriptor);
        std::snprintf(info.name, sizeof(info.name), "%s", name);
        info.version       = version;
        info.identity_algo = ALGO_FNV1A_64;
        return info;
    }
}

// ---------------------------------------------------------------------------

struct Telemetry
{
    uint64_t seq;
    int32_t  battery_pct;
    int32_t  rssi_dbm;
};

constexpr char const* SCHEMA_NAME       = "demo/Telemetry";
constexpr char const* SCHEMA_DESCRIPTOR = "demo.Telemetry(seq:u64, batt:i32, rssi:i32)";
constexpr uint32_t    SCHEMA_VERSION    = 1;

// Both sides must agree on channel geometry; subscribe_or_create / advertise_or_join
// hand it to whichever side wins the create race.
channel::Config make_config()
{
    channel::Config cfg;
    cfg.max_subscribers   = 4;
    cfg.sub_ring_capacity = 8;
    cfg.pool_size         = 16;
    cfg.max_payload_size  = sizeof(Telemetry);
    // Note: the *subscriber* deliberately leaves cfg.schema empty so the
    // schema slot starts Unset.  The publisher will claim it on arrival.
    return cfg;
}

int main()
{
    constexpr char const* PREFIX = "demo_late";
    constexpr char const* TOPIC  = "telemetry";

    SharedMemory::unlink((std::string{"/"} + PREFIX + "_" + TOPIC).c_str());

    // ----- Subscriber starts first, creates the region -----
    Node sub_node("listener", PREFIX);
    auto sub = sub_node.subscribe_or_create(TOPIC, make_config());
    std::cout << "[listener]  region ready, schema = "
              << (sub_node.topic_schema(TOPIC) ? "set" : "unset")
              << " (waiting for publisher to claim)\n";

    // ----- Publisher arrives in another thread after a short delay -----
    std::atomic<bool> publisher_ready{false};
    std::thread publisher_thread([&]()
    {
        std::this_thread::sleep_for(100ms);  // pretend the data source is slow to boot

        Node pub_node("driver", PREFIX);
        auto pub = pub_node.advertise_or_join(TOPIC, make_config());

        SchemaInfo info = make_schema(SCHEMA_NAME, SCHEMA_DESCRIPTOR, SCHEMA_VERSION);
        bool       won  = pub_node.try_claim_topic_schema(TOPIC, info);
        std::cout << "[driver]    joined region, "
                  << (won ? "claimed" : "found existing")
                  << " schema slot (" << SCHEMA_NAME
                  << " v" << SCHEMA_VERSION << ")\n";

        publisher_ready.store(true, std::memory_order_release);

        for (uint64_t i = 0; i < 4; ++i)
        {
            Telemetry t{i, 80 - static_cast<int32_t>(i), -45};
            if (pub.send(&t, sizeof(t)) < 0)
            {
                std::cerr << "[driver]    send failed at seq=" << i << "\n";
            }
            std::this_thread::sleep_for(20ms);
        }
    });

    // ----- Listener polls the schema slot until it's published -----
    SchemaInfo expected = make_schema(SCHEMA_NAME, SCHEMA_DESCRIPTOR, SCHEMA_VERSION);
    std::optional<SchemaInfo> got;
    auto deadline = std::chrono::steady_clock::now() + 2s;

    while (std::chrono::steady_clock::now() < deadline)
    {
        got = sub_node.topic_schema(TOPIC);
        if (got.has_value())
        {
            break;
        }
        std::this_thread::sleep_for(10ms);  // policy: poll cadence is the user's choice
    }

    if (not got.has_value())
    {
        std::cerr << "[listener]  timed out waiting for publisher to claim schema\n";
        publisher_thread.join();
        return 1;
    }

    bool match = (got->identity == expected.identity)
                 and (got->version == expected.version);
    std::cout << "[listener]  observed schema '" << got->name
              << "' v" << got->version
              << " — " << (match ? "OK, consuming" : "mismatch, refusing") << "\n";

    if (not match)
    {
        publisher_thread.join();
        return 1;
    }

    // ----- Listener consumes once the schema is verified -----
    int received = 0;
    auto consume_until = std::chrono::steady_clock::now() + 500ms;
    while (std::chrono::steady_clock::now() < consume_until)
    {
        if (auto sample = sub.try_receive())
        {
            Telemetry t;
            std::memcpy(&t, sample->data(), sizeof(t));
            std::cout << "[listener]  seq=" << t.seq
                      << "  batt=" << t.battery_pct << "%"
                      << "  rssi=" << t.rssi_dbm << " dBm\n";
            ++received;
            if (received == 4)
            {
                break;
            }
        }
        else
        {
            std::this_thread::sleep_for(5ms);
        }
    }

    publisher_thread.join();
    std::cout << "[listener]  consumed " << received << " sample(s)\n";

    SharedMemory::unlink((std::string{"/"} + PREFIX + "_" + TOPIC).c_str());
    std::cout << "Done.\n";
    return 0;
}
