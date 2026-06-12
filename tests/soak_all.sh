#!/bin/bash
# Cycles weighted test profiles until a deadline, each a time-sliced endurance
# pass; aggregates results, persists failing runs, survives any slice failing.
# Self-detaches (survives logout), runs under caffeinate on macOS, and prints
# a watch/stop dashboard before returning.
#
# Usage: soak_all.sh [total_secs] [slice_secs]
#   total_secs : wall-clock budget (default 604800 = 1 week)
#   slice_secs : seconds per profile slice (default 1800 = 30 min)
#   SOAK_FOREGROUND=1   run inline instead of detaching
#   PLAIN_BIN / CRASH_BIN / TSAN_BIN   override binaries (TSAN optional)
#
# NOT 'set -e': a failing slice records and continues.
set -uo pipefail

TOTAL_SECS="${1:-604800}"
SLICE_SECS="${2:-1800}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SELF="$(cd "$(dirname "$0")" && pwd)/$(basename "$0")"
BASE_LOGDIR="${SOAK_LOGDIR:-$ROOT/soak_logs}"

# --- Launcher: detach + caffeinate, print dashboard, return -----------------
if [ -z "${KICKMSG_SOAK_RUNDIR:-}" ] && [ -z "${SOAK_FOREGROUND:-}" ]; then
    RUNDIR="$BASE_LOGDIR/run_$(date +%Y%m%d_%H%M%S)"
    mkdir -p "$RUNDIR"
    ln -sfn "$RUNDIR" "$BASE_LOGDIR/latest" 2>/dev/null || true
    export KICKMSG_SOAK_RUNDIR="$RUNDIR"
    caf=""
    caf_label="off (non-macOS)"
    if [ "$(uname)" = "Darwin" ] && command -v caffeinate >/dev/null 2>&1; then
        caf="caffeinate -i"
        caf_label="on (idle-sleep blocked)"
    fi
    nohup $caf "$SELF" "$TOTAL_SECS" "$SLICE_SECS" </dev/null >"$RUNDIR/stdout.log" 2>&1 &
    pid=$!
    echo "$pid" > "$RUNDIR/soak.pid"
    M="$RUNDIR/soak_all.log"
    cat <<EOF

=== kickmsg soak launched (detached; survives logout) ===
  pid        : $pid
  budget     : ${TOTAL_SECS}s total, ${SLICE_SECS}s slices
  caffeinate : $caf_label
  run dir    : $RUNDIR   (also: $BASE_LOGDIR/latest)

  watch      : tail -f "$M"
  rollup     : grep -E "VERDICT|SLICE |slices=" "$M"
  failures   : ls "$RUNDIR/fails/"           # empty == clean
  stop       : pkill -f soak_all.sh ; pkill -f endurance.sh

EOF
    exit 0
fi

# --- Worker -----------------------------------------------------------------
LOGDIR="${KICKMSG_SOAK_RUNDIR:-$BASE_LOGDIR/run_fg}"
SLICEDIR="$LOGDIR/slices"
MASTER="$LOGDIR/soak_all.log"
mkdir -p "$SLICEDIR" "$LOGDIR/fails"
# Overwrite the launcher's pid (caffeinate's wrapper) with the real worker pid.
echo "$$" > "$LOGDIR/soak.pid"

PLAIN="${PLAIN_BIN:-$ROOT/build/kickmsg_stress_test}"
CRASH="${CRASH_BIN:-$ROOT/build/kickmsg_crash_test}"
STALL="${STALL_BIN:-$ROOT/build/kickmsg_stall_repair_test}"
MP="${MP_BIN:-$ROOT/build/kickmsg_mp_stress_test}"
TSAN="${TSAN_BIN:-$ROOT/build_tsan/kickmsg_stress_test}"
ASAN="${ASAN_BIN:-$ROOT/build_asan/kickmsg_stress_test}"
TSAN_SUPP="$ROOT/tests/tsan.supp"
ENDURANCE="$ROOT/tests/endurance.sh"

