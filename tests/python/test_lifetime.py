"""Lifetime edge cases — do the keep_alive chains hold and does GC
release slot pins?

These tests guard the subtle nanobind wiring:
    Publisher/Subscriber → SharedRegion   (keep_alive<1,2> on __init__)
    Node-returned Pub/Sub → Node          (keep_alive<0,1> on factory methods)
    SampleView → Subscriber               (keep_alive<0,1> on try_receive_view)
    BroadcastHandle.pub/sub → handle → Node
                                          (reference_internal + transitive)
"""

from __future__ import annotations

import gc

import kickmsg


def test_broadcasthandle_pub_extracted_from_temporary(small_cfg):
    """`pub = node.join_broadcast(...).pub` drops the handle reference
    immediately.  The chain Publisher → BroadcastHandle → Node must
    survive so the publisher can still send after the GC cycles.
    """
    a = kickmsg.Node("a", "pytest_lt_bc")
    b = kickmsg.Node("b", "pytest_lt_bc")
    try:
        pub = a.join_broadcast("events", small_cfg).pub
        sub = b.join_broadcast("events", small_cfg).sub

        gc.collect()  # force any intermediate references to drop
        gc.collect()

        assert pub.send(b"survived-gc") > 0
        assert sub.try_receive() == b"survived-gc"
    finally:
        a.unlink_broadcast("events")


def test_sample_view_gc_releases_pin(shm_name, small_cfg):
    """Take a zero-copy view, discard it without calling .release(),
    force GC, then verify the slot came back to the pool by publishing
    pool_size more messages successfully.
    """
    region = kickmsg.SharedRegion.create(
        shm_name, kickmsg.ChannelType.PubSub, small_cfg, "test")
    pub = kickmsg.Publisher(region)
    sub = kickmsg.Subscriber(region)

    pub.send(b"ephemeral")
    view = sub.try_receive_view()
    assert view is not None
    _ = bytes(memoryview(view))

    # Drop the reference — Python GC should run SampleView's C++ dtor,
    # which in turn drops the slot pin.
    view = None
    gc.collect()

    # Pool should be clean — pool_size more publishes must succeed.
    for i in range(small_cfg.pool_size):
        assert pub.send(f"msg-{i}".encode()) > 0


def test_publisher_outlives_shared_region_python_reference(shm_name, small_cfg):
    """Drop the Python SharedRegion reference right after constructing
    the Publisher.  The keep_alive<1,2> on Publisher(region) must hold
    the mmap alive — otherwise the publisher's send() would touch
    freed memory.
    """
    region = kickmsg.SharedRegion.create(
        shm_name, kickmsg.ChannelType.PubSub, small_cfg, "test")
    pub = kickmsg.Publisher(region)
    sub = kickmsg.Subscriber(region)
    region = None  # only pub + sub keep the mapping alive now
    gc.collect()

    assert pub.send(b"still alive") > 0
    assert sub.try_receive() == b"still alive"
