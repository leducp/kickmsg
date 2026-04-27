"""Kickmsg — lock-free shared-memory IPC."""

# Explicit re-exports from the native bindings.  Using an enumerated
# list (rather than `from ._native import *`) makes the public API
# visible here and avoids future name collisions if we add pure-Python
# symbols that share a name with a native one.
from ._native import (
    AllocatedSlot,
    BroadcastHandle,
    ChannelType,
    Config,
    HealthReport,
    Kind,
    Node,
    Participant,
    Publisher,
    RegionStats,
    Registry,
    RingStats,
    Role,
    SampleView,
    SchemaInfo,
    SharedRegion,
    Subscriber,
    TopicSummary,
    current_pid,
    hash,
    process_exists,
    schema,
    unlink_shm,
)

# Diagnostics submodule (typed dataclass API for CLI + GUI).
from . import diagnostics

__all__ = [
    "AllocatedSlot",
    "BroadcastHandle",
    "ChannelType",
    "Config",
    "HealthReport",
    "Kind",
    "Node",
    "Participant",
    "Publisher",
    "RegionStats",
    "Registry",
    "RingStats",
    "Role",
    "SampleView",
    "SchemaInfo",
    "SharedRegion",
    "Subscriber",
    "TopicSummary",
    "current_pid",
    "diagnostics",
    "hash",
    "process_exists",
    "schema",
    "unlink_shm",
]
