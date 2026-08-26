/// @file blackboard_crash_test.cc
/// @brief Multi-process crash recovery test for the kickmsg blackboard.
///
/// Phase 1 -- value survives the owner's death.  A forked child declares a
/// key and rewrites a large checksummed payload in a tight loop; the parent
/// SIGKILLs it at a random instant, almost always inside a memcpy.  Because
/// the writer always targets the cell readers are NOT on, the parent must
/// still read a complete, checksum-valid value.
///
/// Phase 2 -- takeover.  Without sweeping, a second process re-declares the
/// dead owner's key and the prior value is intact until its first write.
///
/// Phase 3 -- declare race.  N children race to declare one key; exactly one
/// wins and the rest see a live owner.
///
/// Phase 4 -- no leak across repeated crash cycles.  More crash/sweep rounds
/// than the board has capacity, ending with an empty board.
///
/// Phase 5 -- unreaped owner.  A SIGKILLed child that is never waited for is
/// a zombie: it answers kill(pid, 0) and keeps its start time, and must still
/// read as dead.
///
/// Phase 6 -- fork inheritance.  A Writer inherited by a forked child neither
/// writes to nor releases the parent's key.
///
/// Exits 0 on success, non-zero on any assertion failure.

#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

#include "shm_cleanup.h"
#include "kickmsg/Blackboard.h"
#include "kickmsg/os/Process.h"
#include "kickmsg/os/Time.h"

using namespace kickmsg;

namespace
{
    constexpr char const* NS   = "kickmsg_bbcrash";
    constexpr char const* NAME = "board";
    constexpr char const* KEY  = "arm/state";

    std::string g_shm_name;

    struct BbPayload
    {
        static constexpr uint32_t MAGIC = 0xB1ACB0AD;
        uint32_t magic;
        uint32_t seq;
        uint32_t checksum;
        uint32_t reserved;
        uint8_t  filler[4000];
    };

    uint32_t compute_checksum(BbPayload const& p)
    {
        uint32_t sum = p.magic ^ p.seq ^ 0xBAADF00Du;
        for (std::size_t i = 0; i < sizeof(p.filler); ++i)
        {
            sum = (sum << 1) ^ (sum >> 31) ^ p.filler[i];
        }
        return sum;
    }

    void fill_payload(BbPayload& p, uint32_t seq)
    {
        p.magic    = BbPayload::MAGIC;
        p.seq      = seq;
        p.reserved = 0;
        for (std::size_t i = 0; i < sizeof(p.filler); ++i)
        {
            p.filler[i] = static_cast<uint8_t>(seq + i);
        }
        p.checksum = compute_checksum(p);
    }

    bool payload_valid(BbPayload const& p)
    {
        return p.magic == BbPayload::MAGIC and p.checksum == compute_checksum(p);
    }

    blackboard::Config cfg()
    {
        blackboard::Config c;
        c.capacity       = 8;
        c.max_value_size = sizeof(BbPayload);
        return c;
    }

    uint64_t g_rng_state = 0;

    uint64_t next_rand() // splitmix64
    {
        g_rng_state += 0x9E3779B97F4A7C15ull;
        uint64_t z = g_rng_state;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }

    void sleep_rand(uint64_t lo_us, uint64_t hi_us)
    {
        uint64_t span = hi_us - lo_us + 1;
        kickmsg::sleep(microseconds{static_cast<int64_t>(lo_us + next_rand() % span)});
    }

    uint64_t seed_fuzzer()
    {
        uint64_t seed = 0;
        char const* env = std::getenv("KICKMSG_BB_SEED");
        if (env != nullptr)
        {
            seed = std::strtoull(env, nullptr, 0);
        }
        else
        {
            seed = static_cast<uint64_t>(::getpid())
                 ^ static_cast<uint64_t>(kickmsg::monotonic_ns().count());
        }
        g_rng_state = seed;
        return seed;
    }

    char const* verdict(bool ok)
    {
        if (ok)
        {
            return "[ok]";
        }
        return "[FAIL]";
    }

    pid_t checked_fork()
    {
        pid_t pid = ::fork();
        if (pid < 0)
        {
            std::fprintf(stderr, "  [FAIL] fork failed\n");
            std::exit(1);
        }
        return pid;
    }

    /// Fork a child that owns `key` and rewrites it until killed.  Never
    /// returns in the child.
    pid_t spawn_writer(char const* key)
    {
        pid_t pid = checked_fork();
        if (pid == 0)
        {
            auto bb = Blackboard::open_or_create(NS, NAME, cfg());
            auto w  = bb.declare(key, "crash_child");
            BbPayload payload{};
            for (uint32_t seq = 1;; ++seq)
            {
                fill_payload(payload, seq);
                w.write(payload);
            }
        }
        return pid;
    }

