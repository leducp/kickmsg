/// @file stall_repair_test.cc
/// @brief False-positive-death fuzz test for the theft-safe commit protocol.
///
/// A child publisher is SIGSTOPped at random instants, so it sometimes
/// freezes while holding a position-tagged entry lock.  With a tight
/// commit_timeout the stall makes the lock "provably stale": an external
/// repairer (the parent) runs repair_locked_entries() during the stall and
/// steals the entry.  The publisher is then SIGCONTed and resumes.
///
/// The theft guard + CAS commit in Publisher::publish() must turn every
/// such steal into a clean publisher drop:
///   - never a torn payload (magic/checksum validated on every sample),
///   - never a per-publisher sequence rewind (seq strictly increasing),
///   - never refcount corruption (structural pool checks at the end).

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cinttypes>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>

#include "shm_cleanup.h"
#include "kickmsg/os/Time.h"
#include "kickmsg/Publisher.h"
#include "kickmsg/Subscriber.h"

using namespace kickmsg;

static constexpr char const* SHM_NAME = "/kickmsg_stall_repair";
static constexpr microseconds COMMIT_TIMEOUT = 2ms;

struct StallPayload
{
    static constexpr uint32_t MAGIC = 0x57A11CAF;
    uint32_t magic;
    uint32_t pub_id;
    uint32_t seq;
    uint32_t checksum;
};

static uint32_t compute_checksum(StallPayload const& p)
{
    return p.magic ^ p.pub_id ^ p.seq ^ 0xDEADBEEF;
}

// --- Seeded stall-timing fuzzer ---------------------------------------------
// Each SIGSTOP fires at a random instant so a long soak explores new stall
// windows instead of re-hitting a fixed schedule.  The seed is logged at
// startup; set KICKMSG_STALL_SEED to replay a specific run.
namespace
{
    uint64_t g_rng_state = 0;

    uint64_t next_rand() // splitmix64
    {
        g_rng_state += 0x9E3779B97F4A7C15ull;
        uint64_t z = g_rng_state;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }

    // Sleep a random duration in [lo_us, hi_us] inclusive (microsecond grain).
    void sleep_rand(uint64_t lo_us, uint64_t hi_us)
    {
        uint64_t span = hi_us - lo_us + 1;
        kickmsg::sleep(microseconds{static_cast<int64_t>(lo_us + next_rand() % span)});
    }

    uint64_t seed_fuzzer()
    {
        uint64_t seed;
        char const* env = std::getenv("KICKMSG_STALL_SEED");
        if (env != nullptr)
        {
            seed = std::strtoull(env, nullptr, 0);
        }
        else
        {
            seed = static_cast<uint64_t>(monotonic_ns().count())
                 ^ (static_cast<uint64_t>(::getpid()) << 32);
        }
        g_rng_state = seed;
        return seed;
    }

    volatile sig_atomic_t g_child_stop = 0;

    void child_stop_handler(int)
    {
        g_child_stop = 1;
    }
}

/// Aborts on fork failure: an unchecked -1 return would make kill(-1, ...)
/// wipe the entire process group.
static pid_t checked_fork(char const* site)
{
    pid_t p = fork();
    if (p < 0)
    {
        std::fprintf(stderr, "fork() failed at %s: errno=%d\n", site, errno);
        std::_Exit(2);
    }
    return p;
}

/// Child publisher: publishes checksummed payloads with a strictly
/// increasing seq in a tight loop until SIGTERM flips the stop flag.
static void child_publisher_main()
{
    // Replace the inherited shm-cleanup SIGTERM handler: the parent still
    // uses the segment, so the child must convert SIGTERM into a clean loop
    // exit instead of unlinking the region out from under it.
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = child_stop_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    ::sigaction(SIGTERM, &sa, nullptr);

    auto region = kickmsg::SharedRegion::open(SHM_NAME);
    kickmsg::Publisher pub(region);

    uint32_t seq = 0;
    while (g_child_stop == 0)
    {
        auto a = pub.allocate();
        if (a.data == nullptr)
        {
            kickmsg::yield();
            continue;
        }

        StallPayload msg;
        msg.magic    = StallPayload::MAGIC;
        msg.pub_id   = 1;
        msg.seq      = seq;
        msg.checksum = compute_checksum(msg);
        std::memcpy(a.data, &msg, sizeof(msg));

        pub.publish(sizeof(msg));
        // A dropped publish (theft detected) leaves a gap -- gaps are
        // legitimate; the subscriber only rejects a seq going backward.
        ++seq;
    }
}

// --- Subscriber thread -------------------------------------------------------

struct SubStats
{
    std::atomic<uint64_t> received{0};
    std::atomic<uint64_t> corrupted{0};
    std::atomic<uint64_t> rewinds{0};
};

static std::atomic<bool> g_sub_stop{false};

