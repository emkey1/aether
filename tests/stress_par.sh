#!/usr/bin/env bash
# Stress lap for the par fixtures: run each one N times while every core is
# kept busy by `yes >/dev/null`, and count the runs whose output is wrong.
#
# Under load, two par branches printing at once used to tear a line:
# par_forward_target_args_pass failed 18 to 24 of 960 loaded runs. The
# deterministic par fixtures now write results into records and print after
# the join, so this lap must report 0 failed runs for each of them.
# par_stdout_lines_pass is the dedicated whole-line check. It is expected to
# tear until pscal-core's vmBuiltinWrite holds the stream lock across one
# write/writeln call, so its torn runs are counted and reported, but they fail
# the lap only with AETHER_PAR_STDOUT_XFAIL=0 (the same switch tests/run.sh
# reads). A crash, a non-zero exit or a hang is always a failure.
#
# CTest registers this as aether_stress_par with LABELS stress. It never
# gates: run the normal suite with `ctest -LE stress` and this lap with
# `ctest -L stress`.
#
# Usage: tests/stress_par.sh [runs-per-fixture]      (default 960)
# Env:   AETHER_BIN               binary under test (default: ../build/aether)
#        STRESS_PAR_RUNS          runs per fixture when no argument is given
#        STRESS_PAR_LOAD          number of `yes` processes (default: one per core)
#        AETHER_PAR_STDOUT_XFAIL  0 makes a torn par_stdout_lines_pass run a failure
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
AETHER_BIN="${AETHER_BIN:-$SCRIPT_DIR/../build/aether}"
RUNS="${1:-${STRESS_PAR_RUNS:-960}}"
XFAIL="${AETHER_PAR_STDOUT_XFAIL:-1}"
CORES="$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2)"
LOAD="${STRESS_PAR_LOAD:-$CORES}"
RUN_TIMEOUT=30  # seconds; a run that takes longer counts as a failure

if [ ! -x "$AETHER_BIN" ]; then
    echo "missing aether binary: $AETHER_BIN" >&2
    exit 1
fi
case "$RUNS" in
    '' | *[!0-9]* | 0)
        echo "runs-per-fixture must be a positive integer, got '$RUNS'" >&2
        exit 1
        ;;
esac
case "$LOAD" in
    '' | *[!0-9]*)
        echo "STRESS_PAR_LOAD must be a non-negative integer, got '$LOAD'" >&2
        exit 1
        ;;
esac

OUT="$(mktemp -d "${TMPDIR:-/tmp}/aether_stress_par.XXXXXX")"
load_pids=""
watchdog_pid=""
cleanup() {
    # wait reaps the killed jobs quietly (no "Terminated" line per process).
    if [ -n "$watchdog_pid" ]; then
        kill "$watchdog_pid" 2>/dev/null
        wait "$watchdog_pid" 2>/dev/null
    fi
    if [ -n "$load_pids" ]; then
        kill $load_pids 2>/dev/null
        wait $load_pids 2>/dev/null
    fi
    rm -rf "$OUT"
}
trap cleanup EXIT

# One `yes` per core keeps every core busy, so the VM's threads are preempted
# between the separate writes of one println, as on a loaded build machine.
# The watchdog stops the load and removes the scratch directory if this script
# dies without its EXIT trap (SIGKILL). Everything here is detached from
# stdout/stderr so that CTest does not wait on the pipe.
i=0
while [ "$i" -lt "$LOAD" ]; do
    yes >/dev/null 2>&1 &
    load_pids="$load_pids $!"
    i=$((i + 1))
done
if [ -n "$load_pids" ]; then
    (while kill -0 $$ 2>/dev/null; do sleep 2; done; kill $load_pids 2>/dev/null; rm -rf "$OUT") >/dev/null 2>&1 &
    watchdog_pid=$!
fi

echo "stress_par: $RUNS runs per fixture, $LOAD load processes on $CORES cores, binary $AETHER_BIN"

# run_one NAME OUTFILE: one run of tests/NAME.aether, stdout and stderr to OUTFILE.
run_one() {
    perl -e 'alarm shift; exec @ARGV or exit 127' "$RUN_TIMEOUT" \
        "$AETHER_BIN" --no-cache "$SCRIPT_DIR/$1.aether" >"$2" 2>&1
}

failed_runs=0

# check_exact NAME EXPECTED: EXPECTED is a printf-style string, the same
# golden tests/run.sh uses. Keeps the first failing output for the report.
check_exact() {
    local name="$1" expected="$2" fails=0 n=1 start="$SECONDS"
    printf '%b' "$expected" >"$OUT/$name.expected"
    while [ "$n" -le "$RUNS" ]; do
        if ! run_one "$name" "$OUT/$name.out" || ! cmp -s "$OUT/$name.expected" "$OUT/$name.out"; then
            fails=$((fails + 1))
            if [ "$fails" -eq 1 ]; then cp "$OUT/$name.out" "$OUT/$name.first_failure"; fi
        fi
        n=$((n + 1))
    done
    printf '%-32s %d/%d runs failed (%ds)\n' "$name" "$fails" "$RUNS" "$((SECONDS - start))"
    if [ "$fails" -gt 0 ]; then
        echo "  first failing output:"
        sed 's/^/  | /' "$OUT/$name.first_failure"
        failed_runs=$((failed_runs + fails))
    fi
}

check_exact par_pass 'worker A\nworker B\n'
check_exact par_forward_target_pass 'worker A\nworker B\n'
check_exact par_forward_target_mixed_pass 'worker A\nworker B\n'
check_exact par_forward_target_nested_pass 'worker A\nworker B\n'
check_exact par_forward_target_args_pass 'worker A 1\nworker B 2\n'

# par_stdout_lines_pass: the multiset of whole lines must match, in any order.
name=par_stdout_lines_pass
torn=0
broken=0
start="$SECONDS"
for tag in A B C D; do
    j=0
    while [ "$j" -lt 200 ]; do
        printf 'branch %s line %d of 200\n' "$tag" "$j"
        j=$((j + 1))
    done
done | LC_ALL=C sort >"$OUT/$name.expected"
n=1
while [ "$n" -le "$RUNS" ]; do
    if ! run_one "$name" "$OUT/$name.out"; then
        broken=$((broken + 1))
        if [ "$broken" -eq 1 ]; then cp "$OUT/$name.out" "$OUT/$name.first_failure"; fi
    elif ! LC_ALL=C sort "$OUT/$name.out" | cmp -s - "$OUT/$name.expected"; then
        torn=$((torn + 1))
    fi
    n=$((n + 1))
done
if [ "$XFAIL" = 1 ]; then
    verdict="expected-fail until vmBuiltinWrite is line-atomic"
else
    verdict="AETHER_PAR_STDOUT_XFAIL=0: torn runs fail the lap"
    failed_runs=$((failed_runs + torn))
fi
printf '%-32s %d/%d runs tore a line, %d crashed or hung (%ds; %s)\n' \
    "$name" "$torn" "$RUNS" "$broken" "$((SECONDS - start))" "$verdict"
if [ "$broken" -gt 0 ]; then
    echo "  first crashed or hung run:"
    sed 's/^/  | /' "$OUT/$name.first_failure" | head -20
    failed_runs=$((failed_runs + broken))
fi

if [ "$failed_runs" -ne 0 ]; then
    echo "stress_par: $failed_runs failed runs" >&2
    exit 1
fi
echo "stress_par: no failed runs"
