"""Zero-copy publish via AllocatedSlot, zero-copy receive via SampleView.

Both paths use the Python buffer protocol:
  - `memoryview(slot)`  → writable view into the publisher-reserved SHM slot
  - `memoryview(view)`  → read-only view into the subscriber-pinned SHM slot
The memoryview pins its source object alive (Py_buffer::obj + Py_INCREF),
so retaining a memoryview past the Python reference to slot/view keeps the
underlying shared memory valid until the last memoryview is released.
"""

from __future__ import annotations

import pytest

import kickmsg


def test_publisher_allocate_returns_writable_slot(shm_name, small_cfg):
    region = kickmsg.SharedRegion.create(shm_name, kickmsg.ChannelType.PubSub, small_cfg, "test")
    pub = kickmsg.Publisher(region)
    sub = kickmsg.Subscriber(region)

    slot = pub.allocate(16)
    assert slot is not None
    assert len(slot) == 16
    assert not slot.published

    mv = memoryview(slot)
    assert not mv.readonly
    mv[:5]   = b"hello"
    mv[5:16] = b"-zerocopy!!"
    slot.publish()
    assert slot.published

    got = sub.try_receive()
    assert got == b"hello-zerocopy!!"


def test_subscriber_view_memoryview_is_readonly(shm_name, small_cfg):
    region = kickmsg.SharedRegion.create(shm_name, kickmsg.ChannelType.PubSub, small_cfg, "test")
    pub = kickmsg.Publisher(region)
    sub = kickmsg.Subscriber(region)

    pub.send(b"hello-view")
    view = sub.try_receive_view()
    assert view is not None
    assert view.valid
    assert len(view) == len(b"hello-view")

    mv = memoryview(view)
    assert mv.readonly
    assert bytes(mv) == b"hello-view"


def test_slot_publish_rejects_new_memoryview(shm_name, small_cfg):
    """After slot.publish(), requesting a new memoryview must fail with
    BufferError — the slot is no longer the publisher's to write to, and
    silently allowing a write would corrupt subscribers in flight.
    """
    region = kickmsg.SharedRegion.create(shm_name, kickmsg.ChannelType.PubSub, small_cfg, "test")
    pub = kickmsg.Publisher(region)
    sub = kickmsg.Subscriber(region)

    slot = pub.allocate(8)
    mv1 = memoryview(slot)
    mv1[:4] = b"xxxx"
    slot.publish()

    with pytest.raises(BufferError):
        memoryview(slot)

    # mv1 was obtained before publish() and is still usable as a pointer
    # (the pin keeps it valid); that's the "retained memoryview" case
    # documented as user-beware.  Subscriber still receives what was written.
    assert sub.try_receive()[:4] == b"xxxx"


def test_double_publish_raises(shm_name, small_cfg):
    region = kickmsg.SharedRegion.create(shm_name, kickmsg.ChannelType.PubSub, small_cfg, "test")
    pub = kickmsg.Publisher(region)
    slot = pub.allocate(4)
    memoryview(slot)[:] = b"ping"
    slot.publish()
    with pytest.raises(ValueError, match="more than once"):
        slot.publish()


def test_view_release_returns_slot_to_pool(shm_name, small_cfg):
    region = kickmsg.SharedRegion.create(shm_name, kickmsg.ChannelType.PubSub, small_cfg, "test")
    pub = kickmsg.Publisher(region)
    sub = kickmsg.Subscriber(region)

    # Publish + consume a zero-copy view, then explicitly release the pin.
    # Publishing `pool_size` more messages afterwards must still succeed
    # (the ring/pool isn't wedged).
    pub.send(b"ephemeral")
    view = sub.try_receive_view()
    assert view is not None
    _ = bytes(memoryview(view))
    view.release()

    for i in range(small_cfg.pool_size):
        assert pub.send(f"msg-{i}".encode()) > 0


