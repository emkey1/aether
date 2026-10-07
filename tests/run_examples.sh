#!/usr/bin/env bash
# Compile-check every shipped example so the examples tree cannot rot silently.
# Uses --no-run: this lap asserts the examples PARSE + COMPILE against the
# current frontend. Keep-going: all failures are reported, then the lap exits
# nonzero if any example failed.
#
# --run (CTest aether_examples_run) RUNS every example program instead, from a
# private copy of its own directory (so cwd-relative payloads resolve and
# nothing is written into the tree), and asserts exit 0 within 20 s. The
# "best example files to copy from" (docs/aether_doc_maintenance.md) must also
# print exactly tests/example_goldens/<dir>/<name>.out. Module files (`mod`)
# are not programs and are skipped. Capability gates: ai_helpers needs the
# OpenAI ext builtin, sockets needs the curl build (its port is rewritten to a
# free one in the copy), http_weather needs curl and live network, so it runs
# only with AETHER_EXAMPLES_NETWORK=1. --update rewrites the goldens.
set -uo pipefail

MODE=compile
UPDATE=0
for arg in "$@"; do
    case "$arg" in
        --run) MODE=run ;;
        --update) UPDATE=1 ;;
        *) echo "usage: $0 [--run [--update]]" >&2; exit 2 ;;
    esac
done

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EX_DIR="$(cd "$SCRIPT_DIR/../examples" && pwd)"
AETHER_BIN="${AETHER_BIN:-$SCRIPT_DIR/../build/aether}"

if [ ! -x "$AETHER_BIN" ]; then
    echo "AETHER_BIN not executable: $AETHER_BIN" >&2
    exit 1
fi

# Capability probe, mirroring tests/run.sh: AI examples need the OpenAI
# extended builtin, which minimal builds omit.
HAS_OPENAI=0
if "$AETHER_BIN" --dump-ext-builtins 2>/dev/null | grep -qi 'OpenAIChatCompletions'; then
    HAS_OPENAI=1
fi

pass=0
skip=0
fail=0
failed_names=""

GOLDEN_DIR="$SCRIPT_DIR/example_goldens"
BEST_EXAMPLES="base/hello base/contracts base/contract_layouts base/inferred_decls
base/function_inference base/object_inference base/self_mutation base/toon_access
base/toon_defaults showcase/agent_report"
if [ "$MODE" = run ]; then
    CAPS="$(python3 "$SCRIPT_DIR/../tools/aether_caps.py" "$AETHER_BIN" 2>/dev/null || true)"
    RUN_DIR="$(mktemp -d "${TMPDIR:-/tmp}/aether_examples.XXXXXX")"
    trap 'rm -rf "$RUN_DIR"' EXIT
fi
has_cap() { case " $CAPS " in *" $1 "*) return 0 ;; esac; return 1; }
free_port() {
    python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])'
}

