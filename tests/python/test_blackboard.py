"""Blackboard coverage -- late readers, ownership, notification, diagnostics."""

from __future__ import annotations

import datetime
import os
import signal
import struct
import subprocess
import sys
import time

import pytest

import kickmsg
from kickmsg import diagnostics as diag


NS = "pytest_bb"


@pytest.fixture
def board(request):
    """A blackboard named after the test, unlinked before and after."""
    name = request.node.name[:20].replace("[", "_").replace("]", "_")
    kickmsg.Blackboard.unlink(NS, name)
    cfg = kickmsg.BlackboardConfig()
    cfg.capacity = 8
    cfg.max_value_size = 128
    yield kickmsg.Blackboard.open_or_create(NS, name, cfg), name
    kickmsg.Blackboard.unlink(NS, name)


def test_late_reader_sees_current_value(board):
    bb, name = board
    w = bb.declare("arm/state", "arm_driver")
    assert w.write(struct.pack("<II", 1, 42))

    # The reader opens the board only now, and never waits for a second write.
    other = kickmsg.Blackboard.try_open(NS, name)
    assert other is not None
    out = other.observe("arm/state").read()

    assert out.status == kickmsg.BlackboardStatus.Ok
    assert struct.unpack("<II", out.data) == (1, 42)
    assert out.update_count == 1
    assert bool(out) is True
    assert len(out) == 8


def test_observe_before_declare_resolves_lazily(board):
    bb, _ = board
    reader = bb.observe("late/key")
    assert reader.read().status == kickmsg.BlackboardStatus.Missing

    w = bb.declare("late/key")
    w.write(b"here")
    # Same reader object, no re-observe.
    out = reader.read()
    assert out.status == kickmsg.BlackboardStatus.Ok
    assert out.data == b"here"


def test_missing_and_unset_are_distinct(board):
    bb, _ = board
    w = bb.declare("declared/only")
    assert bb.observe("never/declared").read().status == kickmsg.BlackboardStatus.Missing
    assert bb.observe("declared/only").read().status == kickmsg.BlackboardStatus.Unset


def test_declare_twice_from_live_owner_raises(board):
    bb, _ = board
    w = bb.declare("owned")
    with pytest.raises(RuntimeError):
        bb.declare("owned")


def test_released_key_keeps_its_value(board):
    bb, _ = board
    reader = bb.observe("released")
    w = bb.declare("released")
    w.write(b"last-known")
    w.release()

    out = reader.read()
    assert out.status == kickmsg.BlackboardStatus.Ok
    assert out.data == b"last-known"
    assert reader.owner_alive() is False

    # And the key can be taken over, value intact.
    w2 = bb.declare("released")
    assert reader.read().data == b"last-known"


def test_value_too_large_is_rejected(board):
    bb, _ = board
    w = bb.declare("k")
    w.write(b"small")
    assert w.write(b"x" * 4096) is False
    assert bb.observe("k").read().data == b"small"


def test_empty_key_raises(board):
    bb, _ = board
    with pytest.raises(ValueError):
        bb.declare("")
    with pytest.raises(ValueError):
        bb.observe("")


def test_capacity_exhaustion_raises():
    cfg = kickmsg.BlackboardConfig()
    cfg.capacity = 2
    cfg.max_value_size = 32
    kickmsg.Blackboard.unlink(NS, "tiny")
    bb = kickmsg.Blackboard.open_or_create(NS, "tiny", cfg)
    try:
        a = bb.declare("a")
        b = bb.declare("b")
        with pytest.raises(RuntimeError):
            bb.declare("c")
    finally:
        kickmsg.Blackboard.unlink(NS, "tiny")


def test_wait_returns_false_on_timeout(board):
    bb, _ = board
    w = bb.declare("quiet")
    seq = bb.change_seq
    start = time.monotonic()
    assert bb.wait(seq, datetime.timedelta(milliseconds=50)) is False
    assert time.monotonic() - start >= 0.045


def test_wait_returns_immediately_when_seq_already_advanced(board):
    bb, _ = board
    w = bb.declare("k")
    stale = bb.change_seq
    w.write(b"v")

    start = time.monotonic()
    assert bb.wait(stale, datetime.timedelta(seconds=5)) is True
    assert time.monotonic() - start < 1.0


def test_wait_releases_the_gil(board):
    """A blocked wait() must not stall other Python threads."""
    import threading

    bb, _ = board
    w = bb.declare("k")
    ticks = []

    def ticker():
        for _ in range(5):
            time.sleep(0.01)
            ticks.append(1)

    t = threading.Thread(target=ticker)
    t.start()
    bb.wait(bb.change_seq, datetime.timedelta(milliseconds=200))
    t.join()
    assert len(ticks) == 5


def test_try_open_returns_none_when_absent():
    kickmsg.Blackboard.unlink(NS, "absent")
    assert kickmsg.Blackboard.try_open(NS, "absent") is None


