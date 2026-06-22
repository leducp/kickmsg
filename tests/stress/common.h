#ifndef KICKMSG_STRESS_COMMON_H
#define KICKMSG_STRESS_COMMON_H

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#include "kickmsg/os/Time.h"

#include "kickmsg/Publisher.h"
#include "kickmsg/Subscriber.h"

using namespace kickmsg;

#if defined(__SANITIZE_THREAD__)
    constexpr int TSAN_SCALE = 100;
#elif defined(__has_feature)
  #if __has_feature(thread_sanitizer)
    constexpr int TSAN_SCALE = 100;
  #else
    constexpr int TSAN_SCALE = 1;
  #endif
#else
    constexpr int TSAN_SCALE = 1;
#endif

// Target TOTAL contention threads (publishers + subscribers) as a percentage
// of the host core count. Default 150 (~1.5x cores: oversubscribed enough to
// contend, bounded enough to finish). Settable from the stress binary's
// command line so a run can be dialed up or down. Read by contention_count().
extern uint16_t g_oversub_pct;

// Per-side thread count for a contention scenario, derived from the host core
// count and g_oversub_pct. This SCALES with the machine (a 192-core box gets
// hundreds of threads, still oversubscribed) instead of a fixed count that
// would leave a big box undersubscribed -- while staying bounded on a 2-core
// CI runner. Floored at 2. Callers must size max_subs / max_subscribers to
// match the returned value.
uint16_t contention_count();

struct Payload
{
    static constexpr uint32_t MAGIC = 0xCAFEBABE;
    uint32_t magic;
    uint32_t pub_id;
    uint32_t seq;
    uint32_t checksum;
};

inline uint32_t compute_checksum(Payload const& p)
{
    return p.magic ^ p.pub_id ^ p.seq ^ 0xDEADBEEF;
}

struct TestConfig
{
    int      num_publishers   = 4;
    int      num_subscribers  = 8;
    uint32_t msgs_per_pub     = 50000;
    std::size_t pool_size     = 512;
    std::size_t ring_capacity = 128;
    std::size_t max_subs      = 16;
    bool     use_zerocopy     = false;
};

struct SubResult
{
    int      sub_id;
    uint64_t received;
    uint64_t lost;
    uint64_t corrupted;
    uint64_t bad_pub_id;
    uint64_t reordered;
};

struct MsgTrace
{
    uint32_t pub_id;
    uint32_t seq;
    uint64_t ring_pos;
};

static constexpr std::size_t TRACE_SIZE = 16;

// ---- Shared scenario state + helpers (defined in common.cc) ----
// Scenarios reset the globals before spawning threads.

extern std::atomic<bool> g_all_publishers_done;

// Readiness barrier: subscribers signal after construction, publishers wait,
// so every ring is Live for the whole run and per-subscriber accounting is exact.
extern std::atomic<int> g_subscribers_ready;
extern int              g_subscribers_expected;

// Messages send_bounded() actually committed, and publishers that gave up.
// Conservation oracles use g_published (not the nominal count) because a
// publisher may stop early under sustained backpressure.
extern std::atomic<uint64_t> g_published;
extern std::atomic<uint64_t> g_publisher_giveups;

void wait_subscribers_ready();

// EAGAIN retry deadline for send_bounded; 5 s default, override via
// KICKMSG_SEND_GIVEUP_MS (the suite uses a small value to exercise the giveup
// path deterministically).
milliseconds send_giveup_deadline();

// A tight pool can stay exhausted forever: slots recycle only on eviction or
// teardown, never on consume, so once every publisher blocks in allocate()
// nothing frees a slot. send_bounded caps the EAGAIN retry rather than
// spinning: returns false (booking a giveup) if the pool stayed full past
// send_giveup_deadline(), true after a committed send.
bool send_bounded(kickmsg::Publisher& pub, Payload const& msg, int pub_id);

// Publish `count` checksummed messages, stopping early if send_bounded gives up.
void publisher_thread(kickmsg::SharedRegion& region, int pub_id, uint32_t count);

// Validate one received message against per-publisher sequence monotonicity
// and its checksum, updating SubResult counters and the reorder trace.
void validate_payload(Payload const& msg, int num_pubs, uint64_t ring_pos,
                      std::vector<uint32_t>& last_seq,
                      std::vector<uint64_t>& last_pos,
                      SubResult& result,
                      MsgTrace* trace, std::size_t& trace_pos);

// Copy / zero-copy subscriber loops: consume until publishers are done and the
// ring is drained, validating every sample.
SubResult subscriber_thread_copy(kickmsg::SharedRegion& region, int sub_id,
                                 int num_pubs, uint32_t msgs_per_pub);
SubResult subscriber_thread_zerocopy(kickmsg::SharedRegion& region, int sub_id,
                                     int num_pubs, uint32_t msgs_per_pub);

// No-crash GC oracle: repair must fix nothing and reclaim must stay within the
// observed publisher-drop budget, else the normal path leaked and GC masked it.
bool verify_gc_zero(kickmsg::SharedRegion& region, kickmsg::channel::Config const& cfg);

// Post-run structural checks: full free stack (no leak / dup / range error),
// every ring Free with in_flight 0, every slot refcount 0.
bool verify_pool_free(kickmsg::SharedRegion& region, kickmsg::channel::Config const& cfg);
bool verify_rings_inactive(kickmsg::SharedRegion& region, kickmsg::channel::Config const& cfg);
bool verify_refcounts_zero(kickmsg::SharedRegion& region, kickmsg::channel::Config const& cfg);

struct TestRunner
{
    int pass = 0;
    int fail = 0;

    // Takes the scenario as a callable (not a pre-computed bool) so we can
    // print its name BEFORE running and time it. The pre-run flush means a
    // scenario that hangs leaves its "[ RUN  ]" line on screen -- the last
    // one without a matching "[  OK  ]" is the culprit.
    template <typename Fn>
    void run(char const* name, Fn&& fn)
    {
        std::printf("[ RUN  ] %s\n", name);
        std::fflush(stdout);
        auto const start  = kickmsg::monotonic_ns();
        bool const result = fn();
        auto const ms = duration_cast<milliseconds>(
                            kickmsg::elapsed_time(start)).count();
        char const* tag = "[  OK  ]";
        if (result)
        {
            ++pass;
        }
        else
        {
            ++fail;
            tag = "[ FAIL ]";
        }
        std::printf("%s %s (%lld ms)\n", tag, name, static_cast<long long>(ms));
        std::fflush(stdout);
    }

    int summary()
    {
        std::printf("=== Summary: %d passed, %d failed ===\n", pass, fail);
        return fail > 0 ? 1 : 0;
    }
};

#endif
