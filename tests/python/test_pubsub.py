"""Basic pub/sub roundtrip tests via the SharedRegion + Publisher/Subscriber API."""

from __future__ import annotations

import kickmsg


def test_send_and_try_receive_roundtrip(shm_name, small_cfg):
    region = kickmsg.SharedRegion.create(shm_name, kickmsg.ChannelType.PubSub, small_cfg, "test")
    pub = kickmsg.Publisher(region)
    sub = kickmsg.Subscriber(region)

    assert pub.send(b"hello") > 0
    got = sub.try_receive()
    assert got == b"hello"
    # Drained — next try_receive returns None.
    assert sub.try_receive() is None


def test_try_receive_is_none_on_empty(shm_name, small_cfg):
    region = kickmsg.SharedRegion.create(shm_name, kickmsg.ChannelType.PubSub, small_cfg, "test")
    sub = kickmsg.Subscriber(region)
    assert sub.try_receive() is None


def test_receive_with_timeout_returns_none(shm_name, small_cfg):
    region = kickmsg.SharedRegion.create(shm_name, kickmsg.ChannelType.PubSub, small_cfg, "test")
    sub = kickmsg.Subscriber(region)
    # 10 ms timeout, no publisher — must return None without throwing.
    got = sub.receive(10_000_000)
    assert got is None


def test_receive_with_timeout_delivers(shm_name, small_cfg):
    region = kickmsg.SharedRegion.create(shm_name, kickmsg.ChannelType.PubSub, small_cfg, "test")
    pub = kickmsg.Publisher(region)
    sub = kickmsg.Subscriber(region)
    pub.send(b"payload")
    got = sub.receive(100_000_000)  # 100 ms
    assert got == b"payload"


def test_multiple_subscribers_receive_same_message(shm_name, small_cfg):
    region = kickmsg.SharedRegion.create(shm_name, kickmsg.ChannelType.PubSub, small_cfg, "test")
    pub = kickmsg.Publisher(region)
    sub_a = kickmsg.Subscriber(region)
    sub_b = kickmsg.Subscriber(region)

    pub.send(b"fan-out")
    assert sub_a.try_receive() == b"fan-out"
    assert sub_b.try_receive() == b"fan-out"


def test_send_larger_than_max_payload_fails(shm_name, small_cfg):
    region = kickmsg.SharedRegion.create(shm_name, kickmsg.ChannelType.PubSub, small_cfg, "test")
    pub = kickmsg.Publisher(region)
    oversize = b"x" * (small_cfg.max_payload_size + 1)
    assert pub.send(oversize) < 0  # EMSGSIZE-style negative return