# Weighted cycle: repeats encode priority.  TSAN is the rarest, highest-value
# signal, so it takes half the slices; crash fuzz a quarter; plain stress the
# rest as periodic sanity at two contention levels.  Oversub is kept <=200 on
# purpose -- a single oversub-300 run can take tens of minutes and blow past a
# slice boundary (endurance.sh only checks the clock between runs).
# A profile whose binary is absent is skipped, not fatal.
PROFILES=(
  "tsan-150|$TSAN|--oversub 150"
  "crash|$CRASH|"
  "tsan-150|$TSAN|--oversub 150"
  "stress-150|$PLAIN|--oversub 150"
  "stall|$STALL|"
  "mp|$MP|"
  "tsan-150|$TSAN|--oversub 150"
  "crash|$CRASH|"
  "tsan-150|$TSAN|--oversub 150"
  "asan-150|$ASAN|--oversub 150"
  "stress-200|$PLAIN|--oversub 200"
)

TOTAL_SLICES=0
TOTAL_PASS=0
TOTAL_FAIL=0
TOTAL_SAN=0
HARNESS_FAIL=0
START=$(date +%s)
DEADLINE=$((START + TOTAL_SECS))

log() { echo "[$(date '+%Y-%m-%d %H:%M:%S')] $*" | tee -a "$MASTER"; }

summary() {
    echo "" | tee -a "$MASTER"
    log "=== SOAK SUMMARY ==="
    log "slices=$TOTAL_SLICES pass_scenarios=$TOTAL_PASS fail_scenarios=$TOTAL_FAIL san_hits=$TOTAL_SAN harness_fails=$HARNESS_FAIL"
    # Per-profile breakdown via awk over the recorded slice lines (no
    # associative arrays -- this must run on macOS bash 3.2).
    echo "--- per-profile ---" | tee -a "$MASTER"
    grep -hE "^SLICE " "$MASTER" 2>/dev/null \
        | awk '{p=$0; sub(/.*profile=/,"",p); sub(/ .*/,"",p);
                f=$0; sub(/.*fail=/,"",f); sub(/ .*/,"",f);
                n[p]++; ff[p]+=f}
               END{for(k in n) printf "  %-12s slices=%d fail=%d\n", k, n[k], ff[k]}' \
        | tee -a "$MASTER"
    if [ "$TOTAL_FAIL" -gt 0 ] || [ "$HARNESS_FAIL" -gt 0 ]; then
        log "VERDICT: FAILURES DETECTED -- evidence under $LOGDIR/fails/"
    elif [ "$TOTAL_SLICES" -eq 0 ] || [ "$TOTAL_PASS" -eq 0 ]; then
        # Zero work done must never read as a clean week: a stale build
        # dir or wrong cwd would otherwise soak nothing and report green.
        log "VERDICT: NO WORK DONE -- check binaries and run dir"
    else
        log "VERDICT: ALL CLEAN"
    fi
}

on_signal() {
    summary
    rc=130
    if [ "$TOTAL_FAIL" -gt 0 ] || [ "$HARNESS_FAIL" -gt 0 ]; then
        rc=1
    fi
    exit "$rc"
}
trap on_signal INT TERM

log "soak start: total=${TOTAL_SECS}s slice=${SLICE_SECS}s deadline_epoch=$DEADLINE"
log "binaries: plain=$([ -x "$PLAIN" ] && echo yes || echo NO) crash=$([ -x "$CRASH" ] && echo yes || echo NO) stall=$([ -x "$STALL" ] && echo yes || echo NO) mp=$([ -x "$MP" ] && echo yes || echo NO) tsan=$([ -x "$TSAN" ] && echo yes || echo NO) asan=$([ -x "$ASAN" ] && echo yes || echo NO)"

if [ ! -x "$PLAIN" ] && [ ! -x "$CRASH" ] && [ ! -x "$TSAN" ]; then
    # Without this guard the while loop below spins at full speed for the
    # whole budget, appending a skip line per iteration and ending ALL CLEAN.
    log "FATAL: no profile binary is executable -- nothing to soak"
    summary
    exit 2
fi

