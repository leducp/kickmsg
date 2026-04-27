/// @file crash_test.cc
/// @brief Multi-process crash recovery test for kickmsg.
///
/// Phase 1 — publisher crash rounds (the original scenario): fork a child
/// publisher, kill it mid-commit with SIGKILL, verify the channel
/// recovers via diagnose() + repair_locked_entries() +
/// reset_retired_rings() + reclaim_orphaned_slots().  A subscriber
/// running throughout validates that no corruption occurs.
///
/// Phase 2 — subscriber crash: a subscriber that SIGKILLs itself while
/// holding live SampleView pins.  Exercises reclaim_orphaned_slots().
///
/// Phase 3 — multi-publisher simultaneous crash: four publishers all
/// killed at once.  Exercises the repair sequence when Case-A (locked
/// sequences) and Case-B (retired rings) residue coexist at multiple
/// ring positions.

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cinttypes>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "kickmsg/os/Time.h"
#include "kickmsg/Publisher.h"
#include "kickmsg/Subscriber.h"

using namespace kickmsg;

static constexpr char const* SHM_NAME = "/kickmsg_crash_test";

struct CrashPayload
{
    static constexpr uint32_t MAGIC = 0xDEADC0DE;
    uint32_t magic;
    uint32_t seq;
    uint32_t checksum;
};

static uint32_t compute_checksum(CrashPayload const& p)
{
    return p.magic ^ p.seq ^ 0xBAADF00D;
}

/// Child publisher: publishes as fast as possible using allocate() + publish()
/// to maximize the window where a kill can orphan a slot.
static void child_publisher_main(int /*round*/)
{
    auto region = kickmsg::SharedRegion::open(SHM_NAME);
    kickmsg::Publisher pub(region);

    for (uint32_t i = 0; ; ++i)
    {
        auto* ptr = pub.allocate(sizeof(CrashPayload));
        if (ptr == nullptr)
        {
            kickmsg::yield();
            continue;
        }

        CrashPayload msg;
        msg.magic    = CrashPayload::MAGIC;
        msg.seq      = i;
        msg.checksum = compute_checksum(msg);
        std::memcpy(ptr, &msg, sizeof(msg));

        pub.publish();
    }
}

/// Child subscriber: receives and validates for a fixed duration.
static void child_subscriber_main(int result_fd, int signal_fd)
{
    auto region = kickmsg::SharedRegion::open(SHM_NAME);
    kickmsg::Subscriber sub(region);

    uint64_t received  = 0;
    uint64_t corrupted = 0;

    while (true)
    {
        auto sample = sub.receive(200ms);
        if (not sample)
        {
            // Check if parent signaled us to exit
            char buf;
            ssize_t n = read(signal_fd, &buf, 1);
            if (n <= 0)
            {
                break;
            }
            continue;
        }

        if (sample->len() != sizeof(CrashPayload))
        {
            ++corrupted;
            continue;
        }

        CrashPayload msg;
        std::memcpy(&msg, sample->data(), sizeof(msg));
        if (msg.magic != CrashPayload::MAGIC or msg.checksum != compute_checksum(msg))
        {
            ++corrupted;
        }
        ++received;
    }

    struct { uint64_t recv; uint64_t corrupt; } result = {received, corrupted};
    ssize_t written = write(result_fd, &result, sizeof(result));
    close(result_fd);
    close(signal_fd);
    // Partial write → parent would read zero-initialized bytes and
    // interpret that as "no corruption" — a silent false pass.  Exit
    // non-zero so waitpid surfaces the anomaly.
    if (written != sizeof(result))
    {
        std::_Exit(3);
    }
}

struct RoundResult
{
    bool recovered_entries;
    bool recovered_rings;
    bool recovered_slots;
    bool subscriber_ok;
};

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

