"""Tests for the `kickmsg.diagnostics` typed API.

Covers the shape of the returned dataclasses and the round-trip
through the registry that backs `list_topics`.
"""

from __future__ import annotations

import os

import pytest

import kickmsg
from kickmsg import diagnostics as diag


@pytest.fixture
def kmsg_namespace() -> str:
    """Namespace unique to this test run; cleaned before and after."""
    ns = f"pytest_diag_{os.getpid()}"
    kickmsg.Registry.unlink(ns)
    yield ns
    kickmsg.Registry.unlink(ns)


# ----------------------------------------------------------------------
# list_topics
# ----------------------------------------------------------------------


def test_list_topics_empty_returns_empty(kmsg_namespace):
    assert diag.list_topics(kmsg_namespace) == []


def test_list_topics_aggregates_by_shm(kmsg_namespace, small_cfg):
    pub_node = kickmsg.Node("producer", namespace=kmsg_namespace)
    pub = pub_node.advertise("telemetry", small_cfg)

    sub_node = kickmsg.Node("consumer", namespace=kmsg_namespace)
    sub = sub_node.subscribe("telemetry")

    topics = diag.list_topics(kmsg_namespace)
    assert len(topics) == 1
    t = topics[0]
    assert t.shm_name == f"/{kmsg_namespace}_telemetry"
    assert t.channel_type == "pubsub"
    assert len(t.producers) == 1
    assert len(t.consumers) == 1
    assert t.producers[0].node_name == "producer"
    assert t.consumers[0].node_name == "consumer"
    assert len(t.stall_producers) == 0
    assert len(t.stall_consumers) == 0

    # Node destructors deregister — referenced to keep them alive above.
    del pub, sub, pub_node, sub_node
    kickmsg.unlink_shm(f"/{kmsg_namespace}_telemetry")


def test_list_topics_broadcast_is_both_role(kmsg_namespace, small_cfg):
    node = kickmsg.Node("bcast", namespace=kmsg_namespace)
    handle = node.join_broadcast("events", small_cfg)

    topics = diag.list_topics(kmsg_namespace)
    assert len(topics) == 1
    t = topics[0]
    assert t.channel_type == "broadcast"
    # Broadcast node counts as both producer and consumer.
    assert len(t.producers) == 1
    assert len(t.consumers) == 1
    assert t.producers[0].role == "both"

    del handle, node
    kickmsg.unlink_shm(f"/{kmsg_namespace}_broadcast_events")


# ----------------------------------------------------------------------
# stats
# ----------------------------------------------------------------------


def test_stats_reports_write_pos_after_publishes(kmsg_namespace, small_cfg):
    node = kickmsg.Node("writer", namespace=kmsg_namespace)
    pub = node.advertise("data", small_cfg)
    sub = node.subscribe("data")

    for _ in range(5):
        pub.send(b"x" * 16)

    s = diag.stats(f"/{kmsg_namespace}_data")
    assert s.live_rings == 1
    assert s.total_writes == 5
    assert s.total_drops == 0
    # RingStats shape
    live_rings = [r for r in s.rings if r.state == "live"]
    assert len(live_rings) == 1
    assert live_rings[0].write_pos == 5

    del pub, sub, node
    kickmsg.unlink_shm(f"/{kmsg_namespace}_data")


# ----------------------------------------------------------------------
# diagnose
# ----------------------------------------------------------------------


def test_diagnose_healthy(kmsg_namespace, small_cfg):
    node = kickmsg.Node("healthy", namespace=kmsg_namespace)
    pub = node.advertise("topic", small_cfg)

    h = diag.diagnose(f"/{kmsg_namespace}_topic")
    assert h.status == "healthy"
    assert h.locked_entries == 0
    assert h.retired_rings == 0

    del pub, node
    kickmsg.unlink_shm(f"/{kmsg_namespace}_topic")


# ----------------------------------------------------------------------
# schema
# ----------------------------------------------------------------------


def test_schema_unset_by_default(kmsg_namespace, small_cfg):
    node = kickmsg.Node("n", namespace=kmsg_namespace)
    pub = node.advertise("topic", small_cfg)

    s = diag.schema(f"/{kmsg_namespace}_topic")
    assert s.state == "unset"
    assert s.name is None

    del pub, node
    kickmsg.unlink_shm(f"/{kmsg_namespace}_topic")


def test_schema_set_after_claim(kmsg_namespace, small_cfg):
    info = kickmsg.SchemaInfo()
    info.name = "my/Type"
    info.version = 3
    info.identity = b"\x01" * 64
    info.layout = b"\x02" * 64

    cfg = small_cfg
    cfg.schema = info

    node = kickmsg.Node("n", namespace=kmsg_namespace)
    pub = node.advertise("topic", cfg)

    s = diag.schema(f"/{kmsg_namespace}_topic")
    assert s.state == "set"
    assert s.name == "my/Type"
    assert s.version == 3
    assert s.identity == b"\x01" * 64

    del pub, node
    kickmsg.unlink_shm(f"/{kmsg_namespace}_topic")


