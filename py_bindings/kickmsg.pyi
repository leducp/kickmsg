"""Kickmsg — lock-free shared-memory IPC"""

import datetime
import enum

from . import hash as hash, schema as schema


class ChannelType(enum.Enum):
    PubSub = 1

    Broadcast = 2

class Config:
    def __init__(self) -> None: ...

    @property
    def max_subscribers(self) -> int: ...

    @max_subscribers.setter
    def max_subscribers(self, arg: int, /) -> None: ...

    @property
    def sub_ring_capacity(self) -> int: ...

    @sub_ring_capacity.setter
    def sub_ring_capacity(self, arg: int, /) -> None: ...

    @property
    def pool_size(self) -> int: ...

    @pool_size.setter
    def pool_size(self, arg: int, /) -> None: ...

    @property
    def max_payload_size(self) -> int: ...

    @max_payload_size.setter
    def max_payload_size(self, arg: int, /) -> None: ...

    @property
    def commit_timeout(self) -> datetime.timedelta:
        """Commit timeout as a timedelta (microsecond resolution)."""

    @commit_timeout.setter
    def commit_timeout(self, arg: datetime.timedelta | float, /) -> None: ...

    @property
    def schema(self) -> SchemaInfo | None: ...

    @schema.setter
    def schema(self, arg: SchemaInfo | None) -> None: ...

    def __repr__(self) -> str: ...

class SchemaInfo:
    def __init__(self) -> None: ...

    @property
    def identity(self) -> bytes: ...

    @identity.setter
    def identity(self, arg: bytes, /) -> None: ...

    @property
    def layout(self) -> bytes: ...

    @layout.setter
    def layout(self, arg: bytes, /) -> None: ...

    @property
    def name(self) -> str: ...

    @name.setter
    def name(self, arg: str, /) -> None: ...

    @property
    def version(self) -> int: ...

    @version.setter
    def version(self, arg: int, /) -> None: ...

    @property
    def identity_algo(self) -> int: ...

    @identity_algo.setter
    def identity_algo(self, arg: int, /) -> None: ...

    @property
    def layout_algo(self) -> int: ...

    @layout_algo.setter
    def layout_algo(self, arg: int, /) -> None: ...

    @property
    def flags(self) -> int: ...

    @flags.setter
    def flags(self, arg: int, /) -> None: ...

    def __repr__(self) -> str: ...

class HealthReport:
    @property
    def locked_entries(self) -> int: ...

    @property
    def retired_rings(self) -> int: ...

    @property
    def draining_rings(self) -> int: ...

    @property
    def live_rings(self) -> int: ...

    @property
    def schema_stuck(self) -> bool: ...

    def __repr__(self) -> str: ...

class SharedRegion:
    @staticmethod
    def create(name: str, type: ChannelType, cfg: Config, creator: str = '') -> SharedRegion: ...

    @staticmethod
    def open(name: str) -> SharedRegion: ...

    @staticmethod
    def create_or_open(name: str, type: ChannelType, cfg: Config, creator: str = '') -> SharedRegion: ...

    @property
    def name(self) -> str: ...

    @property
    def channel_type(self) -> ChannelType: ...

    def schema(self) -> SchemaInfo | None: ...

    def try_claim_schema(self, info: SchemaInfo) -> bool: ...

    def reset_schema_claim(self) -> bool: ...

    def diagnose(self) -> HealthReport: ...

    def repair_locked_entries(self) -> int: ...

    def reset_retired_rings(self) -> int: ...

    def reclaim_orphaned_slots(self) -> int: ...

    def unlink(self) -> None: ...

    def __repr__(self) -> str: ...

def unlink_shm(name: str) -> None:
    """Unlink a shared-memory entry by name (no-op if absent)."""

class SampleView:
    def __buffer__(self, flags, /):
        """
        Return a buffer object that exposes the underlying memory of the object.
        """

    def __release_buffer__(self, buffer, /):
        """
        Release the buffer object that exposes the underlying memory of the object.
        """

    def __len__(self) -> int: ...

    @property
    def ring_pos(self) -> int: ...

    @property
    def valid(self) -> bool: ...

    def release(self) -> None:
        """
        Release the slot pin early.  Idempotent; after this, any NEW memoryview(view) call raises BufferError.  Memoryviews obtained before .release() remain valid as pointers but should not be used (the pin is gone).
        """

    def __enter__(self) -> SampleView: ...

    def __exit__(self, *args) -> None: ...

    def __repr__(self) -> str: ...

