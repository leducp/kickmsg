# Kickmsg

Lock-free shared-memory messaging library for inter-process communication.

Kickmsg provides MPMC publish/subscribe over shared memory with zero-copy receive, per-subscriber ring isolation, and crash resilience — all without locks or kernel-mediated synchronization on the hot path.

## Features

- **Lock-free**: all data paths use atomic CAS (Treiber stack, MPSC rings)
- **Zero-copy receive**: `SampleView` pins slots via refcount, avoiding memcpy for large payloads
- **Per-subscriber isolation**: a slow subscriber only overflows its own ring — fast subscribers are unaffected
- **Crash resilient**: publisher crashes never deadlock the channel; bounded slot leaks are recoverable via GC
- **Topic-centric naming**: subscribers connect by topic name, not publisher identity
- **Blackboard**: shared-memory key/value state -- a late reader immediately sees the current value of every key, with its age and its writer's liveness; no heartbeat, no replay
- **C++17**, no external dependencies beyond POSIX / Win32

## Channel Patterns

| Pattern | API | SHM name |
|---------|-----|----------|
| PubSub (1-to-N) | `advertise` / `subscribe` | `/{prefix}_{topic}` |
| Broadcast (N-to-N) | `join_broadcast` | `/{prefix}_broadcast_{channel}` |
| Mailbox (N-to-1) | `create_mailbox` / `open_mailbox` | `/{prefix}_{owner}_mbx_{tag}` |
| Blackboard (state) | `blackboard` / `declare` / `observe` | `/{prefix}_bb_{name}` |

## Installation

For Python (also installs the `kickmsg` CLI):

```bash
pip install kickmsg
```

