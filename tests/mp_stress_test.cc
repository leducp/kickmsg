/// @file mp_stress_test.cc
/// @brief Multi-process steady-state MPMC soak.
///
/// The parent creates the region, then forks 4 publisher processes and
/// 2 subscriber processes.  Each subscriber signals readiness over a pipe
/// AFTER constructing its kickmsg::Subscriber; the parent releases the
/// publishers (go-pipe EOF) only once every ring is Live, so per-subscriber
/// accounting is exact: received + lost (+ corrupt/bad/reorder) == total
/// sent, no late-attach slack.
///
/// Oracles per subscriber: no corruption (magic + checksum), per-publisher
/// strictly increasing seq, exact conservation, received > 0.  Parent-side:
/// every child exits 0 (signal-death is a failure), then the same
/// structural checks as stall_repair_test (free-stack walk, refcounts,
/// rings Free) plus the no-crash GC oracle (repair fixes nothing,
/// reclaims bounded by observed publisher drops).

#include <cerrno>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>

#include "shm_cleanup.h"
#include "kickmsg/os/Time.h"
#include "kickmsg/Publisher.h"
#include "kickmsg/Subscriber.h"

using namespace kickmsg;

static constexpr char const* SHM_NAME = "/kickmsg_mp_stress";

static constexpr int      NUM_PUBS     = 4;
static constexpr int      NUM_SUBS     = 2;
static constexpr uint32_t MSGS_PER_PUB = 100000;
static constexpr uint64_t TOTAL_SENT   = static_cast<uint64_t>(NUM_PUBS) * MSGS_PER_PUB;

struct MpPayload
{
    static constexpr uint32_t MAGIC = 0x4D505353; // 'MPSS'
    uint32_t magic;
    uint32_t pub_id;
    uint32_t seq;
    uint32_t checksum;
};

static uint32_t compute_checksum(MpPayload const& p)
{
    return p.magic ^ p.pub_id ^ p.seq ^ 0xDEADBEEF;
}

/// Per-subscriber tallies shipped to the parent over a pipe.
struct SubReport
{
    uint64_t received;
    uint64_t lost;
    uint64_t corrupted;
    uint64_t bad_pub_id;
    uint64_t reorders;
};

// --- Seeded PRNG (publisher start jitter; KICKMSG_MP_SEED replays a run) ----
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

    uint64_t seed_fuzzer()
    {
        uint64_t seed;
        char const* env = std::getenv("KICKMSG_MP_SEED");
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
}

// --- Pipe helpers (EINTR + short read/write) ---------------------------------

static bool read_all(int fd, void* buf, std::size_t len)
{
    auto* p = static_cast<uint8_t*>(buf);
    while (len > 0)
    {
        ssize_t n = ::read(fd, p, len);
        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return false;
        }
        if (n == 0) // peer closed before sending everything
        {
            return false;
        }
        p   += n;
        len -= static_cast<std::size_t>(n);
    }
    return true;
}

static bool write_all(int fd, void const* buf, std::size_t len)
{
    auto const* p = static_cast<uint8_t const*>(buf);
    while (len > 0)
    {
        ssize_t n = ::write(fd, p, len);
        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return false;
        }
        p   += n;
        len -= static_cast<std::size_t>(n);
    }
    return true;
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

// --- Children -----------------------------------------------------------------

/// Publisher child: opens the region, blocks on the go-pipe (EOF = release),
/// then publishes MSGS_PER_PUB checksummed payloads.
static int child_publisher_main(int pub_id, int go_rfd, uint64_t jitter_us)
{
    auto region = kickmsg::SharedRegion::open(SHM_NAME);
    kickmsg::Publisher pub(region);

    // Wait for the parent's release: read returns 0 once every write end of
    // the go pipe is closed, so all publishers unblock together.
    char b;
    while (::read(go_rfd, &b, 1) < 0 and errno == EINTR)
    {
    }
    ::close(go_rfd);

    kickmsg::sleep(microseconds{static_cast<int64_t>(jitter_us)});

    for (uint32_t seq = 0; seq < MSGS_PER_PUB; ++seq)
    {
        MpPayload msg;
        msg.magic    = MpPayload::MAGIC;
        msg.pub_id   = static_cast<uint32_t>(pub_id);
        msg.seq      = seq;
        msg.checksum = compute_checksum(msg);

        int32_t rc;
        while ((rc = pub.send(&msg, sizeof(msg))) < 0)
        {
            if (rc != -EAGAIN)
            {
                std::fprintf(stderr, "  [FATAL] publisher %d: send() returned %d\n", pub_id, rc);
                return 3;
            }
            kickmsg::yield();
        }
    }
    return 0;
}