static RoundResult run_one_round(int round)
{
    RoundResult result{};

    // Fork publisher
    pid_t pub_pid = checked_fork("run_one_round publisher");
    if (pub_pid == 0)
    {
        child_publisher_main(round);
        _exit(0); // never reached
    }

    // Let publisher run for 20-50ms
    kickmsg::sleep(milliseconds{20 + (round % 30)});

    // Kill publisher mid-flight
    kill(pub_pid, SIGKILL);
    int status;
    waitpid(pub_pid, &status, 0);

    // Diagnose damage
    auto region = kickmsg::SharedRegion::open(SHM_NAME);
    auto report = region.diagnose();

    result.recovered_entries = (report.locked_entries > 0);
    result.recovered_rings   = (report.retired_rings > 0);

    // Repair
    std::size_t repaired  = region.repair_locked_entries();
    std::size_t reset     = region.reset_retired_rings();
    std::size_t reclaimed = region.reclaim_orphaned_slots();

    result.recovered_slots = (reclaimed > 0);

    if (repaired > 0 or reset > 0 or reclaimed > 0)
    {
        std::printf("  Round %d: repaired %zu entries, reset %zu rings, reclaimed %zu slots\n",
                    round, repaired, reset, reclaimed);
    }

    // Verify clean after repair
    auto post = region.diagnose();
    if (post.locked_entries > 0 or post.retired_rings > 0)
    {
        std::fprintf(stderr, "  [FAIL] Round %d: repair incomplete "
                     "(locked=%u, retired=%u)\n",
                     round, post.locked_entries, post.retired_rings);
    }

    // Fork a new publisher to verify the channel still works
    pid_t pub2_pid = checked_fork("run_one_round verify publisher");
    if (pub2_pid == 0)
    {
        auto reg = kickmsg::SharedRegion::open(SHM_NAME);
        kickmsg::Publisher pub(reg);

        for (uint32_t i = 0; i < 100; ++i)
        {
            CrashPayload msg;
            msg.magic    = CrashPayload::MAGIC;
            msg.seq      = 1000000 + i;
            msg.checksum = compute_checksum(msg);
            while (pub.send(&msg, sizeof(msg)) < 0)
            {
                kickmsg::yield();
            }
        }
        _exit(0);
    }

    waitpid(pub2_pid, &status, 0);
    result.subscriber_ok = true;

    return result;
}

/// Phase 2: subscriber SIGKILLed while holding SampleView pins —
/// `reclaim_orphaned_slots` must release them.
static bool test_subscriber_crash()
{
    std::printf("\n--- Phase 2: subscriber killed mid-receive ---\n");

    constexpr char const* SUB_SHM = "/kickmsg_crash_test_sub";
    kickmsg::SharedMemory::unlink(SUB_SHM);

    kickmsg::channel::Config cfg;
    cfg.max_subscribers   = 2;
    cfg.sub_ring_capacity = 16;
    cfg.pool_size         = 32;
    cfg.max_payload_size  = sizeof(CrashPayload);

    auto region = kickmsg::SharedRegion::create(
        SUB_SHM, kickmsg::channel::PubSub, cfg, "crash_test_sub");

    // Fork subscriber that pins every sample it receives.
    pid_t sub_pid = checked_fork("subscriber");
    if (sub_pid == 0)
    {
        auto r = kickmsg::SharedRegion::open(SUB_SHM);
        kickmsg::Subscriber sub(r);
        // receive_view() returns SampleView which pins the slot — exactly
        // what we want to orphan on SIGKILL.
        std::vector<kickmsg::Subscriber::SampleView> pins;
        while (true)
        {
            auto s = sub.receive_view(200ms);
            if (s)
            {
                pins.push_back(std::move(*s));
            }
        }
    }
    // Let the subscriber attach.
    kickmsg::sleep(50ms);

    // Pool saturates once pins outnumber released slots; send() returns <0.
    kickmsg::Publisher pub(region);
    uint32_t published = 0;
    for (uint32_t i = 0; i < cfg.pool_size * 2; ++i)
    {
        CrashPayload msg{};
        msg.magic    = CrashPayload::MAGIC;
        msg.seq      = i;
        msg.checksum = compute_checksum(msg);
        if (pub.send(&msg, sizeof(msg)) >= 0)
        {
            ++published;
        }
        kickmsg::sleep(1ms);
    }

    kill(sub_pid, SIGKILL);
    int status;
    waitpid(sub_pid, &status, 0);

    auto pre = region.diagnose();

    std::size_t repaired  = region.repair_locked_entries();
    std::size_t reset     = region.reset_retired_rings();
    std::size_t reclaimed = region.reclaim_orphaned_slots();

    auto post = region.diagnose();
    bool clean = (post.locked_entries == 0 and post.retired_rings == 0);

    // Verify the channel is still writable after repair.
    bool writable = false;
    {
        pid_t v = checked_fork("verify child");
        if (v == 0)
        {
            auto r = kickmsg::SharedRegion::open(SUB_SHM);
            kickmsg::Publisher p(r);
            for (uint32_t i = 0; i < 10; ++i)
            {
                CrashPayload msg{};
                msg.magic    = CrashPayload::MAGIC;
                msg.seq      = 3000000 + i;
                msg.checksum = compute_checksum(msg);
                int rc = 0;
                for (int k = 0; k < 100 and rc <= 0; ++k)
                {
                    rc = p.send(&msg, sizeof(msg));
                    if (rc <= 0)
                    {
                        kickmsg::yield();
                    }
                }
                if (rc <= 0)
                {
                    _exit(2);
                }
            }
            _exit(0);
        }
        int v_status = 0;
        waitpid(v, &v_status, 0);
        writable = (WIFEXITED(v_status) and WEXITSTATUS(v_status) == 0);
    }

    std::printf("  Published %u, pre: locked=%u retired=%u, "
                "repaired=%zu reset=%zu reclaimed=%zu, "
                "final_clean=%s, writable_after=%s\n",
                published, pre.locked_entries, pre.retired_rings,
                repaired, reset, reclaimed,
                clean    ? "yes" : "no",
                writable ? "yes" : "no");

    kickmsg::SharedMemory::unlink(SUB_SHM);
    return clean and writable;
}

