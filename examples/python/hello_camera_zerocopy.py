"""Zero-copy camera-frame publishing — the use case the Python bindings'
Publisher.allocate / Subscriber.try_receive_view path was designed for.

Shows how to:
  1. Reserve a slot directly in shared memory via Publisher.allocate(size)
     and fill it in-place (no intermediate buffer).  Here we simulate a
     camera frame with a byte pattern; in a real pipeline you'd:
       - cv2.imdecode / numpy.copyto into the slot, or
       - DMA from a V4L2 buffer into the slot, or
       - render directly into the slot.
  2. Receive it zero-copy on the other side via Subscriber.try_receive_view
     and read the bytes through a read-only memoryview — no copy either.

For a real camera, swap the `fill_frame` lines for your actual capture.
"""

from __future__ import annotations

import kickmsg


def fill_frame(buf: memoryview, height: int, width: int) -> None:
    """Stand-in for a real capture — writes a cheap pattern."""
    stride = width * 3
    for row in range(height):
        for col in range(width):
            off = row * stride + col * 3
            buf[off] = row & 0xFF         # R
            buf[off + 1] = col & 0xFF     # G
            buf[off + 2] = (row + col) & 0xFF  # B


def main() -> int:
    width, height = 320, 240
    frame_bytes = width * height * 3  # RGB

    cfg = kickmsg.Config()
    cfg.max_subscribers = 2
    cfg.sub_ring_capacity = 4
    cfg.pool_size = 8
    cfg.max_payload_size = frame_bytes

    camera = kickmsg.Node("camera", "demo_cam")
    viewer = kickmsg.Node("viewer", "demo_cam")
    camera.unlink_topic("frames")

    pub = camera.advertise("frames", cfg)
    sub = viewer.subscribe("frames")

    for i in range(3):
        # Zero-copy capture: get a writable memoryview into the SHM slot,
        # fill it in place, publish.  Nothing is copied on the publish side.
        buf = pub.allocate(frame_bytes)
        if buf is None:
            print(f"frame {i}: pool exhausted, dropping")
            continue
        fill_frame(buf, height, width)
        pub.publish()
        print(f"Published frame {i} ({width}x{height} RGB, {frame_bytes} B zero-copy)")

    for i in range(3):
        view = sub.try_receive_view()
        if view is None:
            break
        try:
            mv = view.data()
            # Spot-check: no memcpy performed on the receive side either.
            print(f"Received frame {i}: {view.len()} B, "
                  f"pixel[0,0]=RGB({mv[0]},{mv[1]},{mv[2]}), "
                  f"pixel[{height-1},{width-1}]="
                  f"RGB({mv[(height-1)*width*3 + (width-1)*3]},"
                  f"{mv[(height-1)*width*3 + (width-1)*3 + 1]},"
                  f"{mv[(height-1)*width*3 + (width-1)*3 + 2]})")
        finally:
            # Drop the slot pin as soon as we're done reading.  Without
            # this, the slot stays pinned until the SampleView is GC'd,
            # and the pool could run dry.
            view.release()

    camera.unlink_topic("frames")
    print("Done.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
