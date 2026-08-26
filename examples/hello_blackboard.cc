/// @file hello_blackboard.cc
/// @brief State, not stream: a late reader sees the current value at once.
///
/// A Subscriber attaches at its ring's current write_pos and never sees
/// anything published before it. So a node advertising its lifecycle state
/// over a topic has to heartbeat forever, and a listener that starts late
/// still waits a full period before it learns anything.
///
/// A blackboard inverts that. The writer publishes each key ONCE and stops;
/// any reader that attaches afterwards -- a minute later, an hour later --
/// reads the current value immediately, along with how old it is and whether
/// the process that wrote it is still alive.
///
/// This example runs both sides in one process for brevity. In production
/// they are separate processes; nothing about the API changes.

#include <cstdio>
#include <thread>

#include "kickmsg/Node.h"
#include "kickmsg/os/Time.h"

using namespace kickmsg;
using namespace std::chrono_literals;

namespace
{
    char const* const BOARD = "robot";

    enum Lifecycle : uint32_t
    {
        Initializing = 0,
        Active       = 1,
        Degraded     = 2,
    };

    struct ArmState
    {
        uint32_t lifecycle;
        uint32_t fault_code;
        float    temperature_c;
    };

    char const* lifecycle_name(uint32_t value)
    {
        char const* name = "UNKNOWN";
        switch (static_cast<Lifecycle>(value))
        {
            case Initializing: { name = "INITIALIZING"; break; }
            case Active:       { name = "ACTIVE";       break; }
            case Degraded:     { name = "DEGRADED";     break; }
        }
        return name;
    }

    using seconds_f = duration<double>;

    /// How long ago a value was written, from its monotonic stamp.
    seconds_f age_of(uint64_t updated_at_ns)
    {
        return elapsed_time(nanoseconds{static_cast<int64_t>(updated_at_ns)});
    }

    char const* status_name(blackboard::Status status)
    {
        char const* name = "?";
        switch (status)
        {
            case blackboard::Ok:        { name = "Ok";        break; }
            case blackboard::Missing:   { name = "Missing";   break; }
            case blackboard::Unset:     { name = "Unset";     break; }
            case blackboard::Truncated: { name = "Truncated"; break; }
            case blackboard::Busy:      { name = "Busy";      break; }
            case blackboard::SizeMismatch: { name = "SizeMismatch"; break; }
        }
        return name;
    }
}

int main()
{
    Node cleanup("cleanup", "demo");
    cleanup.unlink_blackboard(BOARD);   // clear leftovers from a previous run

    // --- The writer publishes once and stops ------------------------------
    Node  arm("arm_driver", "demo");
    auto& board = arm.blackboard(BOARD);

    // Keys are labelled with the Node's own name; no need to repeat it.
    auto state_key = board.declare("arm/state");
    auto mode_key  = board.declare("arm/mode");
    // Held, not discarded: a Writer releases its key when it goes out of scope.
    auto calib_key = board.declare("arm/calibration");

    ArmState state{Active, 0, 41.5f};
    state_key.write(state);
    mode_key.write(uint32_t{7});
    std::printf("[writer] published arm/state and arm/mode -- once. No heartbeat loop.\n");

    // Let the values get demonstrably old.
    std::this_thread::sleep_for(300ms);

    // --- A reader that starts afterwards ----------------------------------
    Node  hmi("hmi", "demo");
    auto& hmi_board = hmi.blackboard(BOARD);

    auto state_view = hmi_board.observe("arm/state");
    auto mode_view  = hmi_board.observe("arm/mode");

    ArmState seen{};
    auto     out = state_view.read(seen);
    std::printf("[reader] arm/state -> %s (%s, fault %u, %.1f C) age %.3fs owner_alive=%d\n",
                status_name(out.status), lifecycle_name(seen.lifecycle),
                seen.fault_code, static_cast<double>(seen.temperature_c),
                age_of(out.updated_at_ns).count(),
                static_cast<int>(state_view.owner_alive()));

    uint32_t mode = 0;
    out = mode_view.read(mode);
    std::printf("[reader] arm/mode  -> %s (%u) age %.3fs\n",
                status_name(out.status), mode, age_of(out.updated_at_ns).count());
    std::printf("[reader] a Subscriber here would have received nothing at all.\n\n");

    // --- Two states a topic cannot express --------------------------------
    ArmState ignored{};
    std::printf("[reader] arm/gripper     -> %s   (no writer ever declared it)\n",
                status_name(hmi_board.observe("arm/gripper").read(ignored).status));
    std::printf("[reader] arm/calibration -> %s     (declared, never written)\n\n",
                status_name(hmi_board.observe("arm/calibration").read(ignored).status));

    // --- Change notification, without polling -----------------------------
    // Read the sequence BEFORE acting on the current values, then wait on it:
    // that ordering is what makes a change during the read impossible to miss.
    std::thread updater([&]
    {
        for (uint32_t i = 0; i < 2; ++i)
        {
            std::this_thread::sleep_for(30ms);
            state.lifecycle  = Degraded;
            state.fault_code = 100 + i;
            state_key.write(state);
        }
    });

    for (int i = 0; i < 3; ++i)
    {
        uint64_t seq = hmi_board.change_seq();
        if (hmi_board.wait(seq, 250ms))
        {
            state_view.read(seen);
            std::printf("[reader] woke on change: arm/state = %s fault %u\n",
                        lifecycle_name(seen.lifecycle), seen.fault_code);
        }
        else
        {
            std::printf("[reader] quiet for 250ms, nothing changed\n");
        }
    }
    updater.join();

    // --- What `kickmsg bb robot -n demo` renders --------------------------
    std::printf("\n%-18s %6s %8s %9s  %-12s %s\n",
                "KEY", "BYTES", "UPDATES", "AGE", "OWNER", "ALIVE");
    for (auto const& key : hmi_board.snapshot())
    {
        char const* alive = "no";
        if (key.owner_alive)
        {
            alive = "yes";
        }
        char age[16];
        if (key.update_count == 0)
        {
            std::snprintf(age, sizeof(age), "%9s", "never");
        }
        else
        {
            std::snprintf(age, sizeof(age), "%8.3fs", age_of(key.updated_at_ns).count());
        }
        std::printf("%-18s %6zu %8llu %9s  %-12s %s\n",
                    key.key.c_str(), key.value_len,
                    static_cast<unsigned long long>(key.update_count),
                    age, key.owner_node.c_str(), alive);
    }

    cleanup.unlink_blackboard(BOARD);
    return 0;
}
