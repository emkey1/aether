#!/usr/bin/env bash
# tools/bump_pins.sh -- move aether's external/ pins in one verified, atomic
# commit. It never pushes.
#
# Pins used to be moved by hand: SHAs typed from memory (c56c942bb17c never
# existed), pins moved from checkouts that lagged origin, and a front-end
# change committed apart from the pins it needed (0e51e8c, bbfd7a2), which
# left main failing tests/run.sh and broke bisect. This script:
#   1. fetches, and refuses when the current branch is behind its origin twin;
#   2. resolves each REF inside external/<sub> after fetching it (a typed SHA
#      that does not resolve is rejected), and refuses a commit that is not on
#      that submodule's origin/main, or that would move a pin backwards;
#   3. prints the pscal-core rea standalone builds (its FetchContent GIT_TAG)
#      next to aether's pin, which wins;
#   4. checks out the new pins, builds the build dir and runs
#      `ctest -LE 'stress|metric'`; on failure it puts the old pins back and
#      refuses, so a dependent front-end change has to be staged with them;
#   5. optionally runs tools/pin_gate.sh (--gate) or takes its log (--gate-log)
#      and attaches the summary to the commit message;
#   6. commits the pins together with whatever front-end change is already
#      staged, with the upstream `git log` of each moved pin in the body, and
#      flags upstream subjects that may change how programs compile or run
#      (VERSION + CHANGELOG, which W2-10 checks);
#   7. prints the push and umbrella follow-ups, which it leaves to you.
#
# Usage: tools/bump_pins.sh [--core REF] [--rea REF] [options]   (run inside the aether checkout)
#   --core REF / --rea REF   new pin (branch, tag or SHA, resolved after fetch); at least one
#   --subject TEXT           commit subject (required when a front-end change is staged)
#   --body TEXT              paragraph that opens the commit body (the why)
#   --trailer TEXT           line appended to the message (repeatable), e.g. Co-Authored-By
#   --gate                   run tools/pin_gate.sh on the new pins and attach its summary
#   --gate-args "ARGS"       extra arguments for pin_gate.sh (e.g. "--skip umbrella")
#   --gate-log FILE          attach an existing pin_gate summary (must name the new SHAs)
#   --build-dir DIR          build directory to build and test (default: build)
#   --ctest-exclude REGEX    ctest -LE value (default: stress|metric)
#   --dry-run                verify and print the commit message; change nothing
# Exit status: 0 committed (or nothing to do / dry run), 1 refused, 2 usage error.
set -uo pipefail

die() { echo "bump_pins: $*" >&2; exit 1; }
usage_die() { echo "bump_pins: $*" >&2; sed -n '/^# Usage:/,/^# Exit status/p' "$0" | sed 's/^# \{0,1\}//' >&2; exit 2; }
say() { echo "[bump_pins] $*"; }

CORE_REF="" REA_REF="" SUBJECT="" BODY="" TRAILERS=() GATE=0 GATE_ARGS="" GATE_LOG=""
BUILD_DIR="build" CTEST_EXCLUDE="stress|metric" DRY=0
while [ $# -gt 0 ]; do
    case "$1" in
        --core) CORE_REF="${2:?}"; shift ;;
        --rea) REA_REF="${2:?}"; shift ;;
        --subject) SUBJECT="${2:?}"; shift ;;
        --body) BODY="${2:?}"; shift ;;
        --trailer) TRAILERS+=("${2:?}"); shift ;;
        --gate) GATE=1 ;;
        --gate-args) GATE_ARGS="${2?}"; shift ;;
        --gate-log) GATE_LOG="${2:?}"; shift ;;
        --build-dir) BUILD_DIR="${2:?}"; shift ;;
        --ctest-exclude) CTEST_EXCLUDE="${2:?}"; shift ;;
        --dry-run) DRY=1 ;;
        -h|--help) sed -n '/^# Usage:/,/^# Exit status/p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) usage_die "unknown argument: $1" ;;
    esac
    shift
done
[ -n "$CORE_REF$REA_REF" ] || usage_die "give --core REF and/or --rea REF"
[ $GATE -eq 1 ] && [ -n "$GATE_LOG" ] && usage_die "--gate and --gate-log are exclusive"

ROOT="$(git rev-parse --show-toplevel 2>/dev/null)" || die "not inside a git checkout"
cd "$ROOT" || die "cannot cd to $ROOT"
[ -f .gitmodules ] && git config -f .gitmodules --get submodule.external/pscal-core.path >/dev/null \
    || die "$ROOT is not an aether checkout (no external/pscal-core in .gitmodules)"
