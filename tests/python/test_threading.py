"""GIL release on blocking receive — a consumer thread blocked in
Subscriber.receive(timeout) must not stall a concurrent producer thread.
"""

from __future__ import annotations

import threading

import kickmsg


def test_blocking_receive_does_not_stall_other_thread(shm_name, small_cfg):
    region = kickmsg.SharedRegion.create(
        shm_name, kickmsg.ChannelType.PubSub, small_cfg, "test")
    pub = kickmsg.Publisher(region)
    sub = kickmsg.Subscriber(region)

    received: list[bytes] = []
    consumer_started = threading.Event()

    def consumer():
        consumer_started.set()
        # 500 ms is plenty — GIL release lets the producer thread publish
        # immediately; the futex wakes us up on commit; total wall time
        # should be milliseconds, not approaching the timeout.
        got = sub.receive(500_000_000)
        if got is not None:
            received.append(got)

    t = threading.Thread(target=consumer)
    t.start()
    # Wait until the consumer is in its receive() call — without GIL
    # release, our next line would deadlock here.
    assert consumer_started.wait(timeout=1.0)

    # Give the consumer a moment to actually enter the blocking call.
    # Without GIL release, this send would never run until the receive
    # timed out.  With GIL release, it runs immediately.
    pub.send(b"unblocked")

    t.join(timeout=2.0)
    assert not t.is_alive(), "consumer thread did not return — GIL not released?"
    assert received == [b"unblocked"]