/// Subscriber child: constructs its Subscriber, signals readiness, then
/// consumes until every published position is accounted for (received or
/// lost or in an error bucket), and ships the tallies to the parent.
static int child_subscriber_main(int sub_id, int ready_wfd, int report_wfd)
{
    auto region = kickmsg::SharedRegion::open(SHM_NAME);
    kickmsg::Subscriber sub(region);

    char const ready = 1;
    if (not write_all(ready_wfd, &ready, 1))
    {
        return 4;
    }
    ::close(ready_wfd);

    SubReport rep{};
    uint32_t last_seq[NUM_PUBS];
    bool     have_last[NUM_PUBS] = {};

    // Conservation doubles as the stop condition: with the ring Live before
    // the first send, every position ends up in exactly one bucket, so the
    // accounted total reaches TOTAL_SENT iff nothing vanished.  The deadline
    // turns a conservation bug into a parent-side oracle failure instead of
    // a hang.
    auto const deadline = monotonic_ns() + seconds{60};

    while (true)
    {
        uint64_t accounted = rep.received + rep.corrupted + rep.bad_pub_id
                           + rep.reorders + sub.lost();
        if (accounted >= TOTAL_SENT or monotonic_ns() >= deadline)
        {
            break;
        }

        auto sample = sub.receive(100ms);
        if (not sample)
        {
            continue;
        }

        if (sample->len() != sizeof(MpPayload))
        {
            ++rep.corrupted;
            continue;
        }

        MpPayload msg;
        std::memcpy(&msg, sample->data(), sizeof(msg));
        if (msg.magic != MpPayload::MAGIC or msg.checksum != compute_checksum(msg))
        {
            ++rep.corrupted;
            continue;
        }
        if (msg.pub_id >= static_cast<uint32_t>(NUM_PUBS))
        {
            ++rep.bad_pub_id;
            continue;
        }

        if (have_last[msg.pub_id] and msg.seq <= last_seq[msg.pub_id])
        {
            if (rep.reorders < 10)
            {
                std::fprintf(stderr, "  [REORDER] sub%d: pub %u seq %u after %u @pos %" PRIu64 "\n",
                             sub_id, msg.pub_id, msg.seq, last_seq[msg.pub_id],
                             sample->ring_pos());
            }
            ++rep.reorders;
            continue;
        }
        last_seq[msg.pub_id]  = msg.seq;
        have_last[msg.pub_id] = true;
        ++rep.received;
    }

    rep.lost = sub.lost();
    if (not write_all(report_wfd, &rep, sizeof(rep)))
    {
        return 5;
    }
    ::close(report_wfd);
    return 0;
}

// --- Final structural checks (mirrors stall_repair_test) -----------------------

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

// --- Parent -------------------------------------------------------------------

/// Reaps one child; returns false (and reports) on nonzero exit or
/// signal-death.
static bool reap_child(pid_t pid, char const* role, int id)
{
    int st = 0;
    pid_t w = waitpid(pid, &st, 0);
    if (w != pid)
    {
        std::fprintf(stderr, "  [FAIL] waitpid(%s %d) returned %d (errno=%d)\n",
                     role, id, static_cast<int>(w), errno);
        return false;
    }
    if (WIFSIGNALED(st))
    {
        std::fprintf(stderr, "  [FAIL] %s %d killed by signal %d\n", role, id, WTERMSIG(st));
        return false;
    }
    if (not WIFEXITED(st) or WEXITSTATUS(st) != 0)
    {
        std::fprintf(stderr, "  [FAIL] %s %d exited with status 0x%x\n", role, id, st);
        return false;
    }
    return true;
}

