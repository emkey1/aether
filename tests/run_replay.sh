#!/usr/bin/env bash
# Replay lap (CTest aether_replay): the real model-written benchmark programs
# that passed when tools/export_replay.py froze them into tests/replay/ must
# still print exactly their task's expected stdout, with the expected exit
# status, run the way the benchmark harness runs them (--deny net,proc
# --no-cache, the program as <task>.aether in a fresh directory holding the
# task's files, cwd as the task says, stdin from <case>.in or empty).
#
# A case listed in tests/replay/WAIVED (`<case> <CHANGELOG version> <reason>`)
# is a declared break: it must fail, and the lap fails when it passes again.
# Regenerate the cases only with tools/export_replay.py.
#
#   AETHER_BIN=build/aether tests/run_replay.sh [-j N] [CASE ...]
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPLAY="$SCRIPT_DIR/replay"
AETHER_BIN="${AETHER_BIN:-$SCRIPT_DIR/../build/aether}"
case "$AETHER_BIN" in /*) ;; *) AETHER_BIN="$PWD/$AETHER_BIN" ;; esac

# --one CASE TASK FILES CWD RC TIMEOUT: run one case in $REPLAY_WORK, print one
# result line (xargs runs these in parallel).
if [ "${1:-}" = "--one" ]; then
    name="$2" task="$3" files="$4" cwd="$5" want_rc="$6" timeout="$7"
    dir="$(mktemp -d "$REPLAY_WORK/case.XXXXXX")"
    if [ "$files" != "-" ]; then
        cp -R "$REPLAY/files/$files"/. "$dir"/
    fi
    cp "$REPLAY/$name.aether" "$dir/$task.aether"
    run_dir="$dir"
    if [ "$cwd" != "-" ]; then
        run_dir="$dir/$cwd"
        mkdir -p "$run_dir"
    fi
    stdin=/dev/null
    [ -f "$REPLAY/$name.in" ] && stdin="$REPLAY/$name.in"
    (cd "$run_dir" && perl -e 'alarm shift; exec @ARGV' "$timeout" \
        "$AETHER_BIN" --deny net,proc --no-cache "$dir/$task.aether" \
        <"$stdin" >"$dir/stdout" 2>"$dir/stderr")
    rc=$?
    if [ "$rc" = "$want_rc" ] && cmp -s "$REPLAY/$name.out" "$dir/stdout"; then
        echo "PASS $name"
    else
        why="exit $rc (want $want_rc)"
        if ! cmp -s "$REPLAY/$name.out" "$dir/stdout"; then
            line="$(diff "$REPLAY/$name.out" "$dir/stdout" | head -3 | tr '\n' ' ' | cut -c1-160)"
            why="$why; stdout differs: $line"
        fi
        err="$(head -1 "$dir/stderr" | sed 's#^[^ ]*/\([^/ ]*\.aether:\)#\1#' | cut -c1-160)"
        [ -n "$err" ] && why="$why; stderr: $err"
        echo "FAIL $name $why"
    fi
    rm -rf "$dir"
    exit 0
fi

JOBS=4
ONLY=""
while [ $# -gt 0 ]; do
    case "$1" in
        -j) JOBS="$2"; shift 2 ;;
        *) ONLY="$ONLY $1"; shift ;;
    esac
done

if [ ! -x "$AETHER_BIN" ]; then
    echo "AETHER_BIN not executable: $AETHER_BIN" >&2
    exit 2
fi
if [ ! -f "$REPLAY/MANIFEST.tsv" ]; then
    echo "no tests/replay/MANIFEST.tsv: run tools/export_replay.py" >&2
    exit 2
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/aether_replay.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

# Waived cases, checked against CHANGELOG.md.
bad_waiver=0
waived=" "
if [ -f "$REPLAY/WAIVED" ]; then
    while read -r wcase wversion _; do
        case "$wcase" in ''|\#*) continue ;; esac
        if ! grep -q "^## $wversion\$" "$SCRIPT_DIR/../CHANGELOG.md"; then
            echo "BAD WAIVER $wcase: '$wversion' is not a CHANGELOG.md version"
            bad_waiver=1
        fi
        if ! grep -q "^$wcase	" "$REPLAY/MANIFEST.tsv"; then
            echo "BAD WAIVER $wcase: no such case"
            bad_waiver=1
        fi
        waived="$waived$wcase "
    done <"$REPLAY/WAIVED"
fi

export REPLAY_WORK="$WORK" AETHER_BIN
grep -v '^#' "$REPLAY/MANIFEST.tsv" | while IFS="$(printf '\t')" read -r name task files cwd rc timeout; do
    [ -n "$name" ] || continue
    if [ -n "$ONLY" ]; then
        case " $ONLY " in *" $name "*) ;; *) continue ;; esac
    fi
    printf '%s\0' "$name" "$task" "$files" "$cwd" "$rc" "$timeout"
done | xargs -0 -n 6 -P "$JOBS" "$0" --one >"$WORK/results"

pass=0; fail=0; xfail=0; xpass=0
while read -r status name rest; do
    case "$waived" in
        *" $name "*)
            if [ "$status" = PASS ]; then
                echo "XPASS $name: listed in tests/replay/WAIVED but passes; delete its line"
                xpass=$((xpass + 1))
            else
                xfail=$((xfail + 1))
            fi ;;
        *)
            if [ "$status" = PASS ]; then
                pass=$((pass + 1))
            else
                echo "FAIL  $name: $rest"
                fail=$((fail + 1))
            fi ;;
    esac
done <"$WORK/results"
total=$((pass + fail + xfail + xpass))
echo "replay: $total case(s): $pass passed, $fail failed, $xfail waived (failing as declared), $xpass waived but passing"
if [ "$fail" -ne 0 ] || [ "$xpass" -ne 0 ] || [ "$bad_waiver" -ne 0 ] || [ "$total" -eq 0 ]; then
    exit 1
fi
exit 0
