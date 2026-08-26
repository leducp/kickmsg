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
| `kickmsg_stall_repair_test` | False-positive death: SIGSTOP a live publisher so its entry lock looks stale, steal it with `repair_locked_entries()`, SIGCONT -- every steal must become a clean drop (POSIX only). |
| `kickmsg_mp_stress_test` | Multi-process steady-state MPMC: 4 forked publisher processes x 2 forked subscriber processes over one region (POSIX only). |
| `kickmsg_registry_stress_test` | Registry under concurrent register/deregister + dead-PID sweep (POSIX only). |
| `kickmsg_blackboard_crash_test` | Blackboard crash recovery: SIGKILL a key's owner mid-update and prove its last value is still readable, sweepable, and takeover-able (POSIX only). |

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
Runs unit + crash + stall-repair + mp-stress + registry + blackboard-crash +
a bounded stress pass. Expect:
```
100% tests passed, 0 tests failed out of 7
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
`fail` and `reorders` must both be 0 -- the verdict enforces both, and
also counts a nonzero binary exit as a failure even when the Summary
line itself was clean (teardown crash, exit-time sanitizer report,
watchdog kill). Each run is wrapped in `timeout` (`RUN_TIMEOUT_SECS`,
default 900 s) so a deadlock becomes a recorded failure instead of an
eternal hang. `VERDICT: FAILURES DETECTED` means a real problem --
evidence is preserved under the failure dir with the epoch in the name.

### 4. Race detection -- highest value per hour
A clean Release soak only fails if a race *manifests* as corruption;
ThreadSanitizer flags the race even when it does not. A few hours of TSAN
endurance is worth more than a long Release soak for finding ordering bugs.
```bash
scripts/configure.sh build_tsan --with=unit_tests --with=tsan
scripts/setup_build.sh build_tsan && cmake --build build_tsan -j
TSAN_OPTIONS="suppressions=$PWD/tests/tsan.supp:halt_on_error=1:exitcode=66" \
  tests/endurance.sh build_tsan/kickmsg_stress_test 14400
```
`halt_on_error=1` makes TSAN stop at the first race with the report intact;
without it TSAN reports and *continues*, and the run still exits cleanly. The
endurance harness also greps each run for `ThreadSanitizer`/`runtime error:`
and counts it as a failure (`san=` column), so a race is caught either way.

For memory/UB bugs the same recipe works with ASAN+UBSAN: build with
`scripts/configure.sh build_asan --with=unit_tests --with=asan --with=ubsan`,
then soak `build_asan/kickmsg_stress_test` -- the rig (rung 5) picks it up
automatically as its `asan-150` profile.

Rungs 3-4 above are the **hands-on** soaks: pick one binary and a duration and
hammer it (e.g. crash recovery for an hour before a PR, or TSAN stress for an
afternoon). The rig below is the **unattended** counterpart -- it just cycles
all of them for you. It is built *on top of* `endurance.sh`, not a replacement;
keep using the direct form for targeted runs.

### 5. Unattended long-horizon rig -- cycles all profiles
`tests/soak_all.sh` loops weighted profiles until a wall-clock deadline, each a
time-sliced `endurance.sh` pass: **TSAN takes half the slices** (rarest,
highest-value signal), crash fuzz a quarter, plain stress the rest as periodic
sanity at oversub 150/200. It records a per-slice verdict, persists failing
runs, and survives any single slice failing. It self-detaches (survives
logout), runs under `caffeinate` on macOS, and prints a watch/stop dashboard.
```bash
# build the plain + crash binaries (build/) and optionally build_tsan/
tests/soak_all.sh 604800 1800            # 1 week, 30-min slices; returns at once
# absent build_tsan/ is skipped, not fatal. SOAK_FOREGROUND=1 to run inline.
```
Each launch isolates its run under `soak_logs/run_<timestamp>/` (also
`soak_logs/latest`) with the master log, per-slice logs, and `fails/`. Check
progress at any time with:
```bash
tests/soak_status.sh                     # state, elapsed/remaining, per-profile tally
```
The launch dashboard also prints the exact `tail`/stop commands. Interrupting a
run (Ctrl-C or `kill`) unlinks the active segment, so it leaves no stale
`/dev/shm` behind (`kill -9` is the exception, covered by unlink-before-create).

Slice length (arg 2) is operational, not a coverage knob: a profile's total
iterations over the run depend only on its *share* of the cycle, not on how
that time is chunked. Shorter slices just give finer failure-attribution
checkpoints. To shift priority between profiles, edit the weighting in the
`PROFILES` list -- don't lengthen slices.

The **crash test fuzzes its kill timing** (seeded random per round) so a long
soak explores new crash windows instead of re-hitting a fixed schedule:
```bash
build/kickmsg_crash_test                       # seed from clock -- logged at startup
KICKMSG_CRASH_SEED=12345 build/kickmsg_crash_test   # replay an exact schedule
KICKMSG_CRASH_ROUNDS=200 build/kickmsg_crash_test   # more kills per invocation
```

The **stall/repair test fuzzes a *false-positive* death**: the publisher is
not killed but SIGSTOPped at random instants, so it sometimes freezes while
holding an entry lock that a tight `commit_timeout` (2 ms) makes "provably
stale". The parent then runs `repair_locked_entries()` during the stall and
SIGCONTs the publisher, which resumes against a stolen lock. The theft-safe
protocol (position-tagged locks, CAS commit, theft guard) must turn every
steal into a clean drop -- the test fails on any torn payload, per-publisher
sequence rewind, or refcount/pool corruption, and bounds reclaimed orphan
slots by the number of steals. The run prints `Steals observed: N` (a 0 is a
WARN, not a failure -- the window is sampled probabilistically):
```bash
build/kickmsg_stall_repair_test                       # seed from clock -- logged at startup
KICKMSG_STALL_SEED=12345 build/kickmsg_stall_repair_test   # replay an exact schedule
build/kickmsg_stall_repair_test --rounds 1000              # more stalls per invocation
```

The **multi-process MPMC test** is the cross-process counterpart of the
in-process stress suite: the parent creates the region, forks 4 publisher and
2 subscriber *processes*, and holds the publishers on a pipe gate until every
subscriber ring is Live -- so per-subscriber accounting is exact
(received + lost == total sent). It fails on any corruption, per-publisher
sequence reorder, conservation mismatch, signal-killed child, or pool/refcount
damage. The rig runs it as the `mp` profile:
```bash
build/kickmsg_mp_stress_test                         # ~2-5 s
KICKMSG_MP_SEED=12345 build/kickmsg_mp_stress_test   # replay publisher start jitter
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

For a *single-profile* `endurance.sh` run (rungs 3-4); the rig in rung 5
self-detaches and handles `caffeinate` on its own.
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