i=0
while [ "$(date +%s)" -lt "$DEADLINE" ]; do
    n=${#PROFILES[@]}
    idx=$((i % n))
    i=$((i + 1))
    entry="${PROFILES[$idx]}"
    label="${entry%%|*}"
    rest="${entry#*|}"
    bin="${rest%%|*}"
    extra="${rest#*|}"
    if [ "$extra" = "$bin" ]; then
        extra=""
    fi

    if [ ! -x "$bin" ]; then
        log "skip profile=$label (binary absent: $bin)"
        continue
    fi

    now=$(date +%s)
    remain=$((DEADLINE - now))
    if [ "$remain" -lt 5 ]; then
        break
    fi
    this="$SLICE_SECS"
    if [ "$remain" -lt "$this" ]; then
        this="$remain"
    fi

    TOTAL_SLICES=$((TOTAL_SLICES + 1))
    slog="$SLICEDIR/$(printf '%04d' "$TOTAL_SLICES")_${label}.log"
    log "slice $TOTAL_SLICES profile=$label dur=${this}s -> $slog"

    # Per-slice evidence dir (slice number in the path: evidence from one
    # slice must never overwrite another's); sanitizer options only for
    # TSAN slices.
    export FAILDIR="$LOGDIR/fails/${label}_s$(printf '%04d' "$TOTAL_SLICES")"
    # abort_on_error=1 on top of halt_on_error: the report is followed by
    # abort() instead of exit(), so a core dump is captured when ulimits allow.
    case "$label" in
        tsan*)
            supp=""
            if [ -f "$TSAN_SUPP" ]; then
                supp="suppressions=$TSAN_SUPP:"
            fi
            export TSAN_OPTIONS="${supp}halt_on_error=1:abort_on_error=1:exitcode=66"
            unset ASAN_OPTIONS UBSAN_OPTIONS 2>/dev/null || true
            ;;
        asan*)
            export ASAN_OPTIONS="halt_on_error=1:abort_on_error=1:exitcode=66"
            export UBSAN_OPTIONS="halt_on_error=1:abort_on_error=1"
            unset TSAN_OPTIONS 2>/dev/null || true
            ;;
        *)
            unset TSAN_OPTIONS ASAN_OPTIONS UBSAN_OPTIONS 2>/dev/null || true
            ;;
    esac

    rc=0
    "$ENDURANCE" "$bin" "$this" $extra > "$slog" 2>&1 || rc=$?

    p=$(grep -E "Scenarios passed:" "$slog" | grep -oE '[0-9]+' | tail -1 || true)
    f=$(grep -E "Scenarios failed:" "$slog" | grep -oE '[0-9]+' | tail -1 || true)
    sa=$(grep -E "Sanitizer hits:" "$slog" | grep -oE '[0-9]+' | tail -1 || true)
    v=$(grep -E "VERDICT:" "$slog" | tail -1 || true)
    p="${p:-0}"
    f="${f:-0}"
    sa="${sa:-0}"
    # No VERDICT line means endurance.sh itself died (bash error, OOM kill,
    # stray pkill) before finishing the slice -- a harness failure, distinct
    # from a test failure, and never silently counted as a clean slice.
    hfail=0
    if [ -z "$v" ]; then
        hfail=1
        HARNESS_FAIL=$((HARNESS_FAIL + 1))
        mkdir -p "$LOGDIR/fails"
        cp "$slog" "$LOGDIR/fails/harness_slice_$(printf '%04d' "$TOTAL_SLICES").log" 2>/dev/null || true
    fi
    TOTAL_PASS=$((TOTAL_PASS + p))
    TOTAL_FAIL=$((TOTAL_FAIL + f))
    TOTAL_SAN=$((TOTAL_SAN + sa))
    # Machine-greppable record for the per-profile breakdown above.
    echo "SLICE $TOTAL_SLICES profile=$label pass=$p fail=$f san=$sa rc=$rc hfail=$hfail" >> "$MASTER"
    log "  done profile=$label pass=$p fail=$f san=$sa rc=$rc ${v:-(no verdict line -- HARNESS FAILURE)}"
done

summary
if [ "$TOTAL_FAIL" -gt 0 ] || [ "$HARNESS_FAIL" -gt 0 ]; then
    exit 1
fi
if [ "$TOTAL_SLICES" -eq 0 ] || [ "$TOTAL_PASS" -eq 0 ]; then
    exit 2
fi
exit 0
