#!/usr/bin/env bash
# Self-test for tools/run_fixtures.py: a copy of tests/fx is mutated one sidecar
# class at a time, and each mutation must fail the runner (and the .cap gate
# must skip). Exit 1 when a mutation goes unnoticed or the baseline is red.
set -u
L="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
AETHER_BIN="${AETHER_BIN:-$L/build/aether}"
T="$(mktemp -d "${TMPDIR:-/tmp}/aether_fx_selftest.XXXXXX")"; trap 'rm -rf "$T"' EXIT
bad=0
run() { python3 "$L/tools/run_fixtures.py" --aether "$AETHER_BIN" --fx-dir "$T/fx" "$@" 2>&1; }
fresh() { rm -rf "$T/fx"; cp -R "$L/tests/fx" "$T/fx"; }
expect_fail() { # label, fixture
    if out=$(run "$2"); then echo "MISSED  $1: $out"; bad=1; else echo "caught  $1: $(echo "$out" | grep FAIL | head -1)"; fi
}
fresh; if run >/dev/null; then echo "baseline green"; else echo "BASELINE RED"; bad=1; fi
fresh; printf '5 2 c\n' > $T/fx/basic/record_method.out; expect_fail ".out exact" basic/record_method
fresh; printf '5 2 b\nextra\n' > $T/fx/basic/record_method.out; expect_fail ".out extra line" basic/record_method
fresh; printf 'branch 1 sq=1\nbranch 2 sq=4\nbranch 2 sq=4\njoined\n' > $T/fx/par/par_branch_lines.out; expect_fail ".unordered multiset" par/par_branch_lines
fresh; rm $T/fx/par/par_branch_lines.unordered; printf 'joined\nbranch 1 sq=1\nbranch 2 sq=4\nbranch 3 sq=9\n' > $T/fx/par/par_branch_lines.out; expect_fail ".out ordered (no .unordered)" par/par_branch_lines
fresh; printf "identifier 'total' not in scope\n" > $T/fx/diag/scope_unknown_fail.err; expect_fail ".err required" diag/scope_unknown_fail
fresh; printf "warning: [NARROW-001]\n!NARROW\n" > $T/fx/diag/narrow_warn_pass.err; expect_fail ".err forbidden" diag/narrow_warn_pass
fresh; printf 'SCOPE-002\n' > $T/fx/diag/scope_unknown_fail.codes; expect_fail ".codes set" diag/scope_unknown_fail
fresh; printf '' > $T/fx/diag/narrow_warn_pass.codes; expect_fail ".codes warning missing" diag/narrow_warn_pass
fresh; printf '2\n' > $T/fx/diag/scope_unknown_fail.rc; expect_fail ".rc" diag/scope_unknown_fail
fresh; printf '3\n4\n\n' > $T/fx/basic/stdin_sum.in; expect_fail ".in" basic/stdin_sum
fresh; echo "--no-run" > $T/fx/basic/record_method.flags; expect_fail ".flags" basic/record_method
fresh; mkdir -p $T/fx/diag; printf 'fn main() -> Void {\n    let x: Int = 1;\n    x.frobnicate();\n}\n' > $T/fx/diag/null_code.aether; printf "FIELD-002\n" > $T/fx/diag/null_code.codes; printf '1\n' > $T/fx/diag/null_code.rc; expect_fail "code:null forbidden" diag/null_code
fresh; rm $T/fx/basic/record_method.out; expect_fail "asserts nothing" basic/record_method
fresh; echo sdl > $T/fx/net/http_session_denied.cap; out=$(run net/http_session_denied)
case "$out" in
    SKIP*) echo "caught  .cap gate: $(echo "$out" | head -1)" ;;
    *) echo "MISSED  .cap gate: $out"; bad=1 ;;
esac
if [ "$bad" -ne 0 ]; then echo "run_fixtures self-test: FAILED"; exit 1; fi
echo "run_fixtures self-test: every sidecar mutation caught"
