#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

ISH_BIN="${ISH_BIN:-./build/ish}"
ROOTFS_TEMPLATE="${1:-.tmp-go-alpine-x86-pass}"
OUTDIR="${OUTDIR:-e2e_out/go_release_matrix_$(date +%Y%m%d_%H%M%S)}"
WORK_ROOT="${WORK_ROOT:-.tmp-go-release-matrix}"
HOST_GOCACHE="${HOST_GOCACHE:-$PWD/.tmp-host-gocache}"
HOST_GOPATH="${HOST_GOPATH:-$PWD/.tmp-host-gopath}"
SMOKE_GEN_COUNT="${SMOKE_GEN_COUNT:-1}"
SMOKE_MONITORS="${SMOKE_MONITORS:-0}"
MATRIX_CASES="${MATRIX_CASES:-smoke hey cobra-cli}"
SMOKE_ROOTFS="${WORK_ROOT}/rootfs-smoke"
SMOKE_OUTDIR="${OUTDIR}/smoke"
SUMMARY_TSV="${OUTDIR}/summary.tsv"
SUMMARY_MD="${OUTDIR}/SUMMARY.md"

mkdir -p "$OUTDIR" "$WORK_ROOT" "$HOST_GOCACHE" "$HOST_GOPATH" "$WORK_ROOT/repos"

host_go() {
    local go_bin="${HOST_GO_BIN:-/opt/homebrew/bin/go}"
    local goroot="${HOST_GOROOT:-/opt/homebrew/Cellar/go/1.26.2/libexec}"
    GOROOT="$goroot" GOPATH="$HOST_GOPATH" GOCACHE="$HOST_GOCACHE" "$go_bin" "$@"
}

summary_init() {
    printf 'case\tkind\tstatus\trootfs\tartifact\tlog\tnotes\n' >"$SUMMARY_TSV"
}

summary_add() {
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$1" "$2" "$3" "$4" "$5" "$6" "$7" >>"$SUMMARY_TSV"
}

summary_render_md() {
    {
        printf '# Go Release Matrix\n\n'
        printf '| Case | Kind | Status | Rootfs | Artifact | Log | Notes |\n'
        printf '| --- | --- | --- | --- | --- | --- | --- |\n'
        tail -n +2 "$SUMMARY_TSV" | while IFS=$'\t' read -r case kind status rootfs artifact log notes; do
            printf '| `%s` | `%s` | `%s` | `%s` | `%s` | `%s` | %s |\n' \
                "$case" "$kind" "$status" "$rootfs" "$artifact" "$log" "$notes"
        done
    } >"$SUMMARY_MD"
}

copy_rootfs() {
    local dest="$1"
    rm -rf "$dest"
    rsync -a --delete "$ROOTFS_TEMPLATE"/ "$dest"/
}

clone_repo() {
    local name="$1"
    local url="$2"
    local dir="${WORK_ROOT}/repos/${name}"
    if [ ! -d "$dir/.git" ]; then
        git clone --depth 1 "$url" "$dir" >/dev/null
    fi
    printf '%s\n' "$dir"
}

prepare_repo() {
    local name="$1"
    local url="$2"
    local dir
    dir="$(clone_repo "$name" "$url")"
    (
        cd "$dir"
        host_go mod vendor >/dev/null
    )
    printf '%s\n' "$dir"
}

run_smoke_case() {
    local case_name="smoke"
    copy_rootfs "$SMOKE_ROOTFS"
    if GEN_COUNT="$SMOKE_GEN_COUNT" RUN_BACKGROUND_MONITORS="$SMOKE_MONITORS" OUTDIR="$SMOKE_OUTDIR" \
        tests/manual/go_release_smoke.sh "$SMOKE_ROOTFS"; then
        local status="unknown"
        if [ -f "$SMOKE_OUTDIR/status.txt" ]; then
            status="$(tr -d '\n' <"$SMOKE_OUTDIR/status.txt")"
        fi
        summary_add "$case_name" "synthetic" "$status" "$SMOKE_ROOTFS" "$SMOKE_OUTDIR" "$SMOKE_OUTDIR/build-cold.log" "GEN_COUNT=$SMOKE_GEN_COUNT monitors=$SMOKE_MONITORS"
    else
        local status="driver-failed"
        if [ -f "$SMOKE_OUTDIR/status.txt" ]; then
            status="$(tr -d '\n' <"$SMOKE_OUTDIR/status.txt")"
        fi
        summary_add "$case_name" "synthetic" "$status" "$SMOKE_ROOTFS" "$SMOKE_OUTDIR" "$SMOKE_OUTDIR/build-cold.log" "GEN_COUNT=$SMOKE_GEN_COUNT monitors=$SMOKE_MONITORS"
    fi
}

run_external_case() {
    local case_name="$1"
    local url="$2"
    local guest_dir="$3"
    local build_cmd="$4"
    local verify_cmd="$5"
    local rootfs="${WORK_ROOT}/rootfs-${case_name}"
    local out="${OUTDIR}/${case_name}"
    local repo_dir
    repo_dir="$(prepare_repo "$case_name" "$url")"

    copy_rootfs "$rootfs"
    mkdir -p "$out"

    tar -cf - -C "$repo_dir" . | "$ISH_BIN" -f "$rootfs" /bin/sh -lc "rm -rf '$guest_dir' && mkdir -p '$guest_dir' && tar -xf - -C '$guest_dir'"

    local guest_script="
set -eu
cd '$guest_dir'
export HOME=/tmp
export GOCACHE=/tmp/${case_name}-gocache
export GOFLAGS=-mod=vendor
mkdir -p \"\$GOCACHE\"
time sh -lc '$build_cmd' > /tmp/${case_name}-build.log 2>&1
time sh -lc '$verify_cmd' > /tmp/${case_name}-verify.log 2>&1
"

    if "$ISH_BIN" -f "$rootfs" /bin/sh -lc "$guest_script"; then
        "$ISH_BIN" -f "$rootfs" /bin/sh -lc "cat /tmp/${case_name}-build.log" >"$out/build.log"
        "$ISH_BIN" -f "$rootfs" /bin/sh -lc "cat /tmp/${case_name}-verify.log" >"$out/verify.log"
        summary_add "$case_name" "external" "ok" "$rootfs" "$guest_dir" "$out/build.log" "$url"
    else
        "$ISH_BIN" -f "$rootfs" /bin/sh -lc "cat /tmp/${case_name}-build.log 2>/dev/null || true" >"$out/build.log"
        "$ISH_BIN" -f "$rootfs" /bin/sh -lc "cat /tmp/${case_name}-verify.log 2>/dev/null || true" >"$out/verify.log"
        summary_add "$case_name" "external" "failed" "$rootfs" "$guest_dir" "$out/build.log" "$url"
    fi
}

summary_init
for case_name in $MATRIX_CASES; do
    case "$case_name" in
        smoke)
            run_smoke_case
            ;;
        hey)
            run_external_case "hey" "https://github.com/rakyll/hey" "/tmp/hey" "go build -o /tmp/hey-bin ." "/tmp/hey-bin -h"
            ;;
        cobra-cli)
            run_external_case "cobra-cli" "https://github.com/spf13/cobra-cli" "/tmp/cobra-cli" "go build -o /tmp/cobra-cli-bin ." "/tmp/cobra-cli-bin --help"
            ;;
        *)
            printf 'unknown matrix case: %s\n' "$case_name" >&2
            exit 2
            ;;
    esac
done
summary_render_md

printf 'wrote release matrix to %s\n' "$OUTDIR"
