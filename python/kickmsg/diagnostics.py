"""Diagnostic API for kickmsg.

Typed dataclass wrappers around the native bindings, intended for both
the `kickmsg` CLI and third-party code (GUIs, exporters) that wants to
inspect running channels without shelling out.
"""

from __future__ import annotations

import time
from dataclasses import dataclass, field
from typing import Iterator

from . import _native


# ----------------------------------------------------------------------
# Result dataclasses
# ----------------------------------------------------------------------


@dataclass(frozen=True)
class Creator:
    pid: int
    name: str
    created_at_ns: int


@dataclass(frozen=True)
class Geometry:
    max_subscribers: int
    sub_ring_capacity: int
    pool_size: int
    max_payload_size: int
    commit_timeout_us: int
    total_size: int


@dataclass(frozen=True)
class SchemaSnapshot:
    """Result of `schema(shm_name)`.

    `state` is one of "unset" / "claiming" / "set".  `info` is populated
    only when state == "set".
    """
    state: str
    name: str | None = None
    version: int | None = None
    identity: bytes | None = None
    layout: bytes | None = None
    identity_algo: int | None = None
    layout_algo: int | None = None
    flags: int | None = None


@dataclass(frozen=True)
class SchemaDiff:
    """Bit-decomposed schema::diff() result."""
    equal: bool
    identity: bool
    layout: bool
    version: bool
    name: bool
    identity_algo: bool
    layout_algo: bool


@dataclass(frozen=True)
class RegionInfo:
    """Static (non-runtime) snapshot of a SharedRegion header."""
    shm_name: str
    channel_type: str              # "pubsub" | "broadcast"
    version: int
    config_hash: int
    geometry: Geometry
    creator: Creator
    schema: SchemaSnapshot


@dataclass(frozen=True)
class RingStats:
    index: int
    state: str                     # "free" | "live" | "draining"
    in_flight: int
    write_pos: int
    dropped_count: int
    lost_count: int


@dataclass(frozen=True)
class RegionStats:
    """Runtime counter snapshot — cheap, safe under live traffic."""
    shm_name: str
    rings: list[RingStats]
    total_writes: int
    total_drops: int
    total_losses: int
    live_rings: int
    pool_free: int
    pool_size: int


@dataclass(frozen=True)
class HealthSnapshot:
    """Interpretation of `SharedRegion::diagnose()`."""
    shm_name: str
    locked_entries: int
    retired_rings: int
    draining_rings: int
    live_rings: int
    schema_stuck: bool
    status: str                    # "healthy" | "crash residue" | "schema wedged"


@dataclass(frozen=True)
class Participant:
    pid: int
    pid_starttime: int             # boot-relative (Linux), 0 elsewhere
    node_name: str
    topic_name: str                # user-facing logical path
    shm_name: str                  # POSIX SHM path (implementation detail)
    kind: str                      # "pubsub" | "broadcast" | "mailbox"
    channel_type: str              # "pubsub" | "broadcast"
    role: str                      # "publisher" | "subscriber" | "both"
    created_at_ns: int
    # Liveness is carried by which list the entry was in on TopicSummary
    # (producers vs stall_producers).  Callers needing a fresh probe use
    # `kickmsg.process_exists(p.pid)`.


@dataclass(frozen=True)
class TopicSummary:
    topic_name: str                # user-facing logical path
    shm_name: str                  # POSIX SHM path
    kind: str                      # "pubsub" | "broadcast" | "mailbox"
    channel_type: str
    kmsg_namespace: str
    age_seconds: float | None
    producers: list[Participant] = field(default_factory=list)
    consumers: list[Participant] = field(default_factory=list)
    stall_producers: list[Participant] = field(default_factory=list)
    stall_consumers: list[Participant] = field(default_factory=list)
    schema_name: str | None = None
    schema_version: int | None = None


@dataclass(frozen=True)
class WatchSnapshot:
    """One frame of `watch()` output.  Rates are msg/s deltas since the
    previous snapshot; zero on the first frame."""
    stats: RegionStats
    rates_msg_per_sec: list[float]


# ----------------------------------------------------------------------
# Internal helpers
# ----------------------------------------------------------------------


_CHANNEL_NAME = {
    _native.ChannelType.PubSub.value: "pubsub",
    _native.ChannelType.Broadcast.value: "broadcast",
}

_ROLE_NAME = {
    _native.Role.Publisher.value: "publisher",
    _native.Role.Subscriber.value: "subscriber",
    _native.Role.Both.value: "both",
}