def test_view_release_makes_new_memoryview_error(shm_name, small_cfg):
    """After view.release(), a new memoryview(view) must raise BufferError."""
    region = kickmsg.SharedRegion.create(shm_name, kickmsg.ChannelType.PubSub, small_cfg, "test")
    pub = kickmsg.Publisher(region)
    sub = kickmsg.Subscriber(region)

    pub.send(b"fleeting")
    view = sub.try_receive_view()
    assert view is not None
    _ = memoryview(view)  # valid while pin is held
    view.release()
    with pytest.raises(BufferError):
        memoryview(view)


def test_zerocopy_camera_frame_pattern(shm_name):
    """Mimic the robot-camera use case: allocate directly into SHM, fill
    with a byte pattern (stand-in for a real capture), publish, then
    receive as zero-copy memoryview — no intermediate copies on either
    side of the channel."""
    cfg = kickmsg.Config()
    cfg.max_subscribers = 2
    cfg.sub_ring_capacity = 4
    cfg.pool_size = 8
    cfg.max_payload_size = 640 * 480 * 3  # RGB VGA

    region = kickmsg.SharedRegion.create(shm_name, kickmsg.ChannelType.PubSub, cfg, "camera")
    pub = kickmsg.Publisher(region)
    sub = kickmsg.Subscriber(region)

    frame_size = 320 * 240 * 3  # smaller test frame
    slot = pub.allocate(frame_size)
    assert slot is not None
    buf = memoryview(slot)
    # Fill with a pattern — cheap, full write coverage.
    for i in range(0, frame_size, 4096):
        chunk = min(4096, frame_size - i)
        buf[i:i + chunk] = bytes([(i + j) & 0xFF for j in range(chunk)])
    slot.publish()

    view = sub.try_receive_view()
    try:
        assert view is not None
        assert len(view) == frame_size
        mv = memoryview(view)
        # Spot-check a few bytes without copying the whole frame.
        assert mv[0] == 0
        assert mv[1] == 1
        assert mv[4095] == 255
        assert mv[frame_size - 1] == (frame_size - 1) & 0xFF
    finally:
        if view is not None:
            view.release()


def test_view_context_manager_releases_pin(shm_name, small_cfg):
    """`with view:` must release the pin on normal block exit — the pool
    is replenished immediately and new publishes don't starve."""
    region = kickmsg.SharedRegion.create(shm_name, kickmsg.ChannelType.PubSub, small_cfg, "test")
    pub = kickmsg.Publisher(region)
    sub = kickmsg.Subscriber(region)

    pub.send(b"in-block")
    view = sub.try_receive_view()
    with view:
        assert bytes(memoryview(view)) == b"in-block"
    assert not view.valid

    # Pool must be fully available — publish pool_size more messages.
    for i in range(small_cfg.pool_size):
        assert pub.send(f"m{i}".encode()) > 0


def test_view_context_manager_releases_on_exception(shm_name, small_cfg):
    """The pin must drop even when the `with` block raises."""
    region = kickmsg.SharedRegion.create(shm_name, kickmsg.ChannelType.PubSub, small_cfg, "test")
    pub = kickmsg.Publisher(region)
    sub = kickmsg.Subscriber(region)

    pub.send(b"throws")
    view = sub.try_receive_view()
    with pytest.raises(RuntimeError, match="boom"):
        with view:
            _ = memoryview(view)
            raise RuntimeError("boom")
    assert not view.valid


def test_memoryview_pins_sampleview(shm_name, small_cfg):
    """Retain a memoryview past the SampleView Python reference — the
    pin must hold (reading is defined, not UB).  This is the load-bearing
    safety guarantee of the buffer-protocol change.
    """
    import gc

    region = kickmsg.SharedRegion.create(shm_name, kickmsg.ChannelType.PubSub, small_cfg, "test")
    pub = kickmsg.Publisher(region)
    sub = kickmsg.Subscriber(region)

    pub.send(b"retained")
    view = sub.try_receive_view()
    mv = memoryview(view)
    del view  # only `mv` keeps the SampleView alive now
    gc.collect(); gc.collect()
    assert bytes(mv) == b"retained"
