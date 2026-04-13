"""Schema descriptor + diff + hash helpers."""

from __future__ import annotations

import kickmsg


def make_schema(name: str, version: int, identity_byte: int = 0xAA) -> kickmsg.SchemaInfo:
    info = kickmsg.SchemaInfo()
    info.name = name
    info.version = version
    info.identity = bytes([identity_byte]) * 64
    info.layout = b"\x00" * 64
    info.identity_algo = 1
    info.layout_algo = 0
    info.flags = 0
    return info


def test_fnv1a_64_is_deterministic():
    # Canonical FNV-1a 64-bit reference vectors.
    assert kickmsg.hash.fnv1a_64(b"") == 0xCBF29CE484222325
    assert kickmsg.hash.fnv1a_64(b"a") == 0xAF63DC4C8601EC8C
    assert kickmsg.hash.fnv1a_64(b"foobar") == 0x85944171F73967E8


def test_identity_from_fnv1a_pads_zero():
    identity = kickmsg.hash.identity_from_fnv1a("demo/Imu")
    assert len(identity) == 64
    # Remaining bytes after the leading 8 are zero.
    assert identity[8:] == b"\x00" * 56


def test_schema_baked_at_create(shm_name, small_cfg):
    small_cfg.schema = make_schema("my/Pose", 2, 0xAB)
    region = kickmsg.SharedRegion.create(shm_name, kickmsg.ChannelType.PubSub, small_cfg, "test")

    got = region.schema()
    assert got is not None
    assert got.name == "my/Pose"
    assert got.version == 2
    assert got.identity[0] == 0xAB


def test_schema_diff_detects_mismatch(shm_name, small_cfg):
    region = kickmsg.SharedRegion.create(shm_name, kickmsg.ChannelType.PubSub, small_cfg, "test")
    claimed = make_schema("demo/Type", 1, 0x11)
    assert region.try_claim_schema(claimed) is True

    expected_v2 = make_schema("demo/Type", 2, 0x11)
    got = region.schema()
    assert got is not None

    d = kickmsg.schema.diff(got, expected_v2)
    assert d & int(kickmsg.schema.Diff.Version)
    assert not (d & int(kickmsg.schema.Diff.Identity))
    assert not (d & int(kickmsg.schema.Diff.Name))


def test_schema_reset_recovers_wedged_claiming(shm_name, small_cfg):
    region = kickmsg.SharedRegion.create(shm_name, kickmsg.ChannelType.PubSub, small_cfg, "test")
    # No direct API to force Claiming state from Python — verify the normal
    # reset path is callable and returns False on an unset region.
    assert region.reset_schema_claim() is False
    claimed = make_schema("done/Type", 1)
    assert region.try_claim_schema(claimed) is True
    # Already Set → reset is a no-op.
    assert region.reset_schema_claim() is False
