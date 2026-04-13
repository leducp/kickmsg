"""Zero-copy publish (allocate/publish) and zero-copy receive (SampleView)."""

from __future__ import annotations

import kickmsg


def test_publisher_allocate_returns_writable_memoryview(shm_name, small_cfg):
    region = kickmsg.SharedRegion.create(shm_name, kickmsg.ChannelType.PubSub, small_cfg, "test")
    pub = kickmsg.Publisher(region)
    sub = kickmsg.Subscriber(region)

    # Reserve a slot, write directly into the shared-memory page, then publish.
    buf = pub.allocate(16)
    assert buf is not None
    assert not buf.readonly
    buf[:5] = b"hello"
    buf[5:16] = b"-zerocopy!!"  # 11 bytes
    assert pub.publish() > 0

    got = sub.try_receive()
    assert got == b"hello-zerocopy!!"


def test_subscriber_view_buffer_protocol_is_readonly(shm_name, small_cfg):
    region = kickmsg.SharedRegion.create(shm_name, kickmsg.ChannelType.PubSub, small_cfg, "test")
    pub = kickmsg.Publisher(region)
    sub = kickmsg.Subscriber(region)

    pub.send(b"hello-view")
    view = sub.try_receive_view()
    assert view is not None
    assert view.valid()
    assert view.len() == len(b"hello-view")

    mv = view.data()
    # memoryview directly into SHM — must be read-only.
    assert mv.readonly
    assert bytes(mv) == b"hello-view"


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
    _ = bytes(view.data())
    view.release()

    for i in range(small_cfg.pool_size):
        assert pub.send(f"msg-{i}".encode()) > 0


def test_zerocopy_camera_frame_pattern(shm_name):
    """Mimic the robot-camera use case:
    allocate directly into SHM, fill (here with a byte pattern; in real
    code this would be a memcpy from V4L2 / OpenCV / DMA), publish,
    receive as zero-copy memoryview, interpret as bytes without any
    intermediate copy on either side.
    """
    cfg = kickmsg.Config()
    cfg.max_subscribers = 2
    cfg.sub_ring_capacity = 4
    cfg.pool_size = 8
    cfg.max_payload_size = 640 * 480 * 3  # RGB VGA

    region = kickmsg.SharedRegion.create(shm_name, kickmsg.ChannelType.PubSub, cfg, "camera")
    pub = kickmsg.Publisher(region)
    sub = kickmsg.Subscriber(region)

    frame_size = 320 * 240 * 3  # smaller test frame
    buf = pub.allocate(frame_size)
    assert buf is not None
    # Fill with a pattern — cheap, full write coverage.
    for i in range(0, frame_size, 4096):
        chunk = min(4096, frame_size - i)
        buf[i:i + chunk] = bytes([(i + j) & 0xFF for j in range(chunk)])
    pub.publish()

    view = sub.try_receive_view()
    try:
        assert view is not None
        assert view.len() == frame_size
        # Spot-check some bytes without copying the whole frame.
        mv = view.data()
        assert mv[0] == 0
        assert mv[1] == 1
        assert mv[4095] == 255
        assert mv[frame_size - 1] == (frame_size - 1) & 0xFF
    finally:
        if view is not None:
            view.release()
