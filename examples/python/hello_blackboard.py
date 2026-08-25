"""State, not stream — Python counterpart of examples/hello_blackboard.cc.

A Subscriber attaches at the ring's current position and never sees anything
published before it, so a node advertising its lifecycle over a topic has to
heartbeat forever.  A blackboard inverts that: the writer publishes each key
once and stops, and any reader that attaches afterwards reads the current
value immediately, with its age and its writer's liveness.
"""

from __future__ import annotations

import datetime
import struct
import threading
import time

import kickmsg
from kickmsg import diagnostics as diag


NAMESPACE = "demo_py"
BOARD = "robot"

INITIALIZING, ACTIVE, DEGRADED = 0, 1, 2
_LIFECYCLE = {INITIALIZING: "INITIALIZING", ACTIVE: "ACTIVE", DEGRADED: "DEGRADED"}


def main() -> int:
    cleanup = kickmsg.Node("cleanup", NAMESPACE)
    cleanup.unlink_blackboard(BOARD)   # clear leftovers from a prior run

    # --- The writer publishes once and stops ---------------------------
    arm = kickmsg.Node("arm_driver", NAMESPACE)
    board = arm.blackboard(BOARD)

    # Keys are labelled with the Node's own name; no need to repeat it.
    state_key = board.declare("arm/state")
    mode_key = board.declare("arm/mode")
    calib_key = board.declare("arm/calibration")  # never written

    state_key.write(struct.pack("<IIf", ACTIVE, 0, 41.5))
    mode_key.write(struct.pack("<I", 7))
    print("[writer] published arm/state and arm/mode -- once. No heartbeat loop.")

    time.sleep(0.3)   # let the values get demonstrably old

    # --- A reader that starts afterwards --------------------------------
    hmi = kickmsg.Node("hmi", NAMESPACE)
    hmi_board = hmi.blackboard(BOARD)
    state_view = hmi_board.observe("arm/state")

    out = state_view.read()
    lifecycle, fault, temperature = struct.unpack("<IIf", out.data)
    age = (time.monotonic_ns() - out.updated_at_ns) / 1e9
    print(f"[reader] arm/state -> {out.status.name} "
          f"({_LIFECYCLE[lifecycle]}, fault {fault}, {temperature:.1f} C) "
          f"age {age:.3f}s owner_alive={state_view.owner_alive()}")
    print("[reader] a Subscriber here would have received nothing at all.\n")

    # --- Two states a topic cannot express ------------------------------
    print(f"[reader] arm/gripper     -> {hmi_board.observe('arm/gripper').read().status.name}"
          "   (no writer ever declared it)")
    print(f"[reader] arm/calibration -> {hmi_board.observe('arm/calibration').read().status.name}"
          "     (declared, never written)\n")

    # --- Change notification, without polling ---------------------------
    # Read the sequence BEFORE acting on the current values, then wait on it:
    # that ordering is what makes a change during the read impossible to miss.
    def updater() -> None:
        for i in range(2):
            time.sleep(0.03)
            state_key.write(struct.pack("<IIf", DEGRADED, 100 + i, 44.0))

    thread = threading.Thread(target=updater)
    thread.start()
    for _ in range(3):
        seq = hmi_board.change_seq
        if hmi_board.wait(seq, datetime.timedelta(milliseconds=250)):
            lifecycle, fault, _ = struct.unpack("<IIf", state_view.read().data)
            print(f"[reader] woke on change: arm/state = {_LIFECYCLE[lifecycle]} fault {fault}")
        else:
            print("[reader] quiet for 250ms, nothing changed")
    thread.join()

    # --- What `kickmsg bb robot -n demo_py` renders ----------------------
    snapshot = diag.blackboard(BOARD, NAMESPACE)
    print(f"\n{'KEY':<18} {'BYTES':>6} {'UPDATES':>8} {'AGE':>9}  {'OWNER':<12} ALIVE")
    for key in snapshot.keys:
        age_text = "never"
        if key.age_seconds is not None:
            age_text = f"{key.age_seconds:.3f}s"
        alive = "yes" if key.owner_alive else "no"
        print(f"{key.key:<18} {key.value_len:>6} {key.update_count:>8} "
              f"{age_text:>9}  {key.owner_node:<12} {alive}")

    cleanup.unlink_blackboard(BOARD)
    kickmsg.Registry.unlink(NAMESPACE)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