Pre-built wheels are published for CPython 3.10–3.12 on Linux x86_64 / aarch64
(manylinux_2_28) and macOS 11+ (universal2).  On any other platform `pip` will
fall back to a source build, which needs the [build prerequisites](#prerequisites).

For C++ only, see [Building](#building) or use the Conan recipe in
[`conan/all`](conan/all/conanfile.py).

## Quick Start

```cpp
#include <kickmsg/Publisher.h>
#include <kickmsg/Subscriber.h>

// Create a channel
kickmsg::channel::Config cfg;
cfg.max_subscribers   = 4;
cfg.sub_ring_capacity = 64;
cfg.pool_size         = 256;
cfg.max_payload_size  = 4096;

auto region = kickmsg::SharedRegion::create(
    "/my_topic", kickmsg::channel::PubSub, cfg);

// Subscribe, then publish
kickmsg::Subscriber sub(region);
kickmsg::Publisher  pub(region);

uint32_t value = 42;
pub.send(&value, sizeof(value));

auto sample = sub.try_receive();
// sample->data(), sample->len(), sample->ring_pos()
```

### Node API (topic-centric)

```cpp
#include <kickmsg/Node.h>

kickmsg::Node pub_node("sensor", "myapp");
auto pub = pub_node.advertise("imu");

// Any node can subscribe by topic name alone
kickmsg::Node sub_node("logger", "myapp");
auto sub = sub_node.subscribe("imu");
```

### Zero-copy receive

```cpp
auto view = sub.try_receive_view();
// view->data() points directly into shared memory
// slot is pinned until view is destroyed
```

### Blocking receive

```cpp
auto sample = sub.receive(100ms);
// blocks via futex until data arrives or timeout
```

### Optional payload schema descriptor

```cpp
// Bake a schema descriptor into the region at creation.
kickmsg::SchemaInfo info{};
info.identity = my_identity_hash();   // user-defined bytes
info.layout   = my_layout_hash();     // user-defined bytes
std::snprintf(info.name, sizeof(info.name), "my/Pose");
info.version  = 2;

kickmsg::channel::Config cfg;
cfg.schema = info;
auto region = kickmsg::SharedRegion::create("/pose_topic", kickmsg::channel::PubSub, cfg);

// Any process can read it back and decide what to do on mismatch.
auto schema = region.schema();
if (schema and schema->version != 2) { /* user-defined policy */ }
```

The library stores the descriptor in the header but never interprets it — users
choose how to compute identity/layout fingerprints and how to react to mismatches.

### Blackboard (state, not stream)

A `Subscriber` starts at the ring's current position and sees nothing
published before it attached.  When what you have is *state* -- a lifecycle,
a mode, a calibration -- use a blackboard: the writer publishes once and
stops, and any reader that attaches later sees the current value at once.

```cpp
#include <kickmsg/Node.h>

// Writer: declares the keys it owns, publishes once, stops.
kickmsg::Node  arm("arm_driver", "demo");
auto&          board = arm.blackboard("robot");
auto           state = board.declare("arm/state");   // labelled "arm_driver"
state.write(uint32_t{ACTIVE});

// Reader: constructed AFTER the write, reads it immediately.
kickmsg::Node  hmi("hmi", "demo");
auto           view = hmi.blackboard("robot").observe("arm/state");

uint32_t value = 0;
auto     out   = view.read(value);          // out.status == blackboard::Ok
// out.updated_at_ns -> how old it is;  view.owner_alive() -> is the writer up?

// Block until any key on the board changes, instead of polling.
uint64_t seq = board.change_seq();
if (board.wait(seq, 100ms)) { view.read(value); }
```

One declared writer per key; a key whose owner died keeps its last value and
can be taken over by a restarted writer.  See
[ARCHITECTURE.md](ARCHITECTURE.md#blackboard) for the protocol.

### Health diagnostics and crash recovery

```cpp
// Periodic health check (read-only, safe under live traffic)
auto report = region.diagnose();
// report.locked_entries, report.retired_rings,
// report.draining_rings, report.live_rings

// Repair poisoned entries (safe under live traffic)
region.repair_locked_entries();

// Reset retired rings (after confirming crashed publisher is gone)
region.reset_retired_rings();

// Reclaim leaked slots (requires full quiescence)
region.reclaim_orphaned_slots();
```

## CLI (`kickmsg`)

Installing the Python wheel puts a `kickmsg` command on `$PATH` that
inspects running channels via a shared participant registry (one per
namespace, backed by a SHM region at `/{namespace}_registry`).  Works
identically on Linux, macOS, and Windows — no `/dev/shm` filesystem
walk required.

```bash
kickmsg list                        # topic-centric enumeration
kickmsg list -o name,pub,sub,stall  # ps-style column selection
kickmsg info  <shm>                 # static header metadata
kickmsg stats <shm>                 # runtime counters (write_pos / dropped / lost)
kickmsg watch <shm>                 # top-like live view, msg/s rates (interactive; Ctrl-C to quit)
kickmsg diagnose <shm>              # wraps SharedRegion::diagnose()
kickmsg repair   <shm> [--locked]   # run repair primitives
kickmsg schema <shm>                # focused schema descriptor view
kickmsg schema-diff <a> <b>         # field-by-field schema comparison
kickmsg blackboard <name>           # key table: size, freshness, owner, liveness
kickmsg blackboard-watch <name>     # top-like live view, updates/s per key
```

All subcommands accept `--json` for scripting.

### Programmatic use (GUIs, exporters)

The same data the CLI renders is available as typed dataclasses through
`kickmsg.diagnostics`, so a GUI can consume it without shelling out:

```python
from kickmsg import diagnostics as diag

for topic in diag.list_topics(namespace="kickmsg"):
    print(topic.shm_name, len(topic.producers), len(topic.consumers))

stats = diag.stats("/kickmsg_telemetry")
for ring in stats.rings:
    if ring.state == "live":
        print(ring.write_pos, ring.dropped_count, ring.lost_count)

# Live updates (generator — caller drives the loop)
for frame in diag.watch("/kickmsg_telemetry", interval=1.0):
    gui.update(frame.stats, frame.rates_msg_per_sec)

# Blackboards are addressed by (name, namespace), not by shm path
board = diag.blackboard("robot", kmsg_namespace="demo")
for key in board.keys:
    print(key.key, key.value_len, key.age_seconds, key.owner_alive)
```

## Building

### Prerequisites

- C++17 compiler (GCC 10+, Clang 12+, MSVC 2019+)
- CMake 3.15+
- Conan 2.x (for test/benchmark dependencies)

### Build

The project's own scripts do the dependency install and the CMake configure
together, which is the least error-prone route:

```bash
scripts/configure.sh build --with=unit_tests   # record the option set
scripts/setup_build.sh build                   # conan install + cmake configure
cmake --build build
```

By hand:

```bash
conan install conan/conanfile.py -of=build --build=missing -o unit_tests=True

cmake -S . -B build \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH=build \
    -DBUILD_UNIT_TESTS=ON \
    -DBUILD_EXAMPLES=ON
cmake --build build
```

**Re-run the `cmake -S . -B build` line every time you re-run `conan install`.**
Skipping it reuses the existing CMake cache, which reports the dependency as
found -- `Conan: Component target declared 'GTest::gtest'` -- while dropping
its include paths, so the build fails with `gtest/gtest.h: No such file or
directory` even though Conan just said `Already installed`. Deleting the build
directory has the same effect.

```bash
# Run the whole suite: unit, stress, crash, stall-repair, mp-stress,
# blackboard-crash, registry-stress
ctest --test-dir build --output-on-failure

# Run C++ examples
./build/examples/hello_pubsub
./build/examples/hello_zerocopy
./build/examples/hello_broadcast
./build/examples/hello_diagnose
./build/examples/hello_schema
./build/examples/hello_schema_late_publisher
./build/examples/hello_lowlevel
./build/examples/hello_blackboard

# Run Python examples (after `pip install kickmsg`)
python examples/python/hello_pubsub.py
python examples/python/hello_camera_zerocopy.py    # zero-copy with memoryview
python examples/python/hello_schema.py
python examples/python/hello_blackboard.py
python examples/python/cli_playground.py           # long-running, drive the `kickmsg` CLI against it
```

To prove kickmsg works on your own hardware -- the validation ladder from a
quick `ctest` gate to a multi-hour contention soak with a single
`VERDICT: ALL CLEAN` -- see [tests/README.md](tests/README.md).

### As a subdirectory

```cmake
add_subdirectory(kickmsg)
target_link_libraries(my_app PRIVATE kickmsg)
```

### CMake Options

| Option | Default | Description |
|--------|---------|-------------|
| `BUILD_UNIT_TESTS` | `OFF` | Build unit and stress tests |
| `BUILD_EXAMPLES` | `OFF` | Build example programs |
| `BUILD_BENCHMARKS` | `OFF` | Build benchmarks (requires Google Benchmark) |
| `ENABLE_TSAN` | `OFF` | Enable ThreadSanitizer |

## Security

Shared-memory objects are created with mode `0600` (owner-only) on
Linux and macOS, so channel payloads are not readable by other users
on a multi-user host.  To share channels across users, set the
`KICKMSG_SHM_MODE` environment variable to an octal mode (e.g.
`KICKMSG_SHM_MODE=0666`) in every process that *creates* regions —
openers are unaffected.  The value is parsed once per process; an
invalid value falls back to `0600` with a warning on stderr.

## Platform Support

| Platform | SharedMemory | Futex |
|----------|-------------|-------|
| Linux | `shm_open` / `mmap` | `SYS_futex` |
| macOS | `shm_open` / `mmap` | `__ulock_wait` / `__ulock_wake` |
| Windows | `CreateFileMapping` / `MapViewOfFile` | `WaitOnAddress` / `WakeByAddressAll` |

Actively validated on Linux x86-64, Linux ARM64 (Raspberry Pi 4B, 12 h continuous stress), and Darwin ARM64 (Apple Silicon, 12 h continuous stress: 2660 passes, 0 failures, 0 reorders) via `scripts/validate.sh` and `tests/endurance.sh`.

## Architecture

See [ARCHITECTURE.md](ARCHITECTURE.md) for the full design: shared-memory layout, concurrency model, publish/subscribe flows, crash resilience, garbage collection, and ABA safety analysis.

## Troubleshooting

See [TROUBLESHOOTING.md](TROUBLESHOOTING.md) for common operational gotchas: stale segments after a crash, the diagnose/repair flow, SHM naming and length limits, permission errors, and platform-specific notes (macOS PSHMNAMLEN, Windows session isolation, Linux `/dev/shm` sizing).

## License

[CeCILL-C](LICENSE)
