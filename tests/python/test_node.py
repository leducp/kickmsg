"""Node API coverage -- advertise/subscribe, late binding, mailbox."""

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
