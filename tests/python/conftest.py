"""Pytest fixtures for Kickmsg Python tests.

Each test gets a unique SHM name and guaranteed cleanup before + after,
so a crashed prior run can't leave stale state that poisons the next one.
"""

from __future__ import annotations

import os

import pytest

import kickmsg


@pytest.fixture
def shm_name(request: pytest.FixtureRequest) -> str:
    """Unique per-test SHM name, auto-cleaned before and after.

    macOS caps shm names at roughly 31 chars including the leading `/`,
    so we truncate the *test slug* while keeping the PID suffix intact —
    otherwise parallel pytest-xdist workers with overlapping slugs would
    collide on the same shared-memory object.
    """
    slug = request.node.nodeid.replace("/", "_").replace("::", "_").replace(":", "_")
    pid = os.getpid()
    pid_suffix = f"_{pid}"
    max_slug = 30 - len("/pytest_") - len(pid_suffix)
    name = f"/pytest_{slug[:max_slug]}{pid_suffix}"
    kickmsg.unlink_shm(name)
    yield name
    kickmsg.unlink_shm(name)


@pytest.fixture
def small_cfg() -> kickmsg.Config:
    cfg = kickmsg.Config()
    cfg.max_subscribers = 4
    cfg.sub_ring_capacity = 8
    cfg.pool_size = 16
    cfg.max_payload_size = 256
    return cfg
