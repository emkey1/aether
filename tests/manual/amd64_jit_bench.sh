#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

ISH_BIN=./build/ish
ROOTFS=.tmp-root-amd64-alpine
OUT_DIR=
TOP_N=8
TRACE_JIT=0

declare -a CASE_NAMES=()
declare -a CASE_COMMANDS=()

usage() {
    cat <<'EOF'
Usage: tests/manual/amd64_jit_bench.sh [options]

Run a small amd64 JIT progress benchmark against a local guest rootfs.
The script records pass/fail, wall-clock time, amd64 JIT helper counts,
and helper-step opcode histograms for a fixed command set.

Options:
  -b BIN      Path to iSH host binary. Default: ./build/ish
  -r ROOTFS   Path to amd64 guest rootfs. Default: .tmp-root-amd64-alpine
  -o DIR      Output directory. Default: ./e2e_out/amd64-jit-bench-<timestamp>
  -k N        Number of top helper-step opcodes/helpers to report. Default: 8
  -c SPEC     Add a benchmark case as name=command
  -t          Enable expensive ISH_TRACE_AMD64_JIT tracing for JIT runs
  -h          Show this help

Examples:
  tests/manual/amd64_jit_bench.sh
  tests/manual/amd64_jit_bench.sh -r /tmp/amd64-root -c 'ssh_v=ssh -V >/dev/null'
EOF
}

add_case() {
    local spec=$1
    local name=${spec%%=*}
    local command=${spec#*=}
    if [[ -z $name || $name == "$spec" || -z $command ]]; then
        echo "invalid case spec: $spec" >&2
        exit 2
    fi
    CASE_NAMES+=("$name")
    CASE_COMMANDS+=("$command")
}

while getopts ":b:r:o:k:c:th" opt; do
    case "$opt" in
        b) ISH_BIN=$OPTARG ;;
        r) ROOTFS=$OPTARG ;;
        o) OUT_DIR=$OPTARG ;;
        k) TOP_N=$OPTARG ;;
        c) add_case "$OPTARG" ;;
        t) TRACE_JIT=1 ;;
        h)
            usage
            exit 0
            ;;
        :)
            echo "missing argument for -$OPTARG" >&2
            usage >&2
            exit 2
            ;;
        \?)
            echo "unknown option: -$OPTARG" >&2
            usage >&2
            exit 2
            ;;
    esac
done