    /// Block until the child has declared its key and landed a first value.
    /// Without this the random kill delay can fire before the child even
    /// reaches declare(), and the round tests nothing.
    bool wait_for_value(Blackboard& bb, char const* key)
    {
        auto      r = bb.observe(key);
        BbPayload got{};
        for (int i = 0; i < 5000; ++i)
        {
            if (r.read(got).status == blackboard::Ok)
            {
                return true;
            }
            kickmsg::sleep(1ms);
        }
        std::fprintf(stderr, "  [FAIL] writer never published %s\n", key);
        return false;
    }

    /// SIGKILL and reap.  Phase 5 covers the unreaped case.
    void kill_and_reap(pid_t pid)
    {
        ::kill(pid, SIGKILL);
        int status = 0;
        ::waitpid(pid, &status, 0);
    }
}

static bool test_value_survives_owner_death()
{
    constexpr int ROUNDS = 12;
    std::printf("\nPhase 1: value survives the owner's death (%d rounds)\n", ROUNDS);

    bool ok = true;
    for (int round = 0; round < ROUNDS; ++round)
    {
        Blackboard::unlink(NS, NAME);
        auto bb = Blackboard::open_or_create(NS, NAME, cfg());
        auto r  = bb.observe(KEY);

        pid_t child = spawn_writer(KEY);
        if (not wait_for_value(bb, KEY))
        {
            kill_and_reap(child);
            return false;
        }
        sleep_rand(500, 20000);
        kill_and_reap(child);

        // Read repeatedly: every read must be complete and checksum-valid.
        uint32_t last_seq = 0;
        for (int i = 0; i < 200; ++i)
        {
            BbPayload got{};
            auto out = r.read(got);
            if (out.status != blackboard::Ok)
            {
                std::fprintf(stderr,
                    "  [FAIL] round %d: read status %u after owner death\n",
                    round, static_cast<unsigned>(out.status));
                ok = false;
                break;
            }
            if (not payload_valid(got))
            {
                std::fprintf(stderr,
                    "  [FAIL] round %d: torn value survived the owner's death "
                    "(seq %u)\n", round, got.seq);
                ok = false;
                break;
            }
            if (got.seq < last_seq)
            {
                std::fprintf(stderr,
                    "  [FAIL] round %d: value went backwards (%u -> %u)\n",
                    round, last_seq, got.seq);
                ok = false;
                break;
            }
            last_seq = got.seq;
        }

        if (r.owner_alive())
        {
            std::fprintf(stderr, "  [FAIL] round %d: dead owner reported alive\n", round);
            ok = false;
        }

        auto snap = bb.snapshot();
        if (snap.size() != 1 or snap[0].owner_alive)
        {
            std::fprintf(stderr, "  [FAIL] round %d: snapshot did not report a dead owner\n",
                         round);
            ok = false;
        }

        uint32_t freed = bb.sweep_stale();
        if (freed != 1)
        {
            std::fprintf(stderr, "  [FAIL] round %d: sweep_stale freed %u, expected 1\n",
                         round, freed);
            ok = false;
        }
        BbPayload got{};
        if (r.read(got).status != blackboard::Missing)
        {
            std::fprintf(stderr, "  [FAIL] round %d: key readable after sweep\n", round);
            ok = false;
        }
    }

    Blackboard::unlink(NS, NAME);
    std::printf("  %s\n", verdict(ok));
    return ok;
}

static bool test_takeover_after_owner_death()
{
    std::printf("\nPhase 2: takeover preserves the dead owner's value\n");
    Blackboard::unlink(NS, NAME);

    bool ok = true;
    auto bb = Blackboard::open_or_create(NS, NAME, cfg());
    auto r  = bb.observe(KEY);

    pid_t child = spawn_writer(KEY);
    if (not wait_for_value(bb, KEY))
    {
        kill_and_reap(child);
        return false;
    }
    sleep_rand(2000, 15000);
    kill_and_reap(child);

    BbPayload before{};
    auto out = r.read(before);
    if (out.status != blackboard::Ok or not payload_valid(before))
    {
        std::fprintf(stderr, "  [FAIL] no valid value after owner death\n");
        ok = false;
    }
    uint64_t count_before = out.update_count;

    // No sweep: the restart path is a plain re-declare.
    Blackboard::Writer w2;
    try
    {
        w2 = bb.declare(KEY, "restarted");
    }
    catch (std::exception const& e)
    {
        std::fprintf(stderr, "  [FAIL] takeover threw: %s\n", e.what());
        return false;
    }

    BbPayload after{};
    out = r.read(after);
    if (out.status != blackboard::Ok or after.seq != before.seq
        or out.update_count != count_before)
    {
        std::fprintf(stderr, "  [FAIL] takeover did not preserve the prior value\n");
        ok = false;
    }

    // The publish counter continues rather than rewinding.
    BbPayload fresh{};
    fill_payload(fresh, 0xABCD);
    if (not w2.write(fresh))
    {
        std::fprintf(stderr, "  [FAIL] write after takeover failed\n");
        ok = false;
    }
    out = r.read(after);
    if (out.status != blackboard::Ok or after.seq != 0xABCD
        or out.update_count != count_before + 1)
    {
        std::fprintf(stderr, "  [FAIL] counter did not continue after takeover "
                             "(%llu -> %llu)\n",
                     static_cast<unsigned long long>(count_before),
                     static_cast<unsigned long long>(out.update_count));
        ok = false;
    }

    w2.release();
    Blackboard::unlink(NS, NAME);
    std::printf("  %s\n", verdict(ok));
    return ok;
}

