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


def test_send_larger_than_max_payload_raises(shm_name, small_cfg):
    import pytest
    region = kickmsg.SharedRegion.create(shm_name, kickmsg.ChannelType.PubSub, small_cfg, "test")
    pub = kickmsg.Publisher(region)
    oversize = b"x" * (small_cfg.max_payload_size + 1)
    with pytest.raises(ValueError, match="max_payload_size"):
        pub.send(oversize)


def test_send_pool_exhausted_raises(shm_name, small_cfg):
    """When the pool is dry (all slots pinned, none free), send() raises
    BlockingIOError — never a silent negative-int a Python user would
    discard.  Simulated by a subscriber that holds SampleView pins and
    never releases them, so slots can't be recycled."""
    import pytest
    region = kickmsg.SharedRegion.create(shm_name, kickmsg.ChannelType.PubSub, small_cfg, "test")
    pub = kickmsg.Publisher(region)
    sub = kickmsg.Subscriber(region)

    # Publish + pin one by one until the pool runs dry.  Keep the views
    # alive in a list so their pins prevent the slots returning to the pool.
    pinned = []
    sent_ok = 0
    with pytest.raises(BlockingIOError):
        for _ in range(small_cfg.pool_size * 4):
            pub.send(b"x")
            sent_ok += 1
            view = sub.try_receive_view()
            if view is not None:
                pinned.append(view)  # refcount held → slot can't recycle
    assert sent_ok > 0
    assert len(pinned) > 0
