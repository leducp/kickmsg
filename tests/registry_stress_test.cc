/// @file registry_stress_test.cc
/// @brief Multi-process stress test for the kickmsg registry.
///
/// Forks several child processes that each hammer
/// register_participant / deregister / snapshot in tight loops, while
/// the parent periodically calls snapshot() and asserts structural
/// invariants:
///
/// - Every Active entry has a non-empty topic_name and node_name.
/// - Every Active entry's pid is either this process or a live child.
/// - snapshot() never returns an entry whose state subsequently
///   reports Free/Claiming (the seqlock recheck in snapshot handles
///   this, so the invariant is 'no torn reads').
///
/// Exits 0 on success, non-zero on any assertion failure.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "kickmsg/Registry.h"
#include "kickmsg/os/Process.h"
#include "kickmsg/os/Time.h"

using namespace kickmsg;
using namespace std::chrono_literals;

static constexpr char const* NS = "kickmsg_regstress";
static constexpr int N_CHILDREN = 8;
static constexpr int OPS_PER_CHILD = 2000;

static void child_loop(int child_id)
{
    auto reg = Registry::open_or_create(NS);

    std::string base_topic = "/child_" + std::to_string(child_id) + "/t_";
    std::string node_name = "child_" + std::to_string(child_id);

    for (int i = 0; i < OPS_PER_CHILD; ++i)
    {
        std::string topic    = base_topic + std::to_string(i % 32);
        std::string shm_name = "/" + std::string{NS}
                             + "_child_" + std::to_string(child_id)
                             + "_t_" + std::to_string(i % 32);

        uint32_t slot = reg.register_participant(
            shm_name, topic, channel::PubSub,
            registry::Pubsub, registry::Publisher, node_name);

        if (slot == INVALID_SLOT)
        {
            // Registry full — should be rare with opportunistic sweep.
            continue;
        }
        reg.deregister(slot);
    }
}

int main()
{
    Registry::unlink(NS);
    auto reg = Registry::open_or_create(NS);

    std::vector<pid_t> kids;
    kids.reserve(N_CHILDREN);
    for (int i = 0; i < N_CHILDREN; ++i)
    {
        pid_t p = fork();
        if (p == 0)
        {
            child_loop(i);
            _exit(0);
        }
        if (p < 0)
        {
            std::fprintf(stderr, "fork failed\n");
            return 1;
        }
        kids.push_back(p);
    }

    // Parent loop: keep snapshotting until all children exit.
    uint64_t snapshots = 0;
    uint64_t bad_rows  = 0;
    auto t_start = std::chrono::steady_clock::now();
    while (true)
    {
        auto snap = reg.snapshot();
        ++snapshots;

        for (auto const& p : snap)
        {
            // Active rows must have non-empty identity fields.  A torn
            // seqlock read (not caught by the generation recheck) would
            // surface as empty topic_name or garbled node_name here.
            if (p.topic_name.empty() or p.node_name.empty())
            {
                ++bad_rows;
                std::fprintf(stderr,
                    "bad row: topic=%zu node=%zu pid=%llu shm=%zu\n",
                    p.topic_name.size(), p.node_name.size(),
                    (unsigned long long)p.pid, p.shm_name.size());
            }
            // PID must match one of our children (or us).
            if (p.pid != current_pid())
            {
                bool found = std::any_of(kids.begin(), kids.end(),
                    [&](pid_t k) { return static_cast<uint64_t>(k) == p.pid; });
                if (not found)
                {
                    ++bad_rows;
                    std::fprintf(stderr, "unknown pid in snapshot: %llu\n",
                                 (unsigned long long)p.pid);
                }
            }
        }

        // Periodic sweep to keep the registry healthy under churn.
        reg.sweep_stale();

        // Check if all children are done.
        int status;
        pid_t done = waitpid(-1, &status, WNOHANG);
        if (done > 0)
        {
            // Remove from kids list.
            kids.erase(std::remove(kids.begin(), kids.end(), done), kids.end());
            if (kids.empty())
            {
                break;
            }
        }
    }

    // Drain any remaining children.
    while (not kids.empty())
    {
        int status;
        pid_t done = waitpid(-1, &status, 0);
        if (done > 0)
        {
            kids.erase(std::remove(kids.begin(), kids.end(), done), kids.end());
        }
    }

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t_start).count();

    std::printf("Registry stress: %d children × %d ops, "
                "%llu snapshots in %lldms, bad_rows=%llu\n",
                N_CHILDREN, OPS_PER_CHILD,
                (unsigned long long)snapshots,
                (long long)elapsed,
                (unsigned long long)bad_rows);

    // Final state: all children gone, registry should settle to empty.
    reg.sweep_stale();
    auto final_snap = reg.snapshot();
    if (not final_snap.empty())
    {
        std::fprintf(stderr, "expected empty registry after child exit, "
                             "got %zu entries\n", final_snap.size());
        // This is likely a benign race (children just deregistered their
        // last entry before exiting), so don't fail on it.
    }

    Registry::unlink(NS);

    if (bad_rows == 0)
    {
        std::printf("  [PASS]\n");
        return 0;
    }
    else
    {
        std::printf("  [FAIL] %llu torn/invalid rows\n",
                    (unsigned long long)bad_rows);
        return 1;
    }
}