static bool test_declare_race_across_processes()
{
    constexpr int CHILDREN = 8;
    std::printf("\nPhase 3: %d processes race to declare one key\n", CHILDREN);
    Blackboard::unlink(NS, NAME);

    auto bb = Blackboard::open_or_create(NS, NAME, cfg());

    pid_t pids[CHILDREN];
    for (int i = 0; i < CHILDREN; ++i)
    {
        pids[i] = checked_fork();
        if (pids[i] == 0)
        {
            auto child_bb = Blackboard::open_or_create(NS, NAME, cfg());
            int  code     = 1;
            try
            {
                auto w = child_bb.declare(KEY, "racer");
                code = 0;
                // Hold the claim until the parent reaps everyone, so a loser
                // can never win by outliving the winner's release.
                kickmsg::sleep(300ms);
                w.release();
            }
            catch (std::exception const&)
            {
                code = 1;
            }
            ::_exit(code);
        }
    }

    int winners = 0;
    for (int i = 0; i < CHILDREN; ++i)
    {
        int status = 0;
        ::waitpid(pids[i], &status, 0);
        if (WIFEXITED(status) and WEXITSTATUS(status) == 0)
        {
            ++winners;
        }
    }

    bool ok = winners == 1;
    if (not ok)
    {
        std::fprintf(stderr, "  [FAIL] %d winners, expected exactly 1\n", winners);
    }
    Blackboard::unlink(NS, NAME);
    std::printf("  %s\n", verdict(ok));
    return ok;
}

static bool test_no_leak_after_repeated_crash_cycles()
{
    constexpr int ROUNDS = 20;   // more than the board's capacity of 8
    std::printf("\nPhase 4: %d crash/sweep cycles on an 8-key board\n", ROUNDS);
    Blackboard::unlink(NS, NAME);

    bool ok = true;
    auto bb = Blackboard::open_or_create(NS, NAME, cfg());

    for (int round = 0; round < ROUNDS; ++round)
    {
        std::string key = "cycle/" + std::to_string(round);
        pid_t child = spawn_writer(key.c_str());
        if (not wait_for_value(bb, key.c_str()))
        {
            kill_and_reap(child);
            return false;
        }
        sleep_rand(500, 4000);
        kill_and_reap(child);

        if (bb.sweep_stale() != 1)
        {
            std::fprintf(stderr, "  [FAIL] round %d: sweep did not free the key\n", round);
            ok = false;
            break;
        }
    }

    auto snap = bb.snapshot();
    if (not snap.empty())
    {
        std::fprintf(stderr, "  [FAIL] board not empty after %d cycles (%zu keys)\n",
                     ROUNDS, snap.size());
        ok = false;
    }

    // Full capacity must still be claimable.
    std::vector<Blackboard::Writer> held;
    try
    {
        for (uint32_t i = 0; i < bb.capacity(); ++i)
        {
            held.push_back(bb.declare(("final/" + std::to_string(i)).c_str()));
        }
    }
    catch (std::exception const& e)
    {
        std::fprintf(stderr, "  [FAIL] capacity not fully reclaimed: %s\n", e.what());
        ok = false;
    }

    held.clear();
    Blackboard::unlink(NS, NAME);
    std::printf("  %s\n", verdict(ok));
    return ok;
}

