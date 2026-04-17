"""Long-running pub/sub demo for poking at the `kickmsg` CLI.

Starts one node that advertises a topic and a second node that subscribes
to it, then publishes forever at a configurable rate.  Ctrl-C to stop —
teardown deregisters both participants from the registry.

While it's running, try:

    kickmsg ls                              # default namespace "demo"
    kickmsg ls -o all
    kickmsg stats   /demo_telemetry
    kickmsg watch   /demo_telemetry -i 0.5
    kickmsg diag    /demo_telemetry
    kickmsg schema  /demo_telemetry
"""

from __future__ import annotations

import argparse
import struct
import sys
import time

import kickmsg


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-n", "--namespace", default="demo")
    ap.add_argument("-t", "--topic", default="telemetry")
    ap.add_argument("-r", "--rate-hz", type=float, default=10.0,
                    help="publish frequency (default: 10 Hz)")
    ap.add_argument("-c", "--consumers", type=int, default=1,
                    help="number of subscriber nodes (default: 1)")
    ap.add_argument("--payload-size", type=int, default=64,
                    help="bytes per message (default: 64)")
    args = ap.parse_args()

    if args.consumers < 1:
        ap.error("--consumers must be >= 1")

    cfg = kickmsg.Config()
    cfg.max_subscribers   = max(args.consumers, 4)
    cfg.sub_ring_capacity = 64
    cfg.pool_size         = 128
    cfg.max_payload_size  = max(args.payload_size, 16)

    # Clean any leftover SHM from a crashed prior run so startup is deterministic.
    cleanup = kickmsg.Node("cleanup", namespace=args.namespace)
    cleanup.unlink_topic(args.topic)
    del cleanup

    sensor = kickmsg.Node("sensor",  namespace=args.namespace)
    pub    = sensor.advertise(args.topic, cfg)

    loggers = [kickmsg.Node(f"logger_{i}", namespace=args.namespace)
               for i in range(args.consumers)]
    subs    = [n.subscribe(args.topic) for n in loggers]

    shm_name = f"/{args.namespace}_{args.topic}"
    period   = 1.0 / args.rate_hz

    print(f"publishing to {shm_name} at {args.rate_hz:g} Hz "
          f"({args.payload_size} B per message)")
    print(f"nodes:  sensor (publisher)   logger_0..{args.consumers - 1} "
          f"({args.consumers} subscriber{'s' if args.consumers != 1 else ''})")
    print(f"try:    kickmsg ls --namespace {args.namespace}")
    print(f"        kickmsg watch {shm_name}")
    print("Ctrl-C to stop.")

    seq          = 0
    received     = 0
    t_start      = time.monotonic()
    pad          = b"\x00" * max(0, args.payload_size - 12)
    next_tick    = t_start

    try:
        while True:
            # Payload: (uint32 seq, uint64 ns_since_start, padding…)
            elapsed_ns = int((time.monotonic() - t_start) * 1e9)
            pub.send(struct.pack("<IQ", seq, elapsed_ns) + pad)
            seq += 1

            # Drain every subscriber so their rings don't overflow on
            # their own while we're holding the loop.
            for sub in subs:
                while sub.try_receive() is not None:
                    received += 1

            # Sleep to an absolute deadline, not a relative duration, so
            # per-loop work doesn't accumulate into the measured rate.
            next_tick += period
            now = time.monotonic()
            if now >= next_tick + period:
                # Large stall (GC, suspend) — skip ahead instead of
                # bursting to catch up.
                next_tick = now + period
            else:
                delay = next_tick - now
                if delay > 0:
                    time.sleep(delay)
    except KeyboardInterrupt:
        print()  # newline after ^C
        elapsed    = time.monotonic() - t_start
        total_lost = sum(s.lost for s in subs)
        print(f"sent {seq} messages, received {received} "
              f"(across {args.consumers} subscriber"
              f"{'s' if args.consumers != 1 else ''}), "
              f"over {elapsed:.1f}s "
              f"(~{seq / max(elapsed, 0.001):.0f} msg/s)")
        print(f"pub.dropped = {pub.dropped}, sub.lost total = {total_lost}")
        return 0


if __name__ == "__main__":
    sys.exit(main())