class AllocatedSlot:
    def __buffer__(self, flags, /):
        """
        Return a buffer object that exposes the underlying memory of the object.
        """

    def __release_buffer__(self, buffer, /):
        """
        Release the buffer object that exposes the underlying memory of the object.
        """

    def publish(self) -> int:
        """
        Commit the reserved slot.  Returns the number of rings the sample was delivered to.  After this call, any NEW memoryview(slot) fails with BufferError.
        """

    def __len__(self) -> int: ...

    @property
    def published(self) -> bool: ...

    def __repr__(self) -> str: ...

class Publisher:
    def __init__(self, region: SharedRegion) -> None: ...

    def send(self, data: bytes) -> int:
        """
        Copy `data` into a slot and publish (atomic convenience).  Returns the number of bytes written.  Raises ValueError if the message exceeds max_payload_size, BlockingIOError if the slot pool is exhausted, OSError on other failures.
        """

    def allocate(self, len: int) -> AllocatedSlot | None:
        """
        Reserve a slot of `len` bytes and return an AllocatedSlot.  Use memoryview(slot) or numpy.asarray(slot) to fill it in place (zero-copy), then call slot.publish().  Returns None if the pool is exhausted.
        """

    @property
    def dropped(self) -> int:
        """Per-ring delivery drops (CAS contention or pool exhaustion)."""

    def __repr__(self) -> str: ...

class Subscriber:
    def __init__(self, region: SharedRegion) -> None: ...

    def try_receive(self) -> object:
        """
        Non-blocking receive.  Returns bytes on success, None if no message is available.
        """

    def receive(self, timeout: datetime.timedelta | float) -> object:
        """
        Blocking receive with timeout (timedelta).  Releases the GIL while waiting.  Returns bytes on success, None on timeout.
        """

    def try_receive_view(self) -> SampleView | None:
        """
        Non-blocking zero-copy receive.  Returns a SampleView (pins the slot) or None.
        """

    def receive_view(self, timeout: datetime.timedelta | float) -> SampleView | None:
        """
        Blocking zero-copy receive (timedelta timeout).  Releases the GIL.  Returns a SampleView or None on timeout.
        """

    @property
    def lost(self) -> int:
        """
        Messages the subscriber's ring overflowed past — the publisher evicted them before this subscriber drained.
        """

    @property
    def drain_timeouts(self) -> int:
        """
        Count of times the subscriber gave up waiting for an in-flight publisher during teardown.
        """

    def __repr__(self) -> str: ...

    def __iter__(self) -> Subscriber: ...

    def __next__(self) -> bytes: ...

class BroadcastHandle:
    @property
    def pub(self) -> Publisher: ...

    @property
    def sub(self) -> Subscriber: ...

    def __repr__(self) -> str: ...

class Node:
    def __init__(self, name: str, prefix: str = 'kickmsg') -> None: ...

    def advertise(self, topic: str, cfg: Config = ...) -> Publisher: ...

    def subscribe(self, topic: str) -> Subscriber: ...

    def advertise_or_join(self, topic: str, cfg: Config) -> Publisher: ...

    def subscribe_or_create(self, topic: str, cfg: Config) -> Subscriber: ...

    def join_broadcast(self, channel: str, cfg: Config = ...) -> BroadcastHandle: ...

    def create_mailbox(self, tag: str, cfg: Config = ...) -> Subscriber: ...

    def open_mailbox(self, owner_node: str, tag: str) -> Publisher: ...

    def unlink_topic(self, topic: str) -> None: ...

    def unlink_broadcast(self, channel: str) -> None: ...

    def unlink_mailbox(self, tag: str, owner_node: str | None = None) -> None: ...

    def topic_schema(self, topic: str) -> SchemaInfo | None: ...

    def try_claim_topic_schema(self, topic: str, info: SchemaInfo) -> bool: ...

    @property
    def name(self) -> str: ...

    @property
    def prefix(self) -> str: ...

    def __repr__(self) -> str: ...
