"""Node API coverage — advertise/subscribe, broadcast, mailbox, schema forwarding."""

from __future__ import annotations

import kickmsg


def test_advertise_and_subscribe_on_one_node(small_cfg):
    # Each Node test uses unique prefixes to avoid SHM name collisions
    # across test runs.  unlink_topic in tear-down keeps the namespace clean.
    node = kickmsg.Node("tn", "pytest_adv")
    try:
        pub = node.advertise("data", small_cfg)
        sub = node.subscribe("data")
        assert pub.send(b"payload") > 0
        assert sub.try_receive() == b"payload"
    finally:
        node.unlink_topic("data")


def test_subscribe_or_create_late_binding(small_cfg):
    listener = kickmsg.Node("listener", "pytest_late")
    driver = kickmsg.Node("driver", "pytest_late")
    try:
        sub = listener.subscribe_or_create("imu", small_cfg)
        pub = driver.advertise_or_join("imu", small_cfg)
        pub.send(b"imu-data")
        assert sub.try_receive() == b"imu-data"
    finally:
        listener.unlink_topic("imu")


def test_join_broadcast_two_nodes(small_cfg):
    a = kickmsg.Node("a", "pytest_bc")
    b = kickmsg.Node("b", "pytest_bc")
    try:
        ha = a.join_broadcast("events", small_cfg)
        hb = b.join_broadcast("events", small_cfg)
        ha.pub.send(b"from-a")
        assert hb.sub.try_receive() == b"from-a"
        assert ha.sub.try_receive() == b"from-a"
    finally:
        a.unlink_broadcast("events")


def test_mailbox_pattern(small_cfg):
    owner = kickmsg.Node("owner", "pytest_mbx")
    caller = kickmsg.Node("caller", "pytest_mbx")
    try:
        inbox = owner.create_mailbox("inbox", small_cfg)
        send = caller.open_mailbox("owner", "inbox")
        send.send(b"ping")
        assert inbox.try_receive() == b"ping"
    finally:
        owner.unlink_mailbox("inbox")


def test_topic_schema_roundtrip_via_node(small_cfg):
    pub_node = kickmsg.Node("p", "pytest_schema")
    sub_node = kickmsg.Node("s", "pytest_schema")
    try:
        small_cfg.schema = kickmsg.SchemaInfo()
        small_cfg.schema.name = "demo/Type"
        small_cfg.schema.version = 7
        small_cfg.schema.identity = bytes([0x5A]) * 64
        small_cfg.schema.layout = b"\x00" * 64
        _ = pub_node.advertise("topic", small_cfg)

        # Must be able to read back via *either* node.
        _ = sub_node.subscribe("topic")
        got = sub_node.topic_schema("topic")
        assert got is not None
        assert got.name == "demo/Type"
        assert got.version == 7
    finally:
        pub_node.unlink_topic("topic")


def test_topic_schema_none_for_unknown_topic():
    node = kickmsg.Node("orphan", "pytest_unknown")
    assert node.topic_schema("never_joined") is None