int main()
{
    std::printf("=== Kickmsg Multi-Process MPMC Stress Test ===\n");
    kickmsg_test::register_cleanup_shm("/kickmsg_mp_stress");
    kickmsg_test::install_signal_cleanup();

    uint64_t const seed = seed_fuzzer();
    std::printf("mp seed=%llu (set KICKMSG_MP_SEED to replay): %d pubs x %u msgs, %d subs\n",
                static_cast<unsigned long long>(seed), NUM_PUBS, MSGS_PER_PUB, NUM_SUBS);

    kickmsg::SharedMemory::unlink(SHM_NAME);

    kickmsg::channel::Config cfg;
    cfg.max_subscribers   = 4;
    cfg.sub_ring_capacity = 64;
    cfg.pool_size         = 256;
    cfg.max_payload_size  = 64;

    auto region = kickmsg::SharedRegion::create(
        SHM_NAME, kickmsg::channel::PubSub, cfg, "mp_stress_test");

    // Draw publisher start jitters from the parent's PRNG (before forking)
    // so a seed replays the exact schedule.
    uint64_t jitter_us[NUM_PUBS];
    for (int i = 0; i < NUM_PUBS; ++i)
    {
        jitter_us[i] = next_rand() % 2000;
    }

    int ready_pipe[2];
    int go_pipe[2];
    int report_pipe[NUM_SUBS][2];
    bool pipes_ok = (pipe(ready_pipe) == 0) and (pipe(go_pipe) == 0);
    for (int i = 0; i < NUM_SUBS; ++i)
    {
        pipes_ok = pipes_ok and (pipe(report_pipe[i]) == 0);
    }
    if (not pipes_ok)
    {
        std::fprintf(stderr, "pipe() failed: errno=%d\n", errno);
        return 2;
    }

    // Flush before forking: children inherit the stdio buffer and would
    // replay the banner on their own exit.
    std::fflush(nullptr);

    // Fork subscribers first: they must claim their rings (and say so)
    // before any publisher is released.
    pid_t sub_pid[NUM_SUBS];
    for (int i = 0; i < NUM_SUBS; ++i)
    {
        sub_pid[i] = checked_fork("subscriber");
        if (sub_pid[i] == 0)
        {
            ::close(ready_pipe[0]);
            // Close both go-pipe ends: a surviving write-end copy here would
            // defeat the EOF release below.
            ::close(go_pipe[0]);
            ::close(go_pipe[1]);
            for (int j = 0; j < NUM_SUBS; ++j)
            {
                ::close(report_pipe[j][0]);
                if (j != i)
                {
                    ::close(report_pipe[j][1]);
                }
            }
            int rc = child_subscriber_main(i, ready_pipe[1], report_pipe[i][1]);
            std::fflush(nullptr);
            _exit(rc);
        }
    }

    pid_t pub_pid[NUM_PUBS];
    for (int i = 0; i < NUM_PUBS; ++i)
    {
        pub_pid[i] = checked_fork("publisher");
        if (pub_pid[i] == 0)
        {
            ::close(ready_pipe[0]);
            ::close(ready_pipe[1]);
            ::close(go_pipe[1]);
            for (int j = 0; j < NUM_SUBS; ++j)
            {
                ::close(report_pipe[j][0]);
                ::close(report_pipe[j][1]);
            }
            int rc = child_publisher_main(i, go_pipe[0], jitter_us[i]);
            std::fflush(nullptr);
            _exit(rc);
        }
    }

    ::close(ready_pipe[1]);
    ::close(go_pipe[0]);
    for (int i = 0; i < NUM_SUBS; ++i)
    {
        ::close(report_pipe[i][1]);
    }

    // Readiness gate: one byte per subscriber, then release the publishers
    // by closing the last write end of the go pipe.
    char ready_buf[NUM_SUBS];
    if (not read_all(ready_pipe[0], ready_buf, sizeof(ready_buf)))
    {
        std::fprintf(stderr, "  [FAIL] subscribers died before signaling readiness\n");
        return 2;
    }
    ::close(ready_pipe[0]);
    std::printf("subscribers ready, releasing publishers\n");
    std::fflush(stdout);

    nanoseconds const t0 = kickmsg::monotonic_ns();
    ::close(go_pipe[1]);

    bool all_ok = true;

    for (int i = 0; i < NUM_PUBS; ++i)
    {
        all_ok &= reap_child(pub_pid[i], "publisher", i);
    }
    std::printf("publishers done (%lld ms), draining subscribers\n",
                static_cast<long long>(
                    duration_cast<milliseconds>(kickmsg::elapsed_time(t0)).count()));
    std::fflush(stdout);

    SubReport reports[NUM_SUBS] = {};
    for (int i = 0; i < NUM_SUBS; ++i)
    {
        if (not read_all(report_pipe[i][0], &reports[i], sizeof(reports[i])))
        {
            std::fprintf(stderr, "  [FAIL] subscriber %d report read failed (child died?)\n", i);
            all_ok = false;
        }
        ::close(report_pipe[i][0]);
        all_ok &= reap_child(sub_pid[i], "subscriber", i);
    }

    std::printf("  %-6s %10s %10s %10s %10s %10s\n",
                "sub", "received", "lost", "corrupt", "bad_pid", "reorder");
    for (int i = 0; i < NUM_SUBS; ++i)
    {
        SubReport const& r = reports[i];
        std::printf("  sub%-3d %10" PRIu64 " %10" PRIu64 " %10" PRIu64
                    " %10" PRIu64 " %10" PRIu64 "\n",
                    i, r.received, r.lost, r.corrupted, r.bad_pub_id, r.reorders);

        if (r.corrupted > 0)
        {
            std::fprintf(stderr, "  [FAIL] sub%d: %" PRIu64 " corrupted messages\n", i, r.corrupted);
            all_ok = false;
        }
        if (r.bad_pub_id > 0)
        {
            std::fprintf(stderr, "  [FAIL] sub%d: %" PRIu64 " bad publisher IDs\n", i, r.bad_pub_id);
            all_ok = false;
        }
        if (r.reorders > 0)
        {
            std::fprintf(stderr, "  [FAIL] sub%d: %" PRIu64 " reordered messages\n", i, r.reorders);
            all_ok = false;
        }
        if (r.received == 0)
        {
            std::fprintf(stderr, "  [FAIL] sub%d: received 0 messages\n", i);
            all_ok = false;
        }
        uint64_t accounted = r.received + r.lost + r.corrupted + r.bad_pub_id + r.reorders;
        if (accounted != TOTAL_SENT)
        {
            std::fprintf(stderr, "  [FAIL] sub%d: accounted %" PRIu64 " != total sent %" PRIu64
                         " (messages vanished or duplicated)\n", i, accounted, TOTAL_SENT);
            all_ok = false;
        }
    }

    // No-crash GC oracle: every child exited cleanly, so repair must find
    // nothing and reclaims are bounded by observed publisher drops (a
    // descheduled publisher whose lock got stolen books a drop; the steal
    // leaks one slot ref that only reclaim recovers).
    uint64_t dropped = 0;
    for (uint32_t i = 0; i < cfg.max_subscribers; ++i)
    {
        auto* ring = kickmsg::sub_ring_at(region.base(), region.header(), i);
        dropped += ring->dropped_count.load(std::memory_order_acquire);
    }
    std::size_t repaired = region.repair_locked_entries();
    if (repaired != 0)
    {
        std::fprintf(stderr, "  [FAIL] repair_locked_entries fixed %zu entries on a no-crash run\n",
                     repaired);
        all_ok = false;
    }
    std::size_t reclaimed = region.reclaim_orphaned_slots();
    if (reclaimed > dropped)
    {
        std::fprintf(stderr, "  [FAIL] reclaimed %zu slots but only %" PRIu64
                     " publisher drops can account for steal residue\n", reclaimed, dropped);
        all_ok = false;
    }
    else if (reclaimed != 0)
    {
        std::printf("  [NOTE] %zu slot(s) reclaimed, within the %" PRIu64 "-drop steal budget\n",
                    reclaimed, dropped);
    }

    if (not verify_rings_free(region))
    {
        all_ok = false;
    }
    if (not verify_slots(region))
    {
        all_ok = false;
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
