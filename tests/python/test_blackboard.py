"""Blackboard coverage -- late readers, ownership, notification, diagnostics."""

from __future__ import annotations

import datetime
import errno
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
    w.write(struct.pack("<II", 1, 42))

    # The reader opens the board only now, and never waits for a second write.
    other = kickmsg.Blackboard.try_open(NS, name)
    assert other is not None
    out = other.observe("arm/state").read()

    assert out
    assert struct.unpack("<II", out.data) == (1, 42)
    assert out.update_count == 1
    assert bool(out) is True
    assert len(out) == 8


def test_declare_twice_from_live_owner_raises(board):
    bb, _ = board
    w = bb.declare("owned")
    with pytest.raises(RuntimeError):
        bb.declare("owned")


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
    stop = threading.Event()

    def ticker():
        while not stop.is_set():
            time.sleep(0.005)
            ticks.append(1)

    t = threading.Thread(target=ticker)
    t.start()
    try:
        # Nothing writes, so wait() times out; a held GIL freezes the ticker until it returns.
        assert bb.wait(bb.change_seq, datetime.timedelta(milliseconds=300)) is False
        ticked = len(ticks)
    finally:
        stop.set()
        t.join()
    assert ticked >= 5


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
            if reader.read():
                break
            time.sleep(0.02)
        assert reader.read().data == b"survivor"

        writer.send_signal(signal.SIGKILL)
        writer.wait()

        # The writer is gone; its last value is still there.
        out = reader.read()
        assert out
        assert out.data == b"survivor"
        assert reader.owner_alive() is False

        snap = diag.blackboard(name, NS)
        assert snap.status == "stale owners"
        assert snap.dead_owner_keys == 1

        assert diag.blackboard_sweep_stale(name, NS) == 1
        assert reader.read().errno == errno.ENOENT
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
        w.write(b"x" * 128)
        with pytest.raises(OSError) as over:
            w.write(b"x" * 129)
        assert over.value.errno == errno.EMSGSIZE
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
