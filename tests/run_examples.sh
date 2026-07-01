#!/usr/bin/env bash
# Compile-check every shipped example so the examples tree cannot rot silently.
# Uses --no-run: this lap asserts the examples PARSE + COMPILE against the
# current frontend; runtime behavior for a representative subset is covered by
# tests/run.sh (which executes the showcase end to end). Keep-going: all
# failures are reported, then the lap exits nonzero if any example failed.
set -uo pipefail

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

check_one() {
    local src="$1"
    local name="$2"

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

# showcase/: every .aether program (run.sh executes agent_report end to end;
# here we at least compile-check all of them, including gradebook).
while IFS= read -r src; do
    rel="${src#"$EX_DIR"/}"
    check_one "$src" "$rel"
done < <(find "$EX_DIR/showcase" -name '*.aether' -type f | sort)

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

echo "examples lap: $pass passed, $skip skipped, $fail failed"
if [ "$fail" -ne 0 ]; then
    echo "failing examples:$failed_names" >&2
    exit 1
fi
