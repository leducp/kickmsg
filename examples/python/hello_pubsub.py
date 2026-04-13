"""Basic pub/sub via the Node API — Python counterpart of examples/hello_pubsub.cc."""

from __future__ import annotations

import struct

import kickmsg


def main() -> int:
    prefix = "demo_py"
    topic = "temperature"

    sensor = kickmsg.Node("sensor", prefix)
    display = kickmsg.Node("display", prefix)

    # Clean any leftover SHM from prior runs.
    sensor.unlink_topic(topic)

    pub = sensor.advertise(topic)
    sub = display.subscribe(topic)

    readings = [(1, 22.5), (2, 19.8), (1, 23.1), (3, 31.4), (2, 20.0)]
    for sensor_id, celsius in readings:
        # Pack as (uint32 id, float celsius) — same layout as the C++ example.
        pub.send(struct.pack("<If", sensor_id, celsius))

    while (sample := sub.try_receive()) is not None:
        sensor_id, celsius = struct.unpack("<If", sample)
        print(f"Sensor {sensor_id}: {celsius:.1f} C")

    sensor.unlink_topic(topic)
    print("Done.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