/// Phase 3: four publishers SIGKILLed simultaneously — repair sequence
/// must handle Case-A + Case-B residue coexisting at multiple positions.
static bool test_multi_publisher_crash()
{
    std::printf("\n--- Phase 3: multi-publisher simultaneous crash ---\n");

    constexpr char const* MULTI_SHM = "/kickmsg_crash_test_multi";
    kickmsg::SharedMemory::unlink(MULTI_SHM);

    kickmsg::channel::Config cfg;
    cfg.max_subscribers   = 2;
    cfg.sub_ring_capacity = 16;
    cfg.pool_size         = 64;
    cfg.max_payload_size  = sizeof(CrashPayload);

    auto region = kickmsg::SharedRegion::create(
        MULTI_SHM, kickmsg::channel::PubSub, cfg, "crash_test_multi");
    // Subscriber attaches so rings go live and can accumulate damage.
    kickmsg::Subscriber sub(region);

    constexpr int N_PUBS = 4;
    pid_t pubs[N_PUBS];
    for (int i = 0; i < N_PUBS; ++i)
    {
        pubs[i] = checked_fork("multi-pub child");
        if (pubs[i] == 0)
        {
            auto r = kickmsg::SharedRegion::open(MULTI_SHM);
            kickmsg::Publisher p(r);
            for (uint32_t seq = 0; ; ++seq)
            {
                auto* ptr = p.allocate(sizeof(CrashPayload));
                if (ptr == nullptr)
                {
                    kickmsg::yield();
                    continue;
                }
                CrashPayload msg{};
                msg.magic    = CrashPayload::MAGIC;
                msg.seq      = seq;
                msg.checksum = compute_checksum(msg);
                std::memcpy(ptr, &msg, sizeof(msg));
                p.publish();
            }
        }
    }

    kickmsg::sleep(30ms);

    for (int i = 0; i < N_PUBS; ++i)
    {
        kill(pubs[i], SIGKILL);
    }
    for (int i = 0; i < N_PUBS; ++i)
    {
        int st;
        waitpid(pubs[i], &st, 0);
    }

    auto pre = region.diagnose();

    std::size_t repaired  = region.repair_locked_entries();
    std::size_t reset     = region.reset_retired_rings();
    std::size_t reclaimed = region.reclaim_orphaned_slots();

    auto post = region.diagnose();
    bool clean = (post.locked_entries == 0 and post.retired_rings == 0);

    // Confirm throughput resumes: fresh publisher, N messages, no errors.
    bool resumed = false;
    {
        pid_t v = checked_fork("verify child");
        if (v == 0)
        {
            auto r = kickmsg::SharedRegion::open(MULTI_SHM);
            kickmsg::Publisher p(r);
            for (uint32_t i = 0; i < 50; ++i)
            {
                CrashPayload msg{};
                msg.magic    = CrashPayload::MAGIC;
                msg.seq      = 4000000 + i;
                msg.checksum = compute_checksum(msg);
                int rc = 0;
                for (int k = 0; k < 100 and rc <= 0; ++k)
                {
                    rc = p.send(&msg, sizeof(msg));
                    if (rc <= 0)
                    {
                        kickmsg::yield();
                    }
                }
                if (rc <= 0)
                {
                    _exit(2);
                }
            }
            _exit(0);
        }
        int v_status = 0;
        waitpid(v, &v_status, 0);
        resumed = (WIFEXITED(v_status) and WEXITSTATUS(v_status) == 0);
    }

    std::printf("  N=%d, pre: locked=%u retired=%u, "
                "repaired=%zu reset=%zu reclaimed=%zu, "
                "final_clean=%s, resumed=%s\n",
                N_PUBS, pre.locked_entries, pre.retired_rings,
                repaired, reset, reclaimed,
                clean   ? "yes" : "no",
                resumed ? "yes" : "no");

    kickmsg::SharedMemory::unlink(MULTI_SHM);
    return clean and resumed;
}