static bool test_unreaped_owner_is_reclaimable()
{
    std::printf("\nPhase 5: an unreaped (zombie) owner is not alive\n");
    Blackboard::unlink(NS, NAME);

    bool ok = true;
    auto bb = Blackboard::open_or_create(NS, NAME, cfg());

    pid_t child = spawn_writer(KEY);
    if (not wait_for_value(bb, KEY))
    {
        kill_and_reap(child);
        return false;
    }
    // Killed but deliberately NOT reaped: the child is a zombie, which still
    // answers kill(pid, 0) and still reports the start time it was declared
    // with.  Nothing may treat that as a live owner.
    ::kill(child, SIGKILL);
    for (int i = 0; i < 5000 and not owner_is_dead(child, process_starttime(child)); ++i)
    {
        kickmsg::sleep(1ms);
    }

    if (not owner_is_dead(child, process_starttime(child)))
    {
        std::fprintf(stderr, "  [FAIL] a zombie owner still reads as alive\n");
        ok = false;
    }

    auto snap = bb.snapshot();
    if (snap.size() != 1 or snap[0].owner_alive)
    {
        std::fprintf(stderr, "  [FAIL] snapshot reports the zombie owner alive\n");
        ok = false;
    }

    // The takeover path must work while the corpse is still unreaped.
    try
    {
        auto w = bb.declare(KEY, "restarted");
        BbPayload fresh{};
        fill_payload(fresh, 7);
        if (not w.write(fresh))
        {
            std::fprintf(stderr, "  [FAIL] write after zombie takeover failed\n");
            ok = false;
        }
        w.release();
    }
    catch (std::exception const& e)
    {
        std::fprintf(stderr, "  [FAIL] takeover from a zombie owner threw: %s\n", e.what());
        ok = false;
    }

    if (bb.sweep_stale() != 0)
    {
        std::fprintf(stderr, "  [FAIL] sweep freed a key owned by nobody\n");
        ok = false;
    }

    int status = 0;
    ::waitpid(child, &status, 0);
    Blackboard::unlink(NS, NAME);
    std::printf("  %s\n", verdict(ok));
    return ok;
}

static bool test_forked_writer_does_not_touch_the_parents_key()
{
    std::printf("\nPhase 6: a Writer inherited across fork() owns nothing\n");
    Blackboard::unlink(NS, NAME);

    bool ok = true;
    auto bb = Blackboard::open_or_create(NS, NAME, cfg());
    auto r  = bb.observe(KEY);
    auto w  = bb.declare(KEY, "parent");

    BbPayload mine{};
    fill_payload(mine, 1);
    if (not w.write(mine))
    {
        std::fprintf(stderr, "  [FAIL] parent could not write\n");
        return false;
    }

    pid_t child = checked_fork();
    if (child == 0)
    {
        // Both paths run in the child: write() must refuse, and release() --
        // what ~Writer calls on every exit path -- must leave the claim alone.
        BbPayload theirs{};
        fill_payload(theirs, 2);
        int code = 0;
        if (w.write(theirs))
        {
            code = 1;
        }
        w.release();
        ::_exit(code);
    }

    int status = 0;
    ::waitpid(child, &status, 0);
    if (not WIFEXITED(status) or WEXITSTATUS(status) != 0)
    {
        std::fprintf(stderr, "  [FAIL] the forked child wrote to the parent's key\n");
        ok = false;
    }

    auto snap = bb.snapshot();
    if (snap.size() != 1 or snap[0].owner_pid != current_pid())
    {
        std::fprintf(stderr, "  [FAIL] the child's release dropped the parent's claim\n");
        ok = false;
    }

    BbPayload got{};
    if (r.read(got).status != blackboard::Ok or got.seq != 1)
    {
        std::fprintf(stderr, "  [FAIL] the child's write reached the value\n");
        ok = false;
    }

    fill_payload(mine, 3);
    if (not w.write(mine))
    {
        std::fprintf(stderr, "  [FAIL] parent lost its claim to the fork\n");
        ok = false;
    }

    w.release();
    Blackboard::unlink(NS, NAME);
    std::printf("  %s\n", verdict(ok));
    return ok;
}

int main()
{
    g_shm_name = Blackboard::shm_name(NS, NAME);
    kickmsg_test::register_cleanup_shm(g_shm_name.c_str());
    kickmsg_test::install_signal_cleanup();

    uint64_t seed = seed_fuzzer();
    std::printf("Blackboard crash test (seed %llu -- set KICKMSG_BB_SEED to replay)\n",
                static_cast<unsigned long long>(seed));

    bool all_ok = true;
    all_ok = test_value_survives_owner_death() and all_ok;
    all_ok = test_takeover_after_owner_death() and all_ok;
    all_ok = test_declare_race_across_processes() and all_ok;
    all_ok = test_no_leak_after_repeated_crash_cycles() and all_ok;
    all_ok = test_unreaped_owner_is_reclaimable() and all_ok;
    all_ok = test_forked_writer_does_not_touch_the_parents_key() and all_ok;

    Blackboard::unlink(NS, NAME);
    char const* tag = "[FAIL]";
    int         code = 1;
    if (all_ok)
    {
        tag  = "[PASS]";
        code = 0;
    }
    std::printf("\n  %s\n", tag);
    return code;
}