static void subscriber_main(kickmsg::SharedRegion& region, SubStats& stats)
{
    kickmsg::Subscriber sub(region);

    uint32_t last_seq  = 0;
    bool     have_last = false;

    while (true)
    {
        auto sample = sub.receive(50ms);
        if (not sample)
        {
            if (not g_sub_stop.load(std::memory_order_acquire))
            {
                continue;
            }
            // try_receive can return null after exhausting its retry budget
            // on a run of evicted/skip entries while messages remain; null
            // with no lost() progress is the only true ring-empty signal.
            uint64_t lost_before = sub.lost();
            sample = sub.try_receive();
            if (not sample)
            {
                if (sub.lost() == lost_before)
                {
                    break;
                }
                continue;
            }
        }

        if (sample->len() != sizeof(StallPayload))
        {
            ++stats.corrupted;
            continue;
        }

        StallPayload msg;
        std::memcpy(&msg, sample->data(), sizeof(msg));
        if (msg.magic != StallPayload::MAGIC
            or msg.pub_id != 1
            or msg.checksum != compute_checksum(msg))
        {
            ++stats.corrupted;
            continue;
        }

        if (have_last and msg.seq <= last_seq)
        {
            // Cap the per-sample log: one corruption event can cascade into
            // thousands of rewinds and flood the soak evidence dir.
            if (stats.rewinds.load(std::memory_order_relaxed) < 10)
            {
                std::fprintf(stderr, "  [REWIND] seq %u after %u @pos %" PRIu64 "\n",
                             msg.seq, last_seq, sample->ring_pos());
            }
            else if (stats.rewinds.load(std::memory_order_relaxed) == 10)
            {
                std::fprintf(stderr, "  [REWIND] ... further rewinds suppressed\n");
            }
            ++stats.rewinds;
            continue;
        }
        last_seq  = msg.seq;
        have_last = true;
        ++stats.received;
    }
}

// --- Final structural checks --------------------------------------------------

static bool verify_rings_free(kickmsg::SharedRegion& region)
{
    auto* hdr = region.header();
    bool  ok  = true;
    for (uint32_t i = 0; i < hdr->max_subs; ++i)
    {
        auto*    ring   = kickmsg::sub_ring_at(region.base(), hdr, i);
        uint32_t packed = ring->state_flight.load(std::memory_order_acquire);
        if (kickmsg::ring::get_state(packed) != kickmsg::ring::Free)
        {
            std::fprintf(stderr, "  [FAIL] ring %u not Free after teardown (state=%u)\n",
                         i, kickmsg::ring::get_state(packed));
            ok = false;
        }
        if (kickmsg::ring::get_in_flight(packed) != 0)
        {
            std::fprintf(stderr, "  [FAIL] ring %u has in_flight=%u after teardown\n",
                         i, kickmsg::ring::get_in_flight(packed));
            ok = false;
        }
    }
    return ok;
}

/// Every slot must either be a member of the free stack or have refcount 0
/// (a free-stack walk also catches out-of-range and duplicate links, i.e.
/// refcount-driven double-pushes).
static bool verify_slots(kickmsg::SharedRegion& region)
{
    auto* base = region.base();
    auto* hdr  = region.header();

    std::vector<bool> in_free(hdr->pool_size, false);
    uint32_t top = kickmsg::tagged_idx(hdr->free_top.load(std::memory_order_acquire));

    while (top != kickmsg::INVALID_SLOT)
    {
        if (top >= hdr->pool_size)
        {
            std::fprintf(stderr, "  [FAIL] free stack contains out-of-range index %u\n", top);
            return false;
        }
        if (in_free[top])
        {
            std::fprintf(stderr, "  [FAIL] free stack contains duplicate slot %u "
                         "(refcount double-release)\n", top);
            return false;
        }
        in_free[top] = true;

        auto* slot = kickmsg::slot_at(base, hdr, top);
        top = slot->next_free;
    }

    bool ok = true;
    for (uint32_t i = 0; i < hdr->pool_size; ++i)
    {
        if (in_free[i])
        {
            continue;
        }
        auto*    slot = kickmsg::slot_at(base, hdr, i);
        uint32_t rc   = slot->refcount;
        if (rc != 0)
        {
            std::fprintf(stderr, "  [FAIL] slot %u not in free stack and refcount=%u\n", i, rc);
            ok = false;
        }
    }
    return ok;
}

