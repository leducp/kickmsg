#!/bin/bash
# Loop a kickmsg test binary for a wall-clock duration and tally results.
#
# Usage: endurance.sh <binary> [duration_secs] [extra args...]
#   Extra args after the duration are forwarded to the binary each run, e.g.
#     endurance.sh build/kickmsg_stress_test 3600 --oversub 400   # crank contention
#     endurance.sh build/kickmsg_crash_test  3600                 # soak crash recovery
#
# Binaries that print "=== Summary: N passed, M failed ===" (the stress suite)
# are tallied by those counts; others (e.g. the crash test) are tallied by exit
# code, one pass/fail per run.
set -euo pipefail
BINARY="$1"
DURATION_SECS="${2:-3600}"
shift                            # drop binary
if [ "$#" -gt 0 ]; then
    shift                        # drop duration, if it was given
fi
EXTRA_ARGS=("$@")                # anything left is forwarded to the binary

END_TIME=$(($(date +%s) + DURATION_SECS))
PASS=0
FAIL=0
RUNS=0
REORDERS=0
echo "=== kickmsg endurance test ==="
echo "Binary: $BINARY ${EXTRA_ARGS[*]+"${EXTRA_ARGS[*]}"}"
echo "Duration: ${DURATION_SECS}s"
echo "Start: $(date)"
echo ""
while [ "$(date +%s)" -lt "$END_TIME" ]; do
    RC=0
    OUTPUT=$("$BINARY" ${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"} 2>&1) || RC=$?
    RUNS=$((RUNS + 1))
    if [ "$RUNS" -eq 1 ]; then
        echo "$OUTPUT" | grep -iE "harness built|contention:" || true
        echo ""
    fi
    SUMMARY=$(echo "$OUTPUT" | grep "Summary:" | tail -1 || true)
    if [ -n "$SUMMARY" ]; then
        # Guarded: a garbled/interleaved Summary line (possible under heavy
        # sanitizer contention) must not let set -e kill the whole soak.
        RUN_PASS=$(echo "$SUMMARY" | grep -oE '[0-9]+ passed' | grep -oE '[0-9]+' || true)
        RUN_FAIL=$(echo "$SUMMARY" | grep -oE '[0-9]+ failed' | grep -oE '[0-9]+' || true)
    else
        # No summary line (e.g. crash test): tally by exit code.
        if [ "$RC" -eq 0 ]; then
            RUN_PASS=1
            RUN_FAIL=0
        else
            RUN_PASS=0
            RUN_FAIL=1
        fi
    fi
    RUN_PASS=${RUN_PASS:-0}
    RUN_FAIL=${RUN_FAIL:-0}
    RUN_REORDER=$(echo "$OUTPUT" | { grep -c "REORDER" || true; })
    # Sanitizer reports (TSAN/ASAN/UBSAN) go to stderr and do NOT bump the
    # suite's "failed" count -- detect them explicitly or they get swallowed.
    RUN_SANITIZER=$(echo "$OUTPUT" | { grep -c -E "ThreadSanitizer|AddressSanitizer|runtime error:" || true; })
    if [ "$RUN_SANITIZER" -gt 0 ] && [ "$RUN_FAIL" -eq 0 ]; then
        RUN_FAIL=1
    fi
    PASS=$((PASS + RUN_PASS))
    FAIL=$((FAIL + RUN_FAIL))
    REORDERS=$((REORDERS + RUN_REORDER))
    ELAPSED=$(($(date +%s) - END_TIME + DURATION_SECS))
    printf "\r[%ds/%ds] runs=%d pass=%d fail=%d reorders=%d san=%d" \
           "$ELAPSED" "$DURATION_SECS" "$RUNS" "$PASS" "$FAIL" "$REORDERS" "$RUN_SANITIZER"
    if [ "$RUN_FAIL" -gt 0 ] || [ "$RC" -ne 0 ]; then
        # Persist the full run output -- the evidence is otherwise lost.
        FAILDIR="${FAILDIR:-endurance_fails}"
        mkdir -p "$FAILDIR"
        printf '%s\n' "$OUTPUT" > "$FAILDIR/run_${RUNS}_rc${RC}.log"
        echo ""
        echo "$OUTPUT" | grep -E "REORDER|FAIL|WARN|ThreadSanitizer|runtime error:" || true
    fi
done
echo ""
echo ""
echo "=== FINAL RESULTS ==="
echo "Duration: ${DURATION_SECS}s"
echo "Runs: $RUNS"
echo "Scenarios passed: $PASS"
echo "Scenarios failed: $FAIL"
echo "Total reorders: $REORDERS"
echo "End: $(date)"
if [ "$FAIL" -eq 0 ]; then
    echo "VERDICT: ALL CLEAN"
else
    echo "VERDICT: FAILURES DETECTED"
    exit 1
fi
