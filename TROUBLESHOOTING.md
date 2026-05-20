# Troubleshooting

Common gotchas when running kickmsg on a real system. Most of these are
properties of the underlying OS (POSIX shared memory, Win32 file
mappings) rather than of kickmsg itself.

For internals (concurrency model, crash-resilience invariants), see
[ARCHITECTURE.md](ARCHITECTURE.md).

## Stale segments after a crash

A shared-memory region outlives the processes that created it. If a
publisher or subscriber crashes, the region persists until something
explicitly removes it.

**See what's still around:**

```bash
kickmsg list                 # all topics in the default namespace
kickmsg list -n myapp        # different namespace
ls /dev/shm                  # Linux only — macOS/Windows aren't filesystem-visible
```

**Remove it.**  Pick whichever fits the situation:

- From code, when you own the region's lifetime:

  ```cpp
  region.unlink();  // calls shm_unlink (POSIX) or just drops the handle (Windows)
  ```

- From the shell on Linux:

  ```bash
  rm /dev/shm/kickmsg_telemetry   # the leading '/' becomes a flat file name
  ```

- On macOS / Windows: open the region and call `unlink()` from a small
  helper. Listing/unlinking outside the process isn't possible because
  the names aren't filesystem-visible.

**Why this isn't automatic:** kickmsg deliberately does *not*
`shm_unlink` on destruction — that would yank the region out from under
any peer still holding it open. Removal is an operator decision.

## "My channel looks stuck"

If publishers seem to succeed but subscribers receive nothing — or new
subscribers can't attach — a previous crash probably left ring entries
or rings in a transitional state. Use the diagnose / repair flow.

```bash
kickmsg diagnose /myapp_telemetry
```

The output names what to do next:

| Field             | Meaning                                              | Action                                              |
|-------------------|------------------------------------------------------|-----------------------------------------------------|
| `locked_entries`  | Publisher crashed mid-commit                         | `kickmsg repair --locked` (safe under live traffic) |
| `retired_rings`   | Ring stuck Free with `in_flight > 0`                 | `kickmsg repair --retired` *after* confirming the crashed publisher is gone |
| `draining_rings`  | Subscriber tearing down — usually transient          | Wait. Persistent counts may indicate a stuck teardown. |
| `schema_stuck`    | Schema claimant crashed mid-write                    | `region.reset_schema_claim()` *after* confirming the claimant is gone |
| `live_rings`      | Active subscribers                                   | Informational.                                      |

`kickmsg repair` also accepts `--reclaim` for `reclaim_orphaned_slots()` —
leaked slots from publisher crashes between `allocate` and `publish`. It
requires full quiescence (no live publishers, no outstanding `SampleView`)
and is gated by a confirmation prompt; pass `-y` to skip.

The same three primitives are exposed in C++:

```cpp
region.repair_locked_entries();   // safe under live traffic, idempotent
region.reset_retired_rings();     // post-crash only, NOT safe under live traffic
region.reclaim_orphaned_slots();  // requires full quiescence; reclaims leaked slots
```

The safety asymmetry matters: `repair_locked_entries` can be wired into
a periodic health check; the other two are operator-driven actions that
require ruling out concurrent live writers / outstanding `SampleView`s.

## SHM name errors

### `kickmsg::sanitize_shm_component: ... name is empty after sanitization`

The namespace, topic, channel, owner, or tag you passed is blank — or
becomes blank after stripping leading slashes (e.g. `""`, `"/"`,
`"///"`). Pass a non-empty component.

### What characters are allowed

kickmsg sanitizes user-supplied components to POSIX-portable form:

- Leading `/` is stripped (so ROS-style `/robot/arm` is accepted).
- Interior `/` becomes `.` to preserve hierarchy visually.
- `[A-Za-z0-9._-]` passes through.
- Everything else becomes `_`.

So `imu/raw` and `imu.raw` end up at the same SHM region — this is
deliberate. Pick one form per project and stick with it.

### `ENAMETOOLONG` / `EINVAL` on macOS

Darwin's POSIX SHM has a hard 31-byte limit on the full name including
the leading `/` and the null terminator — about **29 visible
characters** for `prefix + "_" + topic`. Long namespaces + long topics
will silently work on Linux and explode on macOS.

Keep names short, or shorten the namespace prefix in `Node(name,
prefix)`.

## Permission errors

### `EACCES` opening `/dev/shm/...` on Linux

POSIX SHM segments inherit the creator's UID and a mode derived from
the creator's umask (kickmsg requests `0666`, masked by umask).
Common causes:

- A daemon created the segment as root; an unprivileged client can't
  open it. Run the client with matching UID, or have the creator
  loosen its umask before constructing the `SharedRegion`.
- Container-vs-host UID drift. The segment was created by UID 1000
  inside a container; on the host UID 1000 is somebody else.

### `ENOSPC` / "No space left on device" on Linux

`/dev/shm` is a tmpfs with a fixed size cap (often half of RAM by
default). Large `pool_size * max_payload_size` channels can exhaust it.
Inspect with `df -h /dev/shm`. Remount larger if needed:

```bash
mount -o remount,size=2G /dev/shm
```

## Platform notes

### Linux

`/dev/shm` is filesystem-visible, so `ls /dev/shm`, `rm /dev/shm/...`,
and `lsof | grep /dev/shm` all work. The Registry-backed `kickmsg
list` works identically — and is the only option on the other two
targets.

### macOS

- POSIX SHM names are capped at ~29 visible chars (see above).
- The Darwin futex backend uses private `__ulock_wait` / `__ulock_wake`
  APIs. ABI has been stable since 10.12 but they are not in any public
  header — TSAN / sanitizers may flag these calls.
- `/dev/shm` does not exist; segments are managed entirely by the
  kernel and are not filesystem-visible.

### Windows

- kickmsg uses `CreateFileMappingA` with the kickmsg name passed
  unmodified — no `Global\` prefix is prepended. This means **regions
  live in the calling session's namespace**: a publisher in user
  session 1 and a subscriber in session 2 (or one of them as a Windows
  service) will *not* see each other.
- Cross-session IPC requires prefixing your topic / namespace with
  `Global\`, which in turn requires the `SeCreateGlobalPrivilege`
  privilege (granted to admins and to `LocalSystem` services). This is
  a Windows policy, not a kickmsg limitation.
- Mapping handles are reference-counted by the kernel: a region is
  released when the last process closes its handle. There is no
  equivalent of `shm_unlink` — `region.unlink()` is a no-op on Windows.

## Still stuck?

Open an issue at <https://github.com/leducp/kickmsg/issues> with the
output of:

```bash
kickmsg list --json
kickmsg diagnose <shm> --json
uname -a            # or `ver` on Windows
```
