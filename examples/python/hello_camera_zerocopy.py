"""Zero-copy camera-frame publishing — the use case the Python bindings'
AllocatedSlot / SampleView buffer-protocol path was designed for.

Shows how to:
  1. Reserve a slot directly in shared memory via Publisher.allocate(size),
     then obtain a writable memoryview with `memoryview(slot)` and fill it
     in-place (no intermediate buffer).  In a real pipeline you'd:
       - numpy.copyto(np.asarray(slot).reshape(H, W, 3), frame), or
       - DMA from a V4L2 buffer into the slot, or
       - render directly into the slot.
     Then call `slot.publish()` to commit.
  2. Receive it zero-copy on the other side via Subscriber.try_receive_view
     and `memoryview(view)` — read-only, no copy either.  The memoryview
     pins the SampleView alive so the slot stays valid until every
     consumer releases the view.

For a real camera, swap `fill_frame` for your actual capture.
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
        # Zero-copy capture: reserve a slot, write directly into SHM via
        # a memoryview, publish.  Nothing is copied on the publish side.
        slot = pub.allocate(frame_bytes)
        if slot is None:
            print(f"frame {i}: pool exhausted, dropping")
            continue
        fill_frame(memoryview(slot), height, width)
        slot.publish()
        print(f"Published frame {i} ({width}x{height} RGB, {frame_bytes} B zero-copy)")

    for i in range(3):
        view = sub.try_receive_view()
        if view is None:
            break
        # `with` releases the pin on block exit, even on exception.
        # Without this, the slot stays pinned until the SampleView and
        # every derived memoryview are GC'd — the pool could run dry.
        with view:
            mv = memoryview(view)  # read-only, zero-copy
            print(f"Received frame {i}: {len(view)} B, "
                  f"pixel[0,0]=RGB({mv[0]},{mv[1]},{mv[2]}), "
                  f"pixel[{height-1},{width-1}]="
                  f"RGB({mv[(height-1)*width*3 + (width-1)*3]},"
                  f"{mv[(height-1)*width*3 + (width-1)*3 + 1]},"
                  f"{mv[(height-1)*width*3 + (width-1)*3 + 2]})")

    camera.unlink_topic("frames")
    print("Done.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