def test_schema_diff_detects_version_delta(kmsg_namespace, small_cfg):
    info_a = kickmsg.SchemaInfo()
    info_a.name = "Type"
    info_a.version = 1
    info_a.identity = b"\x01" * 64
    info_a.layout = b"\x02" * 64

    info_b = kickmsg.SchemaInfo()
    info_b.name = "Type"
    info_b.version = 2  # differs
    info_b.identity = b"\x01" * 64
    info_b.layout = b"\x02" * 64

    cfg_a = kickmsg.Config()
    cfg_a.max_subscribers = 2
    cfg_a.sub_ring_capacity = 4
    cfg_a.pool_size = 8
    cfg_a.max_payload_size = 32
    cfg_a.schema = info_a

    cfg_b = kickmsg.Config()
    cfg_b.max_subscribers = 2
    cfg_b.sub_ring_capacity = 4
    cfg_b.pool_size = 8
    cfg_b.max_payload_size = 32
    cfg_b.schema = info_b

    node = kickmsg.Node("n", namespace=kmsg_namespace)
    pa = node.advertise("topic_a", cfg_a)
    pb = node.advertise("topic_b", cfg_b)

    d = diag.schema_diff(
        f"/{kmsg_namespace}_topic_a",
        f"/{kmsg_namespace}_topic_b",
    )
    assert not d.equal
    assert d.version
    assert not d.identity
    assert not d.layout

    del pa, pb, node
    kickmsg.unlink_shm(f"/{kmsg_namespace}_topic_a")
    kickmsg.unlink_shm(f"/{kmsg_namespace}_topic_b")


# ----------------------------------------------------------------------
# CLI smoke — just that the module parses and main() runs end-to-end.
# ----------------------------------------------------------------------


def test_cli_list_empty_still_prints_header(kmsg_namespace, capsys):
    from kickmsg.cli import main as cli_main
    rc = cli_main(["list", "--namespace", kmsg_namespace])
    captured = capsys.readouterr()
    # Empty list is not an error — exit 0, header still rendered so the
    # column structure is visible.
    assert rc == 0
    assert "TOPIC" in captured.out
    assert "NS" in captured.out


def test_cli_list_shows_registered_topic(kmsg_namespace, small_cfg, capsys):
    from kickmsg.cli import main as cli_main

    node = kickmsg.Node("cli_test", namespace=kmsg_namespace)
    pub = node.advertise("cli_topic", small_cfg)

    rc = cli_main(["list", "--namespace", kmsg_namespace])
    captured = capsys.readouterr()
    assert rc == 0
    assert "cli_topic" in captured.out
    assert "pubsub" in captured.out

    del pub, node
    kickmsg.unlink_shm(f"/{kmsg_namespace}_cli_topic")


@pytest.mark.parametrize("kind,setup,topic,expected_suffix", [
    # (label, fixture-style setup on a Node, logical topic path, SHM suffix)
    ("pubsub",
     lambda node, ns, cfg: node.advertise("reading", cfg),
     "/reading",
     "_reading"),
    ("broadcast",
     lambda node, ns, cfg: node.join_broadcast("events", cfg),
     "/events",
     "_broadcast_events"),
    ("mailbox",
     lambda node, ns, cfg: node.create_mailbox("reply", cfg),
     None,  # mailbox topic is /<owner>/<tag>, filled below from node name
     "_resolver_mbx_reply"),
])
def test_cli_resolves_topic_via_registry(kmsg_namespace, small_cfg,
                                         kind, setup, topic, expected_suffix):
    """Every channel kind (pubsub / broadcast / mailbox) must resolve a
    logical topic to the right SHM name via registry lookup.

    Regression: when `_native` wasn't imported in `_cli.py`, the lookup
    silently fell through to a pubsub-pattern guess that worked only for
    pubsub — broadcast and mailbox silently resolved to the wrong SHM.
    """
    from kickmsg.cli import _resolve_shm_name

    node = kickmsg.Node("resolver", namespace=kmsg_namespace)
    handle = setup(node, kmsg_namespace, small_cfg)

    # Mailbox logical topic is /<owner>/<tag>.
    logical = topic if topic is not None else f"/resolver/reply"

    class Args:
        shm = None
        namespace = kmsg_namespace
    Args.topic = logical

    resolved = _resolve_shm_name(Args())
    assert resolved == f"/{kmsg_namespace}{expected_suffix}", (
        f"{kind}: expected /{kmsg_namespace}{expected_suffix}, got {resolved}"
    )

    del handle, node
    kickmsg.unlink_shm(f"/{kmsg_namespace}{expected_suffix}")


def test_cli_resolve_rejects_unknown_topic(kmsg_namespace):
    """With no fallback, an unknown topic is an explicit error, not a
    silent wrong-region guess."""
    from kickmsg.cli import _resolve_shm_name

    class Args:
        shm = None
        topic = "/nonexistent"
        namespace = kmsg_namespace

    with pytest.raises(SystemExit) as info:
        _resolve_shm_name(Args())
    msg = str(info.value).lower()
    # Either "no registry exists for this namespace" or "topic not found
    # in this namespace" — both are explicit errors, not a silent guess.
    assert "no registry" in msg or "not found" in msg


def test_cli_resolve_shm_overrides_topic(kmsg_namespace):
    """--shm always wins, even if --namespace/topic are set."""
    from kickmsg.cli import _resolve_shm_name

    class Args:
        shm = "raw_name"  # auto-prepended leading '/'
        topic = "/ignored"
        namespace = kmsg_namespace

    assert _resolve_shm_name(Args()) == "/raw_name"