_KIND_NAME = {
    _native.Kind.Pubsub.value:    "pubsub",
    _native.Kind.Broadcast.value: "broadcast",
    _native.Kind.Mailbox.value:   "mailbox",
}

_RING_STATE = {0: "free", 1: "live", 2: "draining"}


def _schema_snapshot(region: _native.SharedRegion) -> SchemaSnapshot:
    # `SharedRegion.schema()` returns None when the slot is Unset or
    # Claiming — the native API collapses both.  We fall back to
    # diagnose().schema_stuck to split "never set" from "mid-claim wedge".
    info = region.schema()
    if info is not None:
        return SchemaSnapshot(
            state="set",
            name=info.name,
            version=info.version,
            identity=info.identity,
            layout=info.layout,
            identity_algo=info.identity_algo,
            layout_algo=info.layout_algo,
            flags=info.flags,
        )
    if region.diagnose().schema_stuck:
        return SchemaSnapshot(state="claiming")
    return SchemaSnapshot(state="unset")


def _open(shm_name: str) -> _native.SharedRegion:
    return _native.SharedRegion.open(shm_name)


# ----------------------------------------------------------------------
# Public API
# ----------------------------------------------------------------------


def info(shm_name: str) -> RegionInfo:
    """Static header metadata for a region."""
    region = _open(shm_name)
    header = region.info()
    return RegionInfo(
        shm_name=header.shm_name,
        channel_type=_CHANNEL_NAME.get(header.channel_type.value, "?"),
        version=header.version,
        config_hash=header.config_hash,
        geometry=Geometry(
            max_subscribers=header.max_subs,
            sub_ring_capacity=header.sub_ring_capacity,
            pool_size=header.pool_size,
            max_payload_size=header.max_payload_size,
            commit_timeout_us=header.commit_timeout_us,
            total_size=header.total_size,
        ),
        creator=Creator(
            pid=header.creator_pid,
            name=header.creator_name,
            created_at_ns=header.created_at_ns,
        ),
        schema=_schema_snapshot(region),
    )


def stats(shm_name: str) -> RegionStats:
    """Runtime counter snapshot."""
    region = _open(shm_name)
    s = region.stats()
    rings = [
        RingStats(
            index=i,
            state=_RING_STATE.get(r.state, "?"),
            in_flight=r.in_flight,
            write_pos=r.write_pos,
            dropped_count=r.dropped_count,
            lost_count=r.lost_count,
        )
        for i, r in enumerate(s.rings)
    ]
    return RegionStats(
        shm_name=region.name,
        rings=rings,
        total_writes=s.total_writes,
        total_drops=s.total_drops,
        total_losses=s.total_losses,
        live_rings=s.live_rings,
        pool_free=s.pool_free,
        pool_size=s.pool_size,
    )


def diagnose(shm_name: str) -> HealthSnapshot:
    """Wraps SharedRegion::diagnose() with an interpretation."""
    region = _open(shm_name)
    r = region.diagnose()

    if r.locked_entries > 0 or r.retired_rings > 0:
        status = "crash residue"
    elif r.schema_stuck:
        status = "schema wedged"
    else:
        status = "healthy"

    return HealthSnapshot(
        shm_name=region.name,
        locked_entries=r.locked_entries,
        retired_rings=r.retired_rings,
        draining_rings=r.draining_rings,
        live_rings=r.live_rings,
        schema_stuck=r.schema_stuck,
        status=status,
    )


def watch(shm_name: str, interval: float = 1.0) -> Iterator[WatchSnapshot]:
    """Generator yielding stats snapshots every `interval` seconds.

    First frame has zero rates.  Caller drives the loop and breaks when
    done.  Cooperates with Ctrl-C via normal generator semantics.
    """
    region = _open(shm_name)
    prev: list[int] | None = None
    prev_t = 0.0
    while True:
        now = time.monotonic()
        raw = region.stats()
        if prev is None or now <= prev_t:
            rates = [0.0] * len(raw.rings)
        else:
            dt = now - prev_t
            rates = [
                max(0.0, (r.write_pos - p) / dt) for r, p in zip(raw.rings, prev)
            ]

        rings = [
            RingStats(
                index=i,
                state=_RING_STATE.get(r.state, "?"),
                in_flight=r.in_flight,
                write_pos=r.write_pos,
                dropped_count=r.dropped_count,
                lost_count=r.lost_count,
            )
            for i, r in enumerate(raw.rings)
        ]
        snap = RegionStats(
            shm_name=region.name,
            rings=rings,
            total_writes=raw.total_writes,
            total_drops=raw.total_drops,
            total_losses=raw.total_losses,
            live_rings=raw.live_rings,
            pool_free=raw.pool_free,
            pool_size=raw.pool_size,
        )
        yield WatchSnapshot(stats=snap, rates_msg_per_sec=rates)

        prev = [r.write_pos for r in raw.rings]
        prev_t = now
        time.sleep(interval)


