# Testing kickmsg

How to prove kickmsg works on *your* hardware. The headline is the **soak**
(step 4): a long, self-verifying run of the lock-free paths under contention,
ending in a single `VERDICT: ALL CLEAN`. Everything before it is a faster
filter you run first.

kickmsg is lock-free shared-memory IPC, so the bugs that matter are timing- and
ordering-dependent and surface mainly under real contention on real hardware --
especially weakly-ordered CPUs (ARM/Apple Silicon) that x86 hides. That is what
these tests exercise.

## The suites

| Binary | Covers |
|--------|--------|
| `kickmsg_unit` | Unit + ABI/layout asserts, schema protocol, geometry validation, recovery primitives. Deterministic. |
| `kickmsg_stress_test` | Lock-free contention: MPMC publish/receive, Treiber free-stack, pool exhaustion, churn, fairness, zero-copy. |
| `kickmsg_crash_test` | Crash recovery: fork a participant, SIGKILL mid-operation, verify the channel self-heals (POSIX only). |
| `kickmsg_registry_stress_test` | Registry under concurrent register/deregister + dead-PID sweep (POSIX only). |

## Prerequisites

- A C++17 compiler (gcc, clang, or apple-clang), CMake >= 3.16.
- Conan 2.x for the test dependencies (GoogleTest, argparse). Use a venv:
  ```bash
  python3 -m venv .venv && source .venv/bin/activate && pip install conan
  ```

## Build

```bash
source .venv/bin/activate                  # conan on PATH
scripts/configure.sh build --with=unit_tests
scripts/setup_build.sh build               # conan install + CMake (detects your toolchain)
cmake --build build -j
```

## The validation ladder

Run top to bottom; stop at the depth you need. Each rung is strictly more
thorough (and slower) than the last.

### 1. Quick gate -- seconds, run on every change
```bash
ctest --test-dir build --output-on-failure
```
Runs unit + crash + registry + a bounded stress pass. Expect:
```
100% tests passed, 0 tests failed out of 4
```

### 2. Full local pass -- minutes, before a PR / after touching lock-free or platform code
```bash
scripts/validate.sh
```
Builds fresh, then runs the unit suite, the stress suite x10 (intermittent
ARM ordering bugs do not show in a single pass), and the crash tests. Ends with
`macOS validation complete` / clean.

### 3. Soak -- hours, the "it works on my hardware" proof
Loops a suite for a wall-clock duration and prints a single verdict.
```bash
# Steady-state lock-free correctness:
tests/endurance.sh build/kickmsg_stress_test 1800       # 30 min
# Crash recovery, hammered:
tests/endurance.sh build/kickmsg_crash_test  1800
# Heavier contention (see "Contention" below):
tests/endurance.sh build/kickmsg_stress_test 1800 --oversub 400
```
A clean run ends with:
```
=== FINAL RESULTS ===
Runs: 110   Scenarios passed: 1650   Scenarios failed: 0   Total reorders: 0
VERDICT: ALL CLEAN
```
`fail` and `reorders` must both be 0. A nonzero exit code (and
`VERDICT: FAILURES DETECTED`) means a real problem -- capture the log.

### 4. Race detection -- highest value per hour
A clean Release soak only fails if a race *manifests* as corruption;
ThreadSanitizer flags the race even when it does not. A few hours of TSAN
endurance is worth more than a long Release soak for finding ordering bugs.
```bash
scripts/configure.sh build_tsan --with=unit_tests --with=tsan
scripts/setup_build.sh build_tsan && cmake --build build_tsan -j
TSAN_OPTIONS="suppressions=$PWD/tests/tsan.supp" \
  tests/endurance.sh build_tsan/kickmsg_stress_test 14400
```

## Contention scales to your machine

The stress suite sizes its thread counts to the host CPU rather than a fixed
number, so it stays a real contention test on a 192-core box and stays bounded
on a 2-core CI runner. The default targets ~1.5x cores total; tune it:
```bash
build/kickmsg_stress_test --oversub 400   # ~4x cores: heavy
build/kickmsg_stress_test --oversub 50    # light
build/kickmsg_stress_test --help
```
Each run prints what it resolved to, e.g. `contention: 150% of 24 cores -> 18 threads/side`.

Caveat: thread sizing uses `std::thread::hardware_concurrency()`, which reports
the host's cores and does **not** see cgroup/container CPU quotas (Docker
`--cpus`, k8s limits). Inside a CPU-throttled container, pass `--oversub` to
bound it explicitly.

## Detached long soak (survives logout; keeps the machine awake)

```bash
# macOS: caffeinate prevents idle sleep mid-soak
nohup caffeinate -i tests/endurance.sh build/kickmsg_stress_test 43200 > soak.log 2>&1 &
disown
# Linux: drop `caffeinate -i`
tail -f soak.log
grep -E "VERDICT|FAIL|reorders|Runs:" soak.log   # summary without the \r progress noise
```

## Known-good results (receipts)

- Linux x86-64 and Linux ARM64 (Raspberry Pi 4B): 12 h continuous stress, clean.
- Darwin ARM64 (Apple Silicon, 10-core): 12 h continuous stress -- 2660 runs,
  39900 scenarios, 0 failures, 0 reorders.
- TSAN: unit + stress + crash clean.

For *why* the recovery and lock-free design is correct (and its documented
limits), see [../ARCHITECTURE.md](../ARCHITECTURE.md).