int main(int argc, char** argv)
{
    // ~8 ms per round (pre-stall jitter + staleness wait + grace pass) puts
    // the default run at roughly 3 s of wall time.
    int rounds = 400;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--rounds") == 0 and i + 1 < argc)
        {
            int v = std::atoi(argv[i + 1]);
            if (v > 0)
            {
                rounds = v;
            }
            ++i;
        }
    }

    std::printf("=== Kickmsg Stall/Repair False-Positive-Death Test ===\n");
    kickmsg_test::register_cleanup_shm("/kickmsg_stall_repair");
    kickmsg_test::install_signal_cleanup();

    uint64_t const seed = seed_fuzzer();
    std::printf("stall fuzz seed=%llu (set KICKMSG_STALL_SEED to replay), rounds=%d\n",
                static_cast<unsigned long long>(seed), rounds);

    kickmsg::SharedMemory::unlink(SHM_NAME);

    kickmsg::channel::Config cfg;
    cfg.max_subscribers   = 2;
    cfg.sub_ring_capacity = 8;
    cfg.pool_size         = 32;
    cfg.max_payload_size  = 64;
    cfg.commit_timeout    = COMMIT_TIMEOUT; // tight: stalls are frequently declared stale

    auto region = kickmsg::SharedRegion::create(
        SHM_NAME, kickmsg::channel::PubSub, cfg, "stall_repair_test");

    // Fork the publisher BEFORE spawning any thread (fork in a multithreaded
    // process would leave the child with a possibly-inconsistent heap).
    pid_t pub_pid = checked_fork("publisher");
    if (pub_pid == 0)
    {
        child_publisher_main();
        _exit(0);
    }

    SubStats stats;
    std::thread sub_thread(subscriber_main, std::ref(region), std::ref(stats));

    // Let the subscriber attach and the publisher spin up.
    kickmsg::sleep(30ms);

    bool     all_ok      = true;
    bool     child_alive = true;
    uint64_t steals      = 0;

    for (int round = 0; round < rounds; ++round)
    {
        sleep_rand(0, 3000);

        kill(pub_pid, SIGSTOP);
        int   st = 0;
        pid_t w  = waitpid(pub_pid, &st, WUNTRACED);
        if (w != pub_pid or not WIFSTOPPED(st))
        {
            std::fprintf(stderr, "  [FAIL] round %d: publisher gone before SIGSTOP "
                         "(w=%d, status=0x%x)\n", round, static_cast<int>(w), st);
            all_ok      = false;
            child_alive = false;
            break;
        }

        // 2x commit_timeout + jitter: any lock the stopped child holds is
        // now provably stale to the repairer's grace pass.
        sleep_rand(4000, 6000);

        steals += region.repair_locked_entries();

        kill(pub_pid, SIGCONT);

        if ((round + 1) % 50 == 0)
        {
            std::printf("  round %d/%d: steals so far %" PRIu64 ", received %" PRIu64 "\n",
                        round + 1, rounds, steals,
                        stats.received.load(std::memory_order_relaxed));
        }
    }

    // Stop the child gracefully and fail on signal-death.
    if (child_alive)
    {
        kill(pub_pid, SIGTERM);
        int st = 0;
        waitpid(pub_pid, &st, 0);
        if (not WIFEXITED(st) or WEXITSTATUS(st) != 0)
        {
            std::fprintf(stderr, "  [FAIL] publisher did not exit cleanly (status=0x%x)\n", st);
            all_ok = false;
        }
    }

    // Subscriber drains the residue, then exits.
    g_sub_stop.store(true, std::memory_order_release);
    sub_thread.join();

    uint64_t const received  = stats.received.load(std::memory_order_relaxed);
    uint64_t const corrupted = stats.corrupted.load(std::memory_order_relaxed);
    uint64_t const rewinds   = stats.rewinds.load(std::memory_order_relaxed);

    std::printf("  Subscriber: received %" PRIu64 ", corrupted %" PRIu64
                ", rewinds %" PRIu64 "\n", received, corrupted, rewinds);
    if (corrupted > 0)
    {
        std::fprintf(stderr, "  [FAIL] %" PRIu64 " corrupted samples (torn payload)\n", corrupted);
        all_ok = false;
    }
    if (rewinds > 0)
    {
        std::fprintf(stderr, "  [FAIL] %" PRIu64 " sequence rewinds\n", rewinds);
        all_ok = false;
    }
    if (received == 0)
    {
        std::fprintf(stderr, "  [FAIL] subscriber received nothing\n");
        all_ok = false;
    }

    // Final GC + structural checks (publisher dead, subscriber destroyed).
    steals += region.repair_locked_entries();
    std::size_t const reclaimed = region.reclaim_orphaned_slots();

    // Each steal can orphan at most one slot ref (entry_steal_and_clear
    // deliberately leaks the displaced reference); more reclaims than
    // steals means the normal path leaked.
    std::printf("  Reclaimed slots: %zu (steal budget %" PRIu64 ")\n", reclaimed, steals);
    if (reclaimed > steals)
    {
        std::fprintf(stderr, "  [FAIL] reclaimed %zu slots but only %" PRIu64
                     " steals can account for orphan residue\n", reclaimed, steals);
        all_ok = false;
    }

    if (not verify_rings_free(region))
    {
        all_ok = false;
    }
    if (not verify_slots(region))
    {
        all_ok = false;
    }

    std::printf("Steals observed: %" PRIu64 "\n", steals);
    if (steals == 0)
    {
        std::printf("  [WARN] no steal happened -- the dangerous window was never "
                    "sampled this run (probabilistic; not a failure)\n");
    }

    kickmsg::SharedMemory::unlink(SHM_NAME);

    int pass = 0;
    int fail = 0;
    if (all_ok)
    {
        std::printf("\n  [PASS]\n");
        pass = 1;
    }
    else
    {
        std::printf("\n  [FAIL]\n");
        fail = 1;
    }
    std::printf("=== Summary: %d passed, %d failed ===\n", pass, fail);

    if (all_ok)
    {
        return 0;
    }
    return 1;
}