def test_geometry_mismatch_raises(board):
    bb, name = board
    other = kickmsg.BlackboardConfig()
    other.capacity = 64
    other.max_value_size = 128
    with pytest.raises(RuntimeError):
        kickmsg.Blackboard.open_or_create(NS, name, other)


def test_node_blackboard_is_idempotent_and_registers():
    node = kickmsg.Node("bbnode", "pytest_bbnode")
    try:
        a = node.blackboard("state")
        b = node.blackboard("state")
        # Not the literal path: macOS hashes shm names to fit PSHMNAMLEN.
        expected = kickmsg.Blackboard.shm_name("pytest_bbnode", "state")
        assert a.name == b.name == expected

        topics = diag.list_topics("pytest_bbnode")
        rows = [t for t in topics if t.shm_name == expected]
        assert len(rows) == 1
        assert rows[0].kind == "blackboard"
        # A blackboard has no ring geometry; list_topics must not try to open
        # it as a channel region.
        assert rows[0].channel_type == "-"
        assert rows[0].schema_name is None
    finally:
        node.unlink_blackboard("state")
        kickmsg.Registry.unlink("pytest_bbnode")


def test_diagnostics_snapshot(board):
    bb, name = board
    w = bb.declare("arm/state", "arm_driver")
    w.write(b"12345")
    unwritten = bb.declare("arm/calibration", "arm_driver")

    snap = diag.blackboard(name, NS)
    assert snap is not None
    assert snap.status == "healthy"
    assert snap.dead_owner_keys == 0
    assert [k.key for k in snap.keys] == ["arm/calibration", "arm/state"]

    calib, state = snap.keys
    assert calib.update_count == 0 and calib.age_seconds is None
    assert state.value_len == 5 and state.owner_node == "arm_driver"
    assert state.owner_alive and state.age_seconds is not None


def test_diagnostics_absent_board_returns_none():
    kickmsg.Blackboard.unlink(NS, "nothing")
    assert diag.blackboard("nothing", NS) is None
    assert diag.blackboard_sweep_stale("nothing", NS) == 0


@pytest.mark.skipif(sys.platform == "win32", reason="POSIX fork/SIGKILL")
def test_value_survives_owner_death_and_is_sweepable():
    name = "crash"
    kickmsg.Blackboard.unlink(NS, name)
    writer = subprocess.Popen([
        sys.executable, "-c",
        "import time, kickmsg;"
        f"bb = kickmsg.Blackboard.open_or_create('{NS}', '{name}');"
        "w = bb.declare('arm/state', 'child');"
        "w.write(b'survivor');"
        "time.sleep(60)",
    ])
    try:
        bb = kickmsg.Blackboard.open_or_create(NS, name)
        reader = bb.observe("arm/state")
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            if reader.read().status == kickmsg.BlackboardStatus.Ok:
                break
            time.sleep(0.02)
        assert reader.read().data == b"survivor"

        writer.send_signal(signal.SIGKILL)
        writer.wait()

        # The writer is gone; its last value is still there.
        out = reader.read()
        assert out.status == kickmsg.BlackboardStatus.Ok
        assert out.data == b"survivor"
        assert reader.owner_alive() is False

        snap = diag.blackboard(name, NS)
        assert snap.status == "stale owners"
        assert snap.dead_owner_keys == 1

        assert diag.blackboard_sweep_stale(name, NS) == 1
        assert reader.read().status == kickmsg.BlackboardStatus.Missing
    finally:
        if writer.poll() is None:
            writer.send_signal(signal.SIGKILL)
            writer.wait()
        kickmsg.Blackboard.unlink(NS, name)


def test_max_value_size_is_exactly_as_configured(request):
    """Alignment padding must not be handed out as extra payload capacity."""
    name = "exact"
    kickmsg.Blackboard.unlink(NS, name)
    cfg = kickmsg.BlackboardConfig()
    cfg.capacity = 4
    cfg.max_value_size = 128
    bb = kickmsg.Blackboard.open_or_create(NS, name, cfg)
    try:
        assert bb.max_value_size == 128
        w = bb.declare("k")
        assert w.write(b"x" * 128) is True
        assert w.write(b"x" * 129) is False
    finally:
        kickmsg.Blackboard.unlink(NS, name)


def test_cli_reports_absent_board(capsys):
    """bbwatch and bb must both fail on a board that does not exist."""
    from kickmsg import cli

    kickmsg.Blackboard.unlink(NS, "ghost")
    assert cli.main(["blackboard", "ghost", "-n", NS]) == 1
    assert cli.main(["blackboard-watch", "ghost", "-n", NS]) == 1


def test_node_blackboard_cache_does_not_alias_sanitized_names():
    node = kickmsg.Node("bbnode", "pytest_bbalias")
    try:
        node.blackboard("a:b")
        with pytest.raises(RuntimeError):
            node.blackboard("a b")
    finally:
        node.unlink_blackboard("a:b")
        kickmsg.Registry.unlink("pytest_bbalias")