if [[ ${#CASE_NAMES[@]} -eq 0 ]]; then
    add_case 'true=/bin/true'
    add_case 'uname=uname -a >/dev/null'
    add_case 'busybox_help=busybox --help >/dev/null'
    add_case 'apk_help=apk --help >/dev/null || true'
fi

if [[ ! -x $ISH_BIN ]]; then
    echo "iSH binary is not executable: $ISH_BIN" >&2
    exit 1
fi

if [[ ! -d $ROOTFS ]]; then
    echo "guest rootfs not found: $ROOTFS" >&2
    exit 1
fi

if [[ -z ${OUT_DIR:-} ]]; then
    OUT_DIR=./e2e_out/amd64-jit-bench-$(date +%Y%m%d-%H%M%S)
fi
mkdir -p "$OUT_DIR"

top_helper_steps() {
    local log=$1
    awk '
        /\[amd64-jit\] helper-step / {
            for (i = 1; i <= NF; i++) {
                if ($i ~ /^opcode=/) {
                    split($i, a, "=")
                    count[a[2]]++
                }
            }
        }
        END {
            for (op in count)
                printf "%s\t%d\n", op, count[op]
        }
    ' "$log" | sort -t "$(printf '\t')" -k2,2nr | head -n "$TOP_N"
}

top_direct_helpers() {
    local log=$1
    awk '
        /\[amd64-jit\] [a-z0-9-]+ ip=/ {
            split($2, a, "]")
            helper=$3
            count[helper]++
        }
        END {
            for (name in count)
                printf "%s\t%d\n", name, count[name]
        }
    ' "$log" | grep -v '^helper-step' | sort -t "$(printf '\t')" -k2,2nr | head -n "$TOP_N"
}

run_case() {
    local case_name=$1
    local mode_name=$2
    local mode_value=$3
    local command=$4
    local prefix=$OUT_DIR/${case_name}.${mode_name}
    local stdout_log=${prefix}.stdout
    local stderr_log=${prefix}.stderr
    local guest_script=${prefix}.guest.sh
    local rc=0
    local real_s=
    local helper_steps=0
    local direct_helpers=0
    local bad_targets=0
    local quoted_command=
    local guest_command=

    printf -v quoted_command '%q' "$command"
    guest_command="echo $mode_value >/proc/ish/amd64_jit; exec /bin/sh -lc $quoted_command"
    printf '%s\n' "$guest_command" >"$guest_script"

    set +e
    if [[ $mode_name == jit && $TRACE_JIT -eq 1 ]]; then
        ISH_TRACE_AMD64_JIT=1 /usr/bin/time -p \
            "$ISH_BIN" -r "$ROOTFS" /bin/sh -lc "$guest_command" \
            >"$stdout_log" 2>"$stderr_log"
        rc=$?
    else
        /usr/bin/time -p \
            "$ISH_BIN" -r "$ROOTFS" /bin/sh -lc "$guest_command" \
            >"$stdout_log" 2>"$stderr_log"
        rc=$?
    fi
    set -e

    real_s=$(awk '$1 == "real" { print $2; exit }' "$stderr_log")
    real_s=${real_s:-NA}

    if [[ $mode_name == jit && $TRACE_JIT -eq 1 ]]; then
        helper_steps=$(grep -c '\[amd64-jit\] helper-step ' "$stderr_log" || true)
        direct_helpers=$(grep -Ec '\[amd64-jit\] (ret-helper|push-helper|pop-helper|call-rel32-helper|jmp-rel32-helper|jmp-rel8-helper|jcc-rel8-helper|jcc-rel32-helper)' "$stderr_log" || true)
        bad_targets=$(grep -Ec '\[amd64-jit\] (bad-|helper bad-rip)' "$stderr_log" || true)
        top_helper_steps "$stderr_log" >"${prefix}.top_helper_steps.tsv" || true
        top_direct_helpers "$stderr_log" >"${prefix}.top_direct_helpers.tsv" || true
    fi

    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$case_name" "$mode_name" "$rc" "$real_s" \
        "$helper_steps" "$direct_helpers" "$bad_targets" \
        >>"$OUT_DIR/summary.tsv"
}

{
    printf 'case\tmode\trc\treal_s\thelper_steps\tdirect_helpers\tbad_targets\n'
} >"$OUT_DIR/summary.tsv"

for i in "${!CASE_NAMES[@]}"; do
    run_case "${CASE_NAMES[$i]}" interp 0 "${CASE_COMMANDS[$i]}"
    run_case "${CASE_NAMES[$i]}" jit 1 "${CASE_COMMANDS[$i]}"
done

cat <<EOF >"$OUT_DIR/README.txt"
amd64 JIT benchmark output
==========================

Files:
  summary.tsv                  Per-case summary for interp and jit runs
  *.stdout                     Guest stdout
  *.stderr                     Guest stderr and timing data
  *.guest.sh                   Guest command script used for the run
  *.top_helper_steps.tsv       Top helper-step opcodes for traced JIT runs
  *.top_direct_helpers.tsv     Top direct helper hits for traced JIT runs

Columns in summary.tsv:
  case             Benchmark case name
  mode             interp or jit
  rc               Exit code
  real_s           Wall-clock seconds from /usr/bin/time -p
  helper_steps     Count of generic [amd64-jit] helper-step lines
  direct_helpers   Count of lowered direct helper hits
  bad_targets      Count of bad-target / bad-rip diagnostics

Tracing is disabled by default because full amd64 JIT traces are large.
Use -t when you want opcode/helper histograms in addition to timings.
EOF

echo "amd64 JIT benchmark results: $OUT_DIR"
column -t -s "$(printf '\t')" "$OUT_DIR/summary.tsv"

for path in "$OUT_DIR"/*.top_helper_steps.tsv; do
    [[ -e $path ]] || continue
    echo
    echo "==> $(basename "$path" .top_helper_steps.tsv) top helper-step opcodes"
    column -t -s "$(printf '\t')" "$path"
done

for path in "$OUT_DIR"/*.top_direct_helpers.tsv; do
    [[ -e $path ]] || continue
    if [[ ! -s $path ]]; then
        continue
    fi
    echo
    echo "==> $(basename "$path" .top_direct_helpers.tsv) top direct helpers"
    column -t -s "$(printf '\t')" "$path"
done