# Run one example from a private copy of its directory; exit 0 within 20 s, and
# the golden stdout for the best example files.
run_one() {
    local src="$1"
    local name="$2"
    local dir base copy out rc golden
    dir="$(dirname "$src")"
    base="$(basename "$src")"
    if grep -qE '^[[:space:]]*mod[[:space:]]' "$src"; then
        return  # a module, not a program
    fi
    case "$name" in
        base/ai_helpers)
            if [ "$HAS_OPENAI" != "1" ]; then
                echo "[skip] $name (OpenAI ext-builtin not present)"; skip=$((skip + 1)); return
            fi ;;
        base/sockets)
            if ! has_cap curl; then
                echo "[skip] $name (needs the curl build)"; skip=$((skip + 1)); return
            fi ;;
        base/http_weather)
            if ! has_cap curl || [ "${AETHER_EXAMPLES_NETWORK:-0}" != "1" ]; then
                echo "[skip] $name (live network: set AETHER_EXAMPLES_NETWORK=1 on a curl build)"
                skip=$((skip + 1)); return
            fi ;;
    esac
    copy="$RUN_DIR/$(dirname "$name")"
    if [ ! -d "$copy" ]; then
        mkdir -p "$copy" && cp -R "$dir"/. "$copy"/
    fi
    if [ "$name" = base/sockets ]; then
        sed -E "s/^const PORT: Int = [0-9]+;/const PORT: Int = $(free_port);/" "$src" >"$copy/$base"
    fi
    out="$RUN_DIR/out.txt"
    (cd "$copy" && perl -e 'alarm shift; exec @ARGV' 20 "$AETHER_BIN" --no-cache "$base" \
        </dev/null >"$out" 2>"$RUN_DIR/err.txt")
    rc=$?
    golden="$GOLDEN_DIR/$name.out"
    case " $(echo $BEST_EXAMPLES) " in
        *" $name "*)
            if [ "$UPDATE" = 1 ] && [ "$rc" = 0 ]; then
                mkdir -p "$(dirname "$golden")"
                if ! cmp -s "$out" "$golden" 2>/dev/null; then
                    diff -u "$golden" "$out" 2>/dev/null || true
                    cp "$out" "$golden"
                    echo "[updated] $golden"
                fi
            fi ;;
        *) golden="" ;;
    esac
    if [ "$rc" != 0 ]; then
        echo "[FAIL] $name (exit $rc)"
        sed 's/^/       /' "$RUN_DIR/err.txt" | head -5
    elif [ -n "$golden" ] && ! cmp -s "$golden" "$out"; then
        echo "[FAIL] $name (stdout differs from tests/example_goldens/$name.out)"
        diff -u "$golden" "$out" | head -20 | sed 's/^/       /'
        rc=1
    fi
    if [ "$rc" = 0 ]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        failed_names="$failed_names $name"
    fi
}

check_one() {
    local src="$1"
    local name="$2"
    if [ "$MODE" = run ]; then
        run_one "$src" "$name"
        return
    fi

    case "$name" in
        */ai_helpers)
            if [ "$HAS_OPENAI" != "1" ]; then
                echo "[skip] $name (OpenAI ext-builtin not present)"
                skip=$((skip + 1))
                return
            fi
            ;;
    esac

    local out
    if out=$("$AETHER_BIN" --no-cache --no-run "$src" 2>&1); then
        pass=$((pass + 1))
    else
        echo "[FAIL] $name"
        echo "$out" | sed 's/^/       /'
        fail=$((fail + 1))
        failed_names="$failed_names $name"
    fi
}

# base/: one program per regular file (payload .json files and README excluded).
for src in "$EX_DIR"/base/*; do
    [ -f "$src" ] || continue
    name="$(basename "$src")"
    case "$name" in
        README.md|*.json) continue ;;
    esac
    check_one "$src" "base/$name"
done

# showcase/: every program (run.sh executes agent_report end to end; here we at
# least compile-check all of them, including gradebook). Showcase programs are
# extensionless like base/, so match on "not README, not .json" the same way --
# an earlier `-name '*.aether'` filter here silently matched nothing at all.
while IFS= read -r src; do
    rel="${src#"$EX_DIR"/}"
    check_one "$src" "$rel"
done < <(find "$EX_DIR/showcase" -type f ! -name 'README.md' ! -name '*.json' | sort)

# sdl/: only meaningful on SDL-enabled builds; skipped by default.
if [ "${AETHER_EXAMPLES_SDL:-0}" = "1" ]; then
    while IFS= read -r src; do
        rel="${src#"$EX_DIR"/}"
        check_one "$src" "$rel"
    done < <(find "$EX_DIR/sdl" -name '*.aether' -type f 2>/dev/null | sort)
else
    echo "[skip] sdl/ (set AETHER_EXAMPLES_SDL=1 on SDL builds)"
    skip=$((skip + 1))
fi

echo "examples lap ($MODE): $pass passed, $skip skipped, $fail failed"
if [ "$fail" -ne 0 ]; then
    echo "failing examples:$failed_names" >&2
    exit 1
fi
