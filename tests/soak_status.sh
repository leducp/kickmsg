#!/bin/bash
# Compact live summary of a soak_all.sh run.
# Usage: soak_status.sh [run_dir]   (defaults to soak_logs/latest)
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
RUNDIR="${1:-${SOAK_LOGDIR:-$ROOT/soak_logs}/latest}"
M="$RUNDIR/soak_all.log"
if [ ! -f "$M" ]; then
    echo "no soak log at $M -- run tests/soak_all.sh first"
    exit 1
fi

fmt() { # seconds -> "Hh MMm SSs"
    local s="$1"
    printf '%dh %02dm %02ds' "$((s / 3600))" "$(((s % 3600) / 60))" "$((s % 60))"
}

pid=""
[ -f "$RUNDIR/soak.pid" ] && pid="$(cat "$RUNDIR/soak.pid" 2>/dev/null)"
state="finished"
if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
    state="RUNNING (pid $pid)"
fi

start_line="$(grep -m1 'soak start' "$M" 2>/dev/null || true)"
total="$(echo "$start_line" | grep -oE 'total=[0-9]+' | grep -oE '[0-9]+' || true)"
deadline="$(echo "$start_line" | grep -oE 'deadline_epoch=[0-9]+' | grep -oE '[0-9]+' || true)"
total="${total:-0}"
deadline="${deadline:-0}"
now="$(date +%s)"
elapsed=0
remain=0
if [ "$deadline" -gt 0 ]; then
    elapsed=$((now - (deadline - total)))
    remain=$((deadline - now))
    [ "$remain" -lt 0 ] && remain=0
fi

# grep -c always prints a count but exits 1 when zero -- don't add `|| echo 0`
# or it double-prints. set -e is off, so a non-zero exit here is harmless.
slices="$(grep -c '^SLICE ' "$M" 2>/dev/null)"
slices="${slices:-0}"
pass="$(grep '^SLICE ' "$M" 2>/dev/null | grep -oE 'pass=[0-9]+' | grep -oE '[0-9]+' | awk '{s+=$1} END{print s+0}')"
fail="$(grep '^SLICE ' "$M" 2>/dev/null | grep -oE 'fail=[0-9]+' | grep -oE '[0-9]+' | awk '{s+=$1} END{print s+0}')"
nfail="$(find "$RUNDIR/fails" -type f 2>/dev/null | wc -l | tr -d ' ')"

echo "=== kickmsg soak status ==="
echo "  state    : $state"
echo "  run dir  : $RUNDIR"
echo "  elapsed  : $(fmt "$elapsed") of $(fmt "$total")   (remaining $(fmt "$remain"))"
echo "  slices   : $slices done   pass_scenarios=$pass   fail_scenarios=$fail   fail_logs=$nfail"
echo "  per-profile:"
grep '^SLICE ' "$M" 2>/dev/null \
    | awk '{p=$0; sub(/.*profile=/,"",p); sub(/ .*/,"",p);
            f=$0; sub(/.*fail=/,"",f); sub(/ .*/,"",f);
            n[p]++; ff[p]+=f}
           END{for(k in n) printf "    %-12s slices=%d fail=%d\n", k, n[k], ff[k]}'

# Current/last slice and its latest live tally (\r-separated -> last field).
cur="$(grep -E ' slice [0-9]+ profile=' "$M" 2>/dev/null | tail -1 || true)"
if [ -n "$cur" ]; then
    label="$(echo "$cur" | sed -E 's/.* profile=([^ ]+).*/\1/')"
    slog="$(echo "$cur" | sed -E 's/.*-> //')"
    tally=""
    [ -f "$slog" ] && tally="$(tr '\r' '\n' < "$slog" | grep -E '^\[[0-9]+s/' | tail -1 || true)"
    echo "  current  : $label   ${tally:-(starting)}"
fi

if [ "$nfail" -gt 0 ] || [ "$fail" -gt 0 ]; then
    echo "  !! failures recorded -- see $RUNDIR/fails/"
fi