case "$BUILD_DIR" in /*) ;; *) BUILD_DIR="$ROOT/$BUILD_DIR" ;; esac
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# ---- 1. up to date with origin ---------------------------------------------------
BRANCH="$(git symbolic-ref --quiet --short HEAD)" || die "HEAD is detached; check out main first"
say "fetching origin"
git fetch --quiet origin || die "git fetch origin failed"
if git rev-parse --verify --quiet "origin/$BRANCH" >/dev/null; then
    behind="$(git rev-list --count "HEAD..origin/$BRANCH")"
    [ "$behind" -eq 0 ] || die "$BRANCH is $behind commit(s) behind origin/$BRANCH: rebase first (git pull --rebase), then rerun"
else
    die "no origin/$BRANCH to compare with"
fi

# ---- worktree preconditions -----------------------------------------------------------
# The tests run on the working tree, so it must hold exactly what the commit
# will: staged changes plus the new pins, nothing unstaged or untracked.
if ! git diff --quiet -- . ':(exclude)external'; then
    die "unstaged changes outside external/: stage what belongs in this commit (git add) or stash the rest"
fi
untracked="$(git ls-files --others --exclude-standard -- . ':(exclude)external' | head -n 5)"
[ -z "$untracked" ] || die "untracked files the tests could see but the commit would leave out: $(echo "$untracked" | tr '\n' ' ')"
STAGED_FE="$(git diff --cached --name-only -- . ':(exclude)external')"

# ---- 2. resolve and verify each new pin -------------------------------------------------
SUBS=() OLD=() NEW=()
resolve_pin() {  # resolve_pin SUBPATH REF
    local sub="$1" ref="$2" old sha
    [ -e "$sub/.git" ] || die "$sub is not checked out (git submodule update --init $sub)"
    [ -z "$(git -C "$sub" status --porcelain)" ] || die "$sub has local changes; commit them upstream or clean them"
    say "fetching $sub"
    git -C "$sub" fetch --quiet --tags origin || die "git fetch in $sub failed"
    sha="$(git -C "$sub" rev-parse --verify --quiet "${ref}^{commit}" \
        || git -C "$sub" rev-parse --verify --quiet "origin/${ref}^{commit}")" \
        || die "$sub: '$ref' does not resolve to a commit after fetching origin. Typed SHAs are not trusted: pass a ref, or a SHA copied from git log in $sub."
    if ! git -C "$sub" branch -r --contains "$sha" | sed 's/^[ *]*//' | grep -qx 'origin/main'; then
        die "$sub: ${sha:0:12} is not on origin/main. Land it on main upstream first (a core-N branch fast-forwards main at the cut), then rerun."
    fi
    old="$(git ls-tree HEAD -- "$sub" | awk '{print $3}')"
    if [ "$old" = "$sha" ]; then
        say "$sub already pinned at ${sha:0:12}"
        return 0
    fi
    if ! git -C "$sub" merge-base --is-ancestor "$old" "$sha" 2>/dev/null; then
        die "$sub: ${sha:0:12} does not descend from the current pin ${old:0:12}; refusing to move a pin backwards or sideways"
    fi
    SUBS+=("$sub"); OLD+=("$old"); NEW+=("$sha")
}
[ -n "$CORE_REF" ] && resolve_pin external/pscal-core "$CORE_REF"
[ -n "$REA_REF" ] && resolve_pin external/rea "$REA_REF"
if [ ${#SUBS[@]} -eq 0 ]; then
    say "nothing to bump"
    exit 0
fi

final_pin() {  # final_pin SUBPATH: the pin this commit records
    local i
    for i in "${!SUBS[@]}"; do [ "${SUBS[$i]}" = "$1" ] && { echo "${NEW[$i]}"; return; }; done
    git ls-tree HEAD -- "$1" | awk '{print $3}'
}
CORE_FINAL="$(final_pin external/pscal-core)"
REA_FINAL="$(final_pin external/rea)"

# A supplied gate log must be a passing run on exactly these pins.
GATE_LOG_TEXT=""
if [ -n "$GATE_LOG" ]; then
    [ -f "$GATE_LOG" ] || die "no such gate log: $GATE_LOG"
    grep -q "^  pscal-core ${CORE_FINAL:0:12} " "$GATE_LOG" && grep -q "^  rea        ${REA_FINAL:0:12} " "$GATE_LOG" \
        || die "$GATE_LOG is not a pin_gate summary for pscal-core ${CORE_FINAL:0:12} / rea ${REA_FINAL:0:12}"
    grep -q '^pin_gate PASS' "$GATE_LOG" || die "$GATE_LOG does not record a passing gate"
    GATE_LOG_TEXT="$(sed -n '/^pin_gate PASS/,$p' "$GATE_LOG" | grep -v '^  logs:')"
fi

# ---- 3. rea's own pscal-core ----------------------------------------------------------------
REA_TAG="$(git -C external/rea show "$REA_FINAL:CMakeLists.txt" 2>/dev/null \
    | sed -n '/FetchContent_Declare/,/)/{s/^[[:space:]]*GIT_TAG[[:space:]]*\([^[:space:])]*\).*/\1/p;}' | head -n 1)"
REA_TAG_SHA="$(git -C external/pscal-core rev-parse --verify --quiet "origin/${REA_TAG:-main}^{commit}" 2>/dev/null \
    || git -C external/pscal-core rev-parse --verify --quiet "${REA_TAG:-main}^{commit}" 2>/dev/null || echo "?")"
REA_LINE="rea standalone builds pscal-core GIT_TAG ${REA_TAG:-?} (now ${REA_TAG_SHA:0:12}); aether pins ${CORE_FINAL:0:12}, and aether's pin wins (it declares pscal-core first)."
say "$REA_LINE"

# ---- commit message ---------------------------------------------------------------------------
MSG="$(mktemp "${TMPDIR:-/tmp}/bump_pins_msg.XXXXXX")"
trap 'rm -f "$MSG"' EXIT
FLAGGED=""
pin_summary=""
for i in "${!SUBS[@]}"; do
    sub="${SUBS[$i]}"; name="${sub#external/}"
    n="$(git -C "$sub" rev-list --count "${OLD[$i]}..${NEW[$i]}")"
    pin_summary="$pin_summary${pin_summary:+, }$name ${OLD[$i]:0:7}..${NEW[$i]:0:7}"
    {
        echo "  $sub ${OLD[$i]:0:12} -> ${NEW[$i]:0:12} ($n commit(s)):"
        git -C "$sub" log --format='    %h %s' "${OLD[$i]}..${NEW[$i]}"
    } >>"$MSG.pins"
    # Subjects whose area is not docs/tests/build/ci/chore/tools may change
    # what programs compile to or print.
    f="$(git -C "$sub" log --format='%h %s' "${OLD[$i]}..${NEW[$i]}" \
        | grep -v -E '^[0-9a-f]+ (docs?|tests?|build|ci|chore|tools?)(\([^)]*\))?:' || true)"
    [ -n "$f" ] && FLAGGED="$FLAGGED$(echo "$f" | sed "s#^#    $name #")"$'\n'
done
if [ -n "$STAGED_FE" ] && [ -z "$SUBJECT" ]; then
    die "a front-end change is staged ($(echo "$STAGED_FE" | tr '\n' ' ')): pass --subject describing it"
fi
[ -n "$SUBJECT" ] || SUBJECT="build: bump external pins ($pin_summary)"
VERSION_STAGED=0
echo "$STAGED_FE" | grep -qx VERSION && VERSION_STAGED=1
{
    echo "$SUBJECT"
    echo
    if [ -n "$BODY" ]; then echo "$BODY"; echo; fi
    echo "Pins (tools/bump_pins.sh):"
    cat "$MSG.pins"
    echo "  $REA_LINE"
    if [ -n "$STAGED_FE" ]; then
        echo
        echo "Front-end changes committed with the pins:"
        echo "$STAGED_FE" | sed 's/^/  /'
    fi
    if [ -n "$FLAGGED" ]; then
        echo
        echo "Upstream subjects that may change how programs compile or run:"
        printf '%s' "$FLAGGED"
        if [ $VERSION_STAGED -eq 1 ]; then
            echo "  VERSION is staged with this commit."
        else
            echo "  VERSION is not staged: if any of these is a language change, run"
            echo "  tools/bump_version.py and add a CHANGELOG.md entry before committing."
        fi
    fi
} >"$MSG"
rm -f "$MSG.pins"

if [ -n "$FLAGGED" ] && [ $VERSION_STAGED -eq 0 ]; then
    say "WARNING: upstream changes may be language changes and VERSION is not staged:"
    printf '%s' "$FLAGGED"
fi
if [ $DRY -eq 1 ]; then
    say "dry run: would check out the pins, build $BUILD_DIR, run ctest -LE '$CTEST_EXCLUDE' and commit:"
    echo "----"; cat "$MSG"; echo "----"
    exit 0
fi

# ---- 4. build and test with the new pins --------------------------------------------------------
restore_pins() {
    local i
    for i in "${!SUBS[@]}"; do git -C "${SUBS[$i]}" checkout --quiet --detach "${OLD[$i]}"; done
}
for i in "${!SUBS[@]}"; do
    git -C "${SUBS[$i]}" checkout --quiet --detach "${NEW[$i]}" || { restore_pins; die "cannot check out ${NEW[$i]} in ${SUBS[$i]}"; }
done
if [ -f "$BUILD_DIR/CMakeCache.txt" ]; then
    for v in PSCAL_CORE:external/pscal-core REA:external/rea; do
        src="$(sed -n "s/^FETCHCONTENT_SOURCE_DIR_${v%%:*}:[A-Z]*=//p" "$BUILD_DIR/CMakeCache.txt")"
        if [ -n "$src" ] && [ "$(cd "$src" 2>/dev/null && pwd -P)" != "$(cd "$ROOT/${v#*:}" && pwd -P)" ]; then
            restore_pins
            die "$BUILD_DIR builds ${v#*:} from $src, not from the pins: use a build dir configured over external/ (--build-dir)"
        fi
    done
else
    say "configuring $BUILD_DIR"
    cmake -S "$ROOT" -B "$BUILD_DIR" >/dev/null || { restore_pins; die "cmake configure failed"; }
fi
TESTLOG="$(mktemp "${TMPDIR:-/tmp}/bump_pins_ctest.XXXXXX")"
say "building $BUILD_DIR and running ctest -LE '$CTEST_EXCLUDE' (log: $TESTLOG)"
if ! { cmake --build "$BUILD_DIR" -j && ctest --test-dir "$BUILD_DIR" -LE "$CTEST_EXCLUDE" --output-on-failure; } >"$TESTLOG" 2>&1; then
    grep -E 'tests passed|\(Failed\)|\*\*\*Failed|Error' "$TESTLOG" | tail -n 15
    restore_pins
    die "build or ctest fails with the new pins (log: $TESTLOG); the old pins are checked out again and nothing was staged. If a front-end change goes with these pins, stage it (git add) and rerun; if the staged change needs another pin too, move it in the same run. The change and its pins land in one commit."
fi
CTEST_LINE="$(grep 'tests passed' "$TESTLOG" | tail -n 1)"
say "ctest: $CTEST_LINE"

# ---- 5. pin_gate ---------------------------------------------------------------------------------
GATE_SUMMARY=""
if [ $GATE -eq 1 ]; then
    say "running tools/pin_gate.sh"
    # shellcheck disable=SC2086
    if ! "$SCRIPT_DIR/pin_gate.sh" --aether-src "$ROOT" --core "$CORE_FINAL" --rea "$REA_FINAL" \
            --core-repo "$ROOT/external/pscal-core" --rea-repo "$ROOT/external/rea" $GATE_ARGS >"$TESTLOG.gate" 2>&1; then
        tail -n 30 "$TESTLOG.gate"
        restore_pins
        die "pin_gate fails on the new pins (log: $TESTLOG.gate)"
    fi
    GATE_SUMMARY="$(sed -n '/^pin_gate PASS/,/^  logs:/p' "$TESTLOG.gate" | grep -v '^  logs:')"
elif [ -n "$GATE_LOG" ]; then
    GATE_SUMMARY="$GATE_LOG_TEXT"
fi
{
    echo
    echo "Verification: cmake --build && ctest -LE '$CTEST_EXCLUDE': $CTEST_LINE"
    if [ -n "$GATE_SUMMARY" ]; then
        echo
        echo "$GATE_SUMMARY"
    else
        echo "pin_gate: not run (attach it with --gate or --gate-log)"
    fi
    if [ ${#TRAILERS[@]} -gt 0 ]; then
        echo
        printf '%s\n' "${TRAILERS[@]}"
    fi
} >>"$MSG"
[ -n "$GATE_SUMMARY" ] || say "WARNING: no pin_gate log attached (--gate or --gate-log)"

# ---- 6. one commit: staged front-end change + pins ----------------------------------------------
for sub in "${SUBS[@]}"; do git add "$sub"; done
if ! git commit --quiet -F "$MSG"; then
    git reset --quiet -- "${SUBS[@]}"
    restore_pins
    die "git commit failed; the pins are unstaged and the old ones checked out again"
fi
rm -f "$TESTLOG" "$TESTLOG.gate"
say "committed $(git rev-parse --short HEAD): $SUBJECT"

# ---- 7. follow-ups ------------------------------------------------------------------------------------
cat <<EOF
[bump_pins] Not done here (this script never pushes):
  1. git push origin $BRANCH
  2. umbrella: tools/sync_aether_canonical_repo.sh moves components/aether to the pushed commit;
     tools/bump_pscal_core.sh ${CORE_FINAL:0:12} moves components/pscal-core and checks the
     nested pin with it; components/rea goes to ${REA_FINAL:0:12} in the same umbrella commit.
EOF
