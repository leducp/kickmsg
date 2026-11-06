"""Schema descriptor in Python — counterpart of examples/hello_schema.cc."""

from __future__ import annotations

import kickmsg

DEFINITION = "demo.Imu(timestamp_ns:u64, ax:f32, ay:f32, az:f32)"


def make_imu_schema(version: int) -> kickmsg.SchemaInfo:
    info = kickmsg.SchemaInfo()
    info.identity = kickmsg.hash.identity_from_fnv1a(DEFINITION)
    info.name = "demo/Imu"
    info.version = version
    info.identity_algo = kickmsg.schema.Algo.fnv1a_64.value
    info.blob_hash = kickmsg.hash.identity_from_fnv1a(DEFINITION)
    info.blob_algo = kickmsg.schema.Algo.fnv1a_64.value
    return info


def main() -> int:
    prefix = "demo_py_schema"
    topic = "imu"

    # Publisher node bakes a v2 schema into the region at create time.
    pub_node = kickmsg.Node("driver", prefix)
    pub_node.unlink_topic(topic)

    cfg = kickmsg.Config()
    cfg.max_subscribers = 4
    cfg.sub_ring_capacity = 8
    cfg.pool_size = 16
    cfg.max_payload_size = 32
    cfg.schema = make_imu_schema(version=2)
    cfg.schema_capacity = 256
    cfg.schema_blob = DEFINITION.encode()

    _ = pub_node.advertise(topic, cfg)
    print("[driver] advertised '%s' with schema %s v%d" % (
        topic, cfg.schema.name, cfg.schema.version))

    # Good subscriber: expects v2 → matches → proceeds.
    good = kickmsg.Node("good_sub", prefix)
    _ = good.subscribe(topic)
    got = good.topic_schema(topic)
    expected = make_imu_schema(version=2)
    d = kickmsg.schema.diff(got, expected)
    print("[good_sub]  observed %s v%d — diff=0x%x %s" % (
        got.name, got.version, d, "OK" if d == kickmsg.schema.Diff.Equal else "MISMATCH"))
    print("[good_sub]  schema blob: %s" % good.topic_schema_blob(topic).decode())

    # Bad subscriber: expects v1 → diff reports Version mismatch → refuses.
    bad = kickmsg.Node("bad_sub", prefix)
    _ = bad.subscribe(topic)
    got = bad.topic_schema(topic)
    expected_v1 = make_imu_schema(version=1)
    d = kickmsg.schema.diff(got, expected_v1)
    if d & kickmsg.schema.Diff.Version:
        print("[bad_sub]   version mismatch (observed v%d, expected v1) — refusing"
              % got.version)

    pub_node.unlink_topic(topic)
    print("Done.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