def schema(shm_name: str) -> SchemaSnapshot:
    """Focused read of just the schema slot."""
    return _schema_snapshot(_open(shm_name))


def schema_diff(shm_a: str, shm_b: str) -> SchemaDiff:
    """Field-by-field diff of two schemas via `schema::diff()`.

    Raises ValueError if either region has no published schema (state !=
    'set') — there's nothing meaningful to diff otherwise.
    """
    a = _open(shm_a).schema()
    b = _open(shm_b).schema()
    if a is None or b is None:
        raise ValueError("schema_diff requires both regions to have a published schema")
    bits = _native.schema.diff(a, b)
    return SchemaDiff(
        equal=(bits == _native.schema.Diff.Equal.value),
        identity=bool(bits & _native.schema.Diff.Identity.value),
        layout=bool(bits & _native.schema.Diff.Layout.value),
        version=bool(bits & _native.schema.Diff.Version.value),
        name=bool(bits & _native.schema.Diff.Name.value),
        identity_algo=bool(bits & _native.schema.Diff.IdentityAlgo.value),
        layout_algo=bool(bits & _native.schema.Diff.LayoutAlgo.value),
    )


def repair_locked(shm_name: str) -> int:
    """Commit entries stuck at LOCKED_SEQUENCE. Safe under live traffic."""
    return _open(shm_name).repair_locked_entries()


def reset_retired(shm_name: str) -> int:
    """Reset retired rings. Only safe after confirming the crashed
    publisher is gone."""
    return _open(shm_name).reset_retired_rings()


def reclaim_orphaned(shm_name: str) -> int:
    """Reclaim orphaned pool slots. Requires full quiescence."""
    return _open(shm_name).reclaim_orphaned_slots()


# ----------------------------------------------------------------------
# Topic-centric discovery
# ----------------------------------------------------------------------


def _to_participant(p: "_native.Participant") -> Participant:
    return Participant(
        pid=p.pid,
        pid_starttime=p.pid_starttime,
        node_name=p.node_name,
        topic_name=p.topic_name,
        shm_name=p.shm_name,
        kind=_KIND_NAME.get(p.kind, "?"),
        channel_type=_CHANNEL_NAME.get(p.channel_type, "?"),
        role=_ROLE_NAME.get(p.role, "?"),
        created_at_ns=p.created_at_ns,
    )


def list_topics(kmsg_namespace: str = "kickmsg") -> list[TopicSummary]:
    """Topic-centric enumeration grouped from the registry.

    Thin wrapper over `Registry::list_topics()` — the aggregation and
    liveness classification happen in C++.  This layer adds schema
    enrichment (name + version from the region header, when published)
    and converts the native dataclasses into Python ones.
    """
    registry = _native.Registry.try_open(kmsg_namespace)
    if registry is None:
        return []

    now_ns = time.time_ns()
    out: list[TopicSummary] = []
    for native_t in registry.list_topics():
        schema_name = None
        schema_version = None
        try:
            region = _native.SharedRegion.open(native_t.shm_name)
            s = _schema_snapshot(region)
            if s.state == "set":
                schema_name = s.name
                schema_version = s.version
        except RuntimeError:
            # Region may have been unlinked between snapshot and open —
            # skip schema enrichment, keep the row.
            pass

        # Topic age = now - earliest participant registration.
        all_parts = (list(native_t.producers) + list(native_t.consumers)
                   + list(native_t.stall_producers) + list(native_t.stall_consumers))
        age_seconds = None
        if all_parts:
            earliest_ns = min(p.created_at_ns for p in all_parts)
            if earliest_ns > 0:
                age_seconds = max(0.0, (now_ns - earliest_ns) / 1e9)

        out.append(TopicSummary(
            topic_name=native_t.topic_name,
            shm_name=native_t.shm_name,
            kind=_KIND_NAME.get(native_t.kind, "?"),
            channel_type=_CHANNEL_NAME.get(native_t.channel_type, "?"),
            kmsg_namespace=kmsg_namespace,
            age_seconds=age_seconds,
            producers=[_to_participant(p) for p in native_t.producers],
            consumers=[_to_participant(p) for p in native_t.consumers],
            stall_producers=[_to_participant(p) for p in native_t.stall_producers],
            stall_consumers=[_to_participant(p) for p in native_t.stall_consumers],
            schema_name=schema_name,
            schema_version=schema_version,
        ))
    return out
