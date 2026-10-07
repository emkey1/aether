#!/usr/bin/env bash
# The AETHER_EXPERIMENT arms (src/aether/experiment.h) on their fixtures.
# Each row: fixture, AETHER_EXPERIMENT value, expected exit status, and either
# the exact stdout or "code:<CODE>" for the first diagnostic code on stderr.
# The empty experiment is the shipped compiler; it must match the arm named
# for the shipped rule exactly.
set -uo pipefail
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
AETHER_BIN="${AETHER_BIN:-$DIR/../../build/aether}"
OUT="$(mktemp -d "${TMPDIR:-/tmp}/aether_experiments.XXXXXX")"
trap 'rm -rf "$OUT"' EXIT
fail=0
pass=0

check() {
    local fixture="$1" exp="$2" want_rc="$3" want="$4"
    local out err rc
    out="$(cd "$OUT" && HOME="$OUT" AETHER_EXPERIMENT="$exp" "$AETHER_BIN" --no-cache \
        "$DIR/$fixture" 2>"$OUT/err" </dev/null)"
    rc=$?
    err="$(cat "$OUT/err")"
    local got="$out"
    if [ "${want#code:}" != "$want" ]; then
        got="code:$(printf '%s\n' "$err" | grep -o '\[[A-Z]*-[0-9]*\]' | head -1 | tr -d '[]')"
    fi
    if [ "$rc" != "$want_rc" ] || [ "$got" != "$want" ]; then
        echo "FAIL $fixture AETHER_EXPERIMENT='$exp': rc=$rc (want $want_rc) got '$got' want '$want'"
        [ -n "$err" ] && printf '%s\n' "$err" | head -3
        fail=$((fail + 1))
    else
        pass=$((pass + 1))
    fi
}

# D4: Int / Int (W8-08).
check div_demand.aether ""             0 "x=3 half=3 loops=4 pick=40"
check div_demand.aether "div=current"  0 "x=3 half=3 loops=4 pick=40"
check div_demand.aether "div=int"      0 "x=3 half=3 loops=3 pick=40"
check div_demand.aether "div=intplus"  0 "x=3 half=3 loops=3 pick=40"
check div_demand.aether "div=realplus" 1 "code:DIV-001"
check div_demand.aether "div=ctx"      0 "x=3 half=3 loops=3 pick=40"
check div_differ.aether ""             0 "avg=3.500000 twice=7"
check div_differ.aether "div=current"  0 "avg=3.500000 twice=7"
check div_differ.aether "div=int"      0 "avg=3.000000 twice=6"
check div_differ.aether "div=intplus"  1 "code:DIV-001"
check div_differ.aether "div=realplus" 0 "avg=3.500000 twice=7"
check div_differ.aether "div=ctx"      1 "code:DIV-001"

# A typo must not silently measure the default.
(cd "$OUT" && AETHER_EXPERIMENT="div=nope" "$AETHER_BIN" --no-cache "$DIR/div_demand.aether" \
    >/dev/null 2>&1 </dev/null)
if [ $? -eq 2 ]; then pass=$((pass + 1)); else echo "FAIL unknown experiment value did not exit 2"; fail=$((fail + 1)); fi

echo "experiment arms: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
