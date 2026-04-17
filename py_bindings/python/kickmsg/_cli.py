"""Argparse entry point — thin formatting layer over `kickmsg.diagnostics`.

Every data-fetching path lives in `kickmsg.diagnostics`; this module only
renders results.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from dataclasses import asdict
from typing import Sequence

from . import _native
from . import diagnostics as diag


# Env default stays CLI-only so the core library remains env-agnostic.
_ENV_VAR = "KICKMSG_NAMESPACE"


def _default_namespace() -> str:
    value = os.environ.get(_ENV_VAR)
    if value:
        return value
    return "kickmsg"


def _normalize_shm_name(name: str) -> str:
    """POSIX shm_open requires a leading '/'; accept names without it."""
    if name.startswith("/"):
        return name
    return "/" + name


def _normalize_topic(name: str) -> str:
    if not name.startswith("/"):
        return "/" + name
    return name


def _add_region_target(sp: argparse.ArgumentParser) -> None:
    """Attach the (topic | --shm) target selector."""
    sp.add_argument("topic", nargs="?", default=None,
                    help="Topic path (leading '/' optional).  Combined with "
                         "--namespace to find the region.  Use --shm for "
                         "explicit SHM names.")
    sp.add_argument("--shm", default=None,
                    help="Raw SHM name (leading '/' optional).  Overrides "
                         "positional topic when both are given.")
    sp.add_argument("-n", "--namespace", default=_default_namespace(),
                    help=f"Namespace used with positional topic. Defaults to "
                         f"${_ENV_VAR} if set, else \"kickmsg\".")


def _resolve_shm_name(args) -> str:
    if args.shm is not None:
        return _normalize_shm_name(args.shm)

    if args.topic is None:
        raise SystemExit(
            "error: need a topic (positional) or --shm SHM_NAME")

    topic = _normalize_topic(args.topic)

    # Registry lookup only — no pubsub-pattern fallback, since guessing
    # silently gave the wrong SHM name for broadcast/mailbox topics.
    try:
        registry = _native.Registry.open_or_create(args.namespace)
    except RuntimeError as e:
        raise SystemExit(
            f"error: no registry for namespace '{args.namespace}': {e}") from e

    for t in registry.list_topics():
        if t.topic_name == topic:
            return t.shm_name

    raise SystemExit(
        f"error: topic '{topic}' not found in namespace '{args.namespace}' "
        f"registry.  The region may not be published yet, or you may need "
        f"--shm for a region whose creator isn't registered.")


# ----------------------------------------------------------------------
# Output helpers
# ----------------------------------------------------------------------


def _json_default(v):
    if isinstance(v, bytes):
        return v.hex()
    return str(v)


def _to_json(obj) -> str:
    def _normalize(x):
        if hasattr(x, "__dataclass_fields__"):
            return asdict(x)
        if isinstance(x, list):
            return [_normalize(y) for y in x]
        if isinstance(x, dict):
            return {k: _normalize(v) for k, v in x.items()}
        return x
    return json.dumps(_normalize(obj), indent=2, default=_json_default)


def _humanize_age(seconds: float | None) -> str:
    if seconds is None:
        return "-"
    s = int(seconds)
    if s < 60:
        return f"{s}s"
    if s < 3600:
        return f"{s // 60}m{s % 60:02d}s"
    return f"{s // 3600}h{(s % 3600) // 60:02d}m"


def _hex_or_dash(b: bytes | None) -> str:
    if b is None:
        return "-"
    return b.hex()


# ----------------------------------------------------------------------
# Subcommand: list
# ----------------------------------------------------------------------


def _col_topic(t):         return t.topic_name
def _col_shm(t):           return t.shm_name
def _col_kind(t):          return t.kind
def _col_namespace(t):     return t.kmsg_namespace
def _col_producers(t):     return str(len(t.producers))
def _col_consumers(t):     return str(len(t.consumers))
def _col_producer_pids(t):
    joined = ",".join(str(p.pid) for p in t.producers)
    if not joined:
        return "-"
    return joined
def _col_consumer_pids(t):
    joined = ",".join(str(p.pid) for p in t.consumers)
    if not joined:
        return "-"
    return joined
def _col_producer_names(t):
    joined = ",".join(p.node_name for p in t.producers)
    if not joined:
        return "-"
    return joined
def _col_consumer_names(t):
    joined = ",".join(p.node_name for p in t.consumers)
    if not joined:
        return "-"
    return joined
def _col_stall(t):         return str(len(t.stall_producers) + len(t.stall_consumers))
def _col_stall_pub(t):     return str(len(t.stall_producers))
def _col_stall_sub(t):     return str(len(t.stall_consumers))
def _col_schema(t):
    if t.schema_name is None:
        return "-"
    return t.schema_name
def _col_ver(t):
    if t.schema_version is None:
        return "-"
    return str(t.schema_version)
def _col_age(t):           return _humanize_age(t.age_seconds)


_LIST_COLUMNS = {
    "topic":           ("TOPIC",        _col_topic),
    "shm":             ("SHM",          _col_shm),
    "ns":              ("NS",           _col_namespace),
    "namespace":       ("NS",           _col_namespace),
    "kind":            ("KIND",         _col_kind),
    "producers":       ("PUB",          _col_producers),
    "pub":             ("PUB",          _col_producers),
    "producers_pid":   ("PUB_PID",      _col_producer_pids),
    "pub_pid":         ("PUB_PID",      _col_producer_pids),
    "producers_names": ("PUB_NODES",    _col_producer_names),
    "pub_names":       ("PUB_NODES",    _col_producer_names),
    "consumers":       ("SUB",          _col_consumers),
    "sub":             ("SUB",          _col_consumers),
    "consumers_pid":   ("SUB_PID",      _col_consumer_pids),
    "sub_pid":         ("SUB_PID",      _col_consumer_pids),
    "consumers_names": ("SUB_NODES",    _col_consumer_names),
    "sub_names":       ("SUB_NODES",    _col_consumer_names),
    "age":             ("AGE",          _col_age),
    "stall_pub":       ("STALL_PUB",    _col_stall_pub),
    "stall_sub":       ("STALL_SUB",    _col_stall_sub),
    "stall":           ("STALL",        _col_stall),
    "schema":          ("SCHEMA",       _col_schema),
    "ver":             ("VER",          _col_ver),
}

_LIST_DEFAULT_COLS = ["topic", "ns", "kind", "pub", "sub", "age", "stall"]
_LIST_ALL_COLS = [
    "topic", "ns", "kind",
    "pub", "pub_pid", "pub_names",
    "sub", "sub_pid", "sub_names",
    "age", "stall_pub", "stall_sub", "stall",
    "schema", "ver", "shm",
]


def _column_width(header: str, rows: Sequence[Sequence[str]], col: int) -> int:
    width = len(header)
    for r in rows:
        if len(r[col]) > width:
            width = len(r[col])
    return width


def _render_table(headers: Sequence[str], rows: Sequence[Sequence[str]]) -> str:
    widths = [_column_width(h, rows, i) for i, h in enumerate(headers)]
    lines = ["  ".join(h.ljust(widths[i]) for i, h in enumerate(headers))]
    for r in rows:
        lines.append("  ".join(c.ljust(widths[i]) for i, c in enumerate(r)))
    return "\n".join(lines)


def _parse_columns(spec: str | None) -> list[str]:
    if spec == "all":
        return list(_LIST_ALL_COLS)
    if not spec:
        return list(_LIST_DEFAULT_COLS)
    return [c.strip() for c in spec.split(",")]


def cmd_list(args) -> int:
    topics = diag.list_topics(kmsg_namespace=args.namespace)

    if args.json:
        print(_to_json(topics))
        return 0

    cols = _parse_columns(args.columns)
    bad = [c for c in cols if c not in _LIST_COLUMNS]
    if bad:
        print(f"unknown columns: {', '.join(bad)}", file=sys.stderr)
        print(f"available: {', '.join(sorted(_LIST_COLUMNS))}", file=sys.stderr)
        return 2

    headers = [_LIST_COLUMNS[c][0] for c in cols]
    rows = [[_LIST_COLUMNS[c][1](t) for c in cols] for t in topics]
    print(_render_table(headers, rows))
    if not topics:
        # Friendly hint but no error — empty output is a valid state
        # (no participants yet, or all cleanly deregistered).
        print(f"\n(no participants in namespace '{args.namespace}')",
              file=sys.stderr)
    return 0


# ----------------------------------------------------------------------
# Subcommand: info
# ----------------------------------------------------------------------


def cmd_info(args) -> int:
    i = diag.info(_resolve_shm_name(args))
    if args.json:
        print(_to_json(i))
        return 0
    print(f"region:       {i.shm_name}")
    print(f"type:         {i.channel_type}")
    s = i.schema
    if s.state == "set":
        print(f"schema:       name={s.name!r}, version={s.version}")
        print(f"  identity:   {_hex_or_dash(s.identity)}")
        print(f"  layout:     {_hex_or_dash(s.layout)}")
    elif s.state == "claiming":
        print("schema:       claim in progress (possibly wedged — see `kickmsg diagnose`)")
    else:
        print("schema:       unset")
    return 0


# ----------------------------------------------------------------------
# Subcommand: stats
# ----------------------------------------------------------------------


def cmd_stats(args) -> int:
    s = diag.stats(_resolve_shm_name(args))
    if args.json:
        print(_to_json(s))
        return 0
    print(f"region:       {s.shm_name}")
    print(f"subscribers:  {s.live_rings} live")
    print(f"pool:         {s.pool_free} / {s.pool_size} free")
    print()
    headers = ["ring", "state", "in_flight", "write_pos", "dropped", "lost"]
    rows = [
        [
            str(r.index), r.state, str(r.in_flight), str(r.write_pos),
            str(r.dropped_count), str(r.lost_count),
        ]
        for r in s.rings
    ]
    print(_render_table(headers, rows))
    print()
    print(f"total writes: {s.total_writes}")
    print(f"total drops:  {s.total_drops}")
    print(f"total losses: {s.total_losses}")
    return 0


# ----------------------------------------------------------------------
# Subcommand: diagnose
# ----------------------------------------------------------------------


def cmd_diagnose(args) -> int:
    h = diag.diagnose(_resolve_shm_name(args))
    if args.json:
        print(_to_json(h))
        return 0
    print(f"region:          {h.shm_name}")
    print(f"locked_entries:  {h.locked_entries}")
    print(f"retired_rings:   {h.retired_rings}")
    print(f"draining_rings:  {h.draining_rings}")
    print(f"live_rings:      {h.live_rings}")
    print(f"schema_stuck:    {h.schema_stuck}")
    print()
    print(f"status:          {h.status}")
    if h.status == "crash residue":
        flags = []
        if h.locked_entries:
            flags.append("--locked")
        if h.retired_rings:
            flags.append("--retired")
        print(f"suggested:       kickmsg repair {h.shm_name} {' '.join(flags)}")
    if h.status == "healthy":
        return 0
    return 1


# ----------------------------------------------------------------------
# Subcommand: repair
# ----------------------------------------------------------------------


def cmd_repair(args) -> int:
    target = _resolve_shm_name(args)
    if not (args.locked or args.retired or args.reclaim):
        args.locked = True  # default: safe operation only

    if args.locked:
        n = diag.repair_locked(target)
        print(f"--locked:   repaired {n} entries")
    if args.retired:
        n = diag.reset_retired(target)
        print(f"--retired:  reset {n} rings")
    if args.reclaim:
        n = diag.reclaim_orphaned(target)
        print(f"--reclaim:  reclaimed {n} slots")
    return 0


# ----------------------------------------------------------------------
# Subcommand: watch
# ----------------------------------------------------------------------


def _format_rate(rate: float) -> str:
    return f"{rate:.0f} msg/s"


def cmd_watch(args) -> int:
    target = _resolve_shm_name(args)
    is_tty = sys.stdout.isatty()
    try:
        for frame in diag.watch(target, interval=args.interval):
            if is_tty:
                sys.stdout.write("\033[2J\033[H")  # clear screen + home
            else:
                sys.stdout.write("\n--- frame ---\n")
            print(f"{frame.stats.shm_name}    live: {frame.stats.live_rings}    "
                  f"pool: {frame.stats.pool_free}/{frame.stats.pool_size}")
            print()
            headers = ["ring", "state", "in_flight", "write_pos", "dropped", "lost", "rate"]
            rows = [
                [
                    str(r.index), r.state, str(r.in_flight), str(r.write_pos),
                    str(r.dropped_count), str(r.lost_count),
                    _format_rate(rate),
                ]
                for r, rate in zip(frame.stats.rings, frame.rates_msg_per_sec)
            ]
            print(_render_table(headers, rows))
            sys.stdout.flush()
    except KeyboardInterrupt:
        pass
    return 0


# ----------------------------------------------------------------------
# Subcommand: schema / schema-diff
# ----------------------------------------------------------------------


def cmd_schema(args) -> int:
    s = diag.schema(_resolve_shm_name(args))
    if args.json:
        print(_to_json(s))
        return 0
    if s.state == "unset":
        print("unset (no schema published)")
        return 1
    if s.state == "claiming":
        print("claiming (claim in progress — possibly wedged)")
        return 1
    print(f"name:          {s.name}")
    print(f"version:       {s.version}")
    print(f"identity:      {_hex_or_dash(s.identity)}")
    print(f"layout:        {_hex_or_dash(s.layout)}")
    print(f"identity_algo: {s.identity_algo}")
    print(f"layout_algo:   {s.layout_algo}")
    print(f"flags:         {s.flags}")
    return 0


def cmd_schema_diff(args) -> int:
    d = diag.schema_diff(args.shm_a, args.shm_b)
    if args.json:
        print(_to_json(d))
        if d.equal:
            return 0
        return 1
    for field_name in ("identity", "layout", "version", "name", "identity_algo", "layout_algo"):
        differs = getattr(d, field_name)
        if differs:
            status = "differ"
        else:
            status = "match"
        print(f"{field_name:14} {status}")
    if d.equal:
        return 0
    return 1


# ----------------------------------------------------------------------
# Entry point
# ----------------------------------------------------------------------


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(prog="kickmsg",
                                description="Inspect running kickmsg channels.")
    sub = p.add_subparsers(dest="cmd", required=True)

    sp = sub.add_parser("list", aliases=["ls"],
                        help="List running topics (registry-backed)")
    sp.add_argument("-n", "--namespace", default=_default_namespace(),
                    help=f"Namespace to inspect. Defaults to ${_ENV_VAR} if "
                         f"set, else \"kickmsg\".")
    sp.add_argument("-o", "--columns", default=None,
                    help="Comma-separated column list, or 'all'. "
                    f"Default: {','.join(_LIST_DEFAULT_COLS)}")
    sp.add_argument("--json", action="store_true")
    sp.set_defaults(func=cmd_list)

    sp = sub.add_parser("info", help="Static header metadata for one region")
    _add_region_target(sp)
    sp.add_argument("--json", action="store_true")
    sp.set_defaults(func=cmd_info)

    sp = sub.add_parser("stats", help="Runtime counter snapshot")
    _add_region_target(sp)
    sp.add_argument("--json", action="store_true")
    sp.set_defaults(func=cmd_stats)

    sp = sub.add_parser("diagnose", aliases=["diag"],
                        help="Health check")
    _add_region_target(sp)
    sp.add_argument("--json", action="store_true")
    sp.set_defaults(func=cmd_diagnose)

    sp = sub.add_parser("repair", help="Run repair primitives")
    _add_region_target(sp)
    sp.add_argument("--locked",  action="store_true",
                    help="repair_locked_entries() — safe under live traffic")
    sp.add_argument("--retired", action="store_true",
                    help="reset_retired_rings() — only after crashed publisher confirmed gone")
    sp.add_argument("--reclaim", action="store_true",
                    help="reclaim_orphaned_slots() — requires full quiescence")
    sp.set_defaults(func=cmd_repair)

    sp = sub.add_parser("watch", help="top-like live stats view")
    _add_region_target(sp)
    sp.add_argument("-i", "--interval", type=float, default=1.0)
    sp.set_defaults(func=cmd_watch)

    sp = sub.add_parser("schema", help="Inspect the schema descriptor")
    _add_region_target(sp)
    sp.add_argument("--json", action="store_true")
    sp.set_defaults(func=cmd_schema)

    sp = sub.add_parser("schema-diff", aliases=["sdiff"],
                        help="Compare schemas across two regions")
    sp.add_argument("shm_a", type=_normalize_shm_name,
                    help="SHM name of region A (leading '/' optional)")
    sp.add_argument("shm_b", type=_normalize_shm_name,
                    help="SHM name of region B (leading '/' optional)")
    sp.add_argument("--json", action="store_true")
    sp.set_defaults(func=cmd_schema_diff)

    return p


def main(argv: Sequence[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