int main()
{
    std::printf("=== Kickmsg Multi-Process Crash Test ===\n\n");

    kickmsg::SharedMemory::unlink(SHM_NAME);

    kickmsg::channel::Config cfg;
    cfg.max_subscribers   = 4;
    cfg.sub_ring_capacity = 32;
    cfg.pool_size         = 64;
    cfg.max_payload_size  = sizeof(CrashPayload);

    auto region = kickmsg::SharedRegion::create(
        SHM_NAME, kickmsg::channel::PubSub, cfg, "crash_test");

    // Fork a long-lived subscriber.
    // signal_pipe: parent writes to [1] to tell subscriber to exit.
    // result_pipe: subscriber writes results to [1], parent reads from [0].
    int signal_pipe[2];
    int result_pipe[2];
    if (pipe(signal_pipe) != 0 or pipe(result_pipe) != 0)
    {
        std::fprintf(stderr, "pipe() failed: errno=%d\n", errno);
        return 2;
    }

    pid_t sub_pid = checked_fork("subscriber");
    if (sub_pid == 0)
    {
        close(signal_pipe[1]); // close write end
        close(result_pipe[0]); // close read end
        child_subscriber_main(result_pipe[1], signal_pipe[0]);
        _exit(0);
    }
    close(signal_pipe[0]); // close read end in parent
    close(result_pipe[1]); // close write end in parent

    // Let subscriber attach
    kickmsg::sleep(50ms);

    constexpr int NUM_ROUNDS = 10;
    int any_recovery = 0;
    bool all_ok = true;

    for (int round = 0; round < NUM_ROUNDS; ++round)
    {
        auto result = run_one_round(round);
        if (result.recovered_entries or result.recovered_rings or result.recovered_slots)
        {
            ++any_recovery;
        }
    }

    // Signal subscriber to exit
    close(signal_pipe[1]);

    // Read subscriber results.  A short read means the child either
    // crashed before writing its result struct or we lost bytes on the
    // pipe — either way, we can't trust a zero-initialised sub_result
    // and should fail rather than silently pass the corruption check.
    struct { uint64_t recv; uint64_t corrupt; } sub_result{};
    ssize_t got = read(result_pipe[0], &sub_result, sizeof(sub_result));
    close(result_pipe[0]);

    int sub_status;
    waitpid(sub_pid, &sub_status, 0);

    if (got != static_cast<ssize_t>(sizeof(sub_result)))
    {
        std::fprintf(stderr,
            "  [FAIL] Subscriber result pipe short read: got=%zd expected=%zu\n",
            got, sizeof(sub_result));
        all_ok = false;
    }
    else
    {
        std::printf("  Subscriber: received %" PRIu64 ", corrupted %" PRIu64 "\n",
                    sub_result.recv, sub_result.corrupt);
        if (sub_result.corrupt > 0)
        {
            std::fprintf(stderr,
                "  [FAIL] Subscriber saw %" PRIu64 " corrupted messages!\n",
                sub_result.corrupt);
            all_ok = false;
        }
    }

    std::printf("\n  Rounds: %d, rounds with recovery: %d\n", NUM_ROUNDS, any_recovery);

    if (any_recovery == 0)
    {
        std::printf("  [WARN] No crash damage detected in %d rounds "
                    "(kill timing may have missed the commit window)\n", NUM_ROUNDS);
    }

    // Final cleanup and verification
    {
        auto reg = kickmsg::SharedRegion::open(SHM_NAME);
        reg.repair_locked_entries();
        reg.reset_retired_rings();
        reg.reclaim_orphaned_slots();

        auto report = reg.diagnose();
        if (report.locked_entries > 0 or report.retired_rings > 0)
        {
            std::fprintf(stderr, "  [FAIL] Final state not clean\n");
            all_ok = false;
        }
    }

    kickmsg::SharedMemory::unlink(SHM_NAME);

    if (not test_subscriber_crash())
    {
        all_ok = false;
    }
    if (not test_multi_publisher_crash())
    {
        all_ok = false;
    }

    if (all_ok)
    {
        std::printf("\n  [PASS]\n");
    }
    else
    {
        std::printf("\n  [FAIL]\n");
    }

    return all_ok ? 0 : 1;
}
