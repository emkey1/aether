#!/usr/bin/env bash
# tools/pin_gate.sh -- gate a candidate pscal-core/rea pin pair before aether
# commits it.
#
# Two silent Aether regressions arrived through pin bumps (INT32 truncation,
# pscal-core 95efdcb; the per-access ArrayObj leak, 62d4139), and twice a pin
# moved alone and left main failing tests. This script builds aether from the
# working tree against scratch checkouts of the candidate SHAs, never touching
# external/ or the repos it clones from, and runs:
#
#   build        aether, from this working tree, over the candidates
#   conformance  tests/backend_conformance (known engine defects are
#                expected failures; one that passes fails the gate until its
#                manifest line flips to `pass`)
#   runsh        tests/run.sh as written (--no-cache)
#   runsh-warm   tests/run.sh with --no-cache stripped, twice on one private
#                cache: a cold run that fills it, then a warm run
#   rea          rea's own tests/run.sh, rea built standalone at the candidate
#   umbrella     with --umbrella or PSCAL_UMBRELLA: a scratch umbrella tree with
#                components/ at the candidates (aether = this working tree),
#                built, then ctest pascal/clike/rea/aether/pscalvm_frontend/
#                json2bc, the exsh and pscalasm suites, vm_fx_policy and
#                vm_thread_stress. Skipped, not failed, without an umbrella.
#   d19          the D19 Pascal/CLike wrap probes (report only), with the
#                umbrella's pascal/clike or --pascal-bin/--clike-bin
#   corpus-ab    tools/corpus_ab.py, the base binary (aether HEAD at HEAD's
#                pins, or --baseline-bin) against the candidate over a corpus
#                directory (--corpus, default the umbrella's
#                Tests/aether_specialization/corpus_candidates)
#   asan         opt-in (--asan): an AddressSanitizer build running tests/run.sh
#                and the conformance pack, for releases that touch ownership
#
# The summary at the end (also written to <work>/pin_gate.summary) is the gate
# log that goes into the pin-bump commit message; tools/bump_pins.sh --gate
# attaches it. Exit status: 0 every selected step passed or was skipped, 1 a
# step failed, 2 usage or setup error.
set -uo pipefail

usage() {
    cat <<'EOF'
Usage: tools/pin_gate.sh [options]
  --aether-src DIR    aether working tree to test (default: the checkout holding this script)
  --core REF          pscal-core candidate (default: the external/pscal-core gitlink in the index)
  --rea REF           rea candidate (default: the external/rea gitlink in the index)
  --core-repo SRC     where to clone pscal-core from (default: external/pscal-core when it is a
                      checkout, else the .gitmodules URL)
  --rea-repo SRC      the same for rea
  --steps LIST        comma list of steps to run (default: build,conformance,runsh,runsh-warm,
                      rea,umbrella,d19,corpus-ab)
  --skip LIST         comma list of steps to leave out
  --asan              add the asan step
  --aether-bin BIN    test this binary instead of building one (smoke runs; the summary says so)
  --umbrella DIR      umbrella checkout to copy (default: $PSCAL_UMBRELLA); read only
  --umbrella-cmake-args "ARGS"   extra configure arguments for the umbrella build
  --corpus DIR        corpus for corpus-ab (default: <umbrella>/Tests/aether_specialization/corpus_candidates)
  --corpus-args "ARGS"  extra arguments for tools/corpus_ab.py (e.g. "--limit 200")
  --baseline-bin BIN  base binary for corpus-ab (default: build aether HEAD at HEAD's pins)
  --pascal-bin BIN, --clike-bin BIN   interpreters for the d19 probes without an umbrella build
  --work DIR          scratch directory (default: a new one under $TMPDIR); kept on failure or
                      with --keep, otherwise only its logs are kept
  --keep              keep the scratch trees
  --jobs N            build parallelism (default: CPU count)
  -h, --help
EOF
}

die() { echo "pin_gate: $*" >&2; exit 2; }

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
AETHER_SRC="$(cd "$SCRIPT_DIR/.." && pwd)"   # --aether-src overrides

CORE_REF="" REA_REF="" CORE_REPO="" REA_REPO=""
STEPS="build,conformance,runsh,runsh-warm,rea,umbrella,d19,corpus-ab"
SKIP="" ASAN=0 GIVEN_BIN="" UMBRELLA="${PSCAL_UMBRELLA:-}" UMB_CMAKE_ARGS=""
CORPUS="" CORPUS_ARGS="" BASELINE_BIN="" PASCAL_BIN="" CLIKE_BIN=""
WORK="" KEEP=0 JOBS=""

while [ $# -gt 0 ]; do
    case "$1" in
        --aether-src) AETHER_SRC="$(cd "${2:?}" && pwd)" || die "no such directory: $2"; shift ;;
        --core) CORE_REF="${2:?}"; shift ;;
        --rea) REA_REF="${2:?}"; shift ;;
        --core-repo) CORE_REPO="${2:?}"; shift ;;
        --rea-repo) REA_REPO="${2:?}"; shift ;;
        --steps) STEPS="${2:?}"; shift ;;
        --skip) SKIP="${2:?}"; shift ;;
        --asan) ASAN=1 ;;
        --aether-bin) GIVEN_BIN="${2:?}"; shift ;;
        --umbrella) UMBRELLA="${2:?}"; shift ;;
        --umbrella-cmake-args) UMB_CMAKE_ARGS="${2?}"; shift ;;
        --corpus) CORPUS="${2:?}"; shift ;;
        --corpus-args) CORPUS_ARGS="${2?}"; shift ;;
        --baseline-bin) BASELINE_BIN="${2:?}"; shift ;;
        --pascal-bin) PASCAL_BIN="${2:?}"; shift ;;
        --clike-bin) CLIKE_BIN="${2:?}"; shift ;;
        --work) WORK="${2:?}"; shift ;;
        --keep) KEEP=1 ;;
        --jobs) JOBS="${2:?}"; shift ;;
        -h|--help) usage; exit 0 ;;
        *) usage >&2; die "unknown argument: $1" ;;
    esac
    shift
done
[ "$ASAN" -eq 1 ] && STEPS="$STEPS,asan"

want() {  # want STEP: selected by --steps and not by --skip
    case ",$STEPS," in *",$1,"*) ;; *) return 1 ;; esac
    case ",$SKIP," in *",$1,"*) return 1 ;; esac
    return 0
}

if [ -z "$JOBS" ]; then
    JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"
fi
if [ -z "$WORK" ]; then
    WORK="$(mktemp -d "${TMPDIR:-/tmp}/pin_gate.XXXXXX")" || die "cannot create a scratch directory"
fi
mkdir -p "$WORK/logs" || die "cannot create $WORK"
WORK="$(cd "$WORK" && pwd)"
case "$WORK" in *[[:space:]]*) die "scratch directory contains whitespace: $WORK" ;; esac
SUMMARY="$WORK/pin_gate.summary"
: > "$SUMMARY"

log() { echo "[pin_gate $(date +%H:%M:%S)] $*"; }

# ---- result bookkeeping -----------------------------------------------------
RESULTS=()   # "step|STATUS|detail"
FAILED=0
record() {   # record STEP STATUS DETAIL
    RESULTS+=("$1|$2|$3")
    [ "$2" = "FAIL" ] && FAILED=1
    log "$1: $2${3:+ -- $3}"
}

run_logged() {  # run_logged STEP CMD... : output to logs/STEP.log, status returned
    local step="$1"; shift
    local logf="$WORK/logs/$step.log"
    log "$step: running (log: logs/$step.log)"
    "$@" >"$logf" 2>&1
    local rc=$?
    if [ $rc -ne 0 ]; then
        echo "---- last 25 lines of logs/$step.log ----"
        tail -n 25 "$logf"
        echo "----"
    fi
    return $rc
}

# ---- candidates ---------------------------------------------------------------
gitlink() {  # gitlink PATH: the gitlink staged in aether's index
    git -C "$AETHER_SRC" ls-files -s -- "$1" 2>/dev/null | awk '$1 == "160000" {print $2}'
}
default_repo() {  # default_repo SUBPATH NAME
    if [ -e "$AETHER_SRC/$1/.git" ]; then
        echo "$AETHER_SRC/$1"
    else
        git -C "$AETHER_SRC" config -f .gitmodules --get "submodule.$1.url" 2>/dev/null \
            || echo "https://github.com/emkey1/$2.git"
    fi
}
[ -n "$CORE_REF" ] || CORE_REF="$(gitlink external/pscal-core)"
[ -n "$REA_REF" ] || REA_REF="$(gitlink external/rea)"
[ -n "$CORE_REF" ] || die "no --core given and no external/pscal-core gitlink in the index"
[ -n "$REA_REF" ] || die "no --rea given and no external/rea gitlink in the index"
[ -n "$CORE_REPO" ] || CORE_REPO="$(default_repo external/pscal-core pscal-core)"
[ -n "$REA_REPO" ] || REA_REPO="$(default_repo external/rea rea)"

# checkout_at SRC REF DEST: a scratch clone of SRC with REF checked out
# (detached). Prints the full SHA. A local SRC is cloned with its whole object
# store, so any commit it holds resolves; SRC itself is only read.
checkout_at() {
    local src="$1" ref="$2" dest="$3" sha
    if [ ! -d "$dest/.git" ]; then
        rm -rf "$dest"
        git clone --quiet --no-checkout "$src" "$dest" >/dev/null 2>&1 \
            || { echo "cannot clone $src" >&2; return 1; }
    fi
    sha="$(git -C "$dest" rev-parse --verify --quiet "${ref}^{commit}" 2>/dev/null \
        || git -C "$dest" rev-parse --verify --quiet "origin/${ref}^{commit}" 2>/dev/null)" \
        || { echo "'$ref' does not resolve to a commit in $src" >&2; return 1; }
    git -C "$dest" checkout --quiet --detach "$sha" >/dev/null 2>&1 \
        || { echo "cannot check out $sha in $dest" >&2; return 1; }
    echo "$sha"
}

AETHER_HEAD="$(git -C "$AETHER_SRC" rev-parse HEAD 2>/dev/null || echo unknown)"
AETHER_DIRTY="clean"
if [ -n "$(git -C "$AETHER_SRC" status --porcelain --untracked-files=no -- . ':(exclude)external' 2>/dev/null)" ]; then
    AETHER_DIRTY="with uncommitted changes"
fi

log "work dir: $WORK"
CORE_SHA="$(checkout_at "$CORE_REPO" "$CORE_REF" "$WORK/src/pscal-core")" || die "pscal-core candidate rejected"
REA_SHA="$(checkout_at "$REA_REPO" "$REA_REF" "$WORK/src/rea")" || die "rea candidate rejected"
CORE_SUBJ="$(git -C "$WORK/src/pscal-core" log -1 --format=%s "$CORE_SHA")"
REA_SUBJ="$(git -C "$WORK/src/rea" log -1 --format=%s "$REA_SHA")"
log "pscal-core candidate ${CORE_SHA:0:12} $CORE_SUBJ"
log "rea candidate        ${REA_SHA:0:12} $REA_SUBJ"
# rea standalone fetches pscal-core at its FetchContent GIT_TAG, not a pin.
REA_CORE_TAG="$(sed -n '/FetchContent_Declare/,/)/{s/^[[:space:]]*GIT_TAG[[:space:]]*\([^[:space:])]*\).*/\1/p;}' \
    "$WORK/src/rea/CMakeLists.txt" | head -n 1)"

cmake_configure() {  # retried once: a configure racing another build can fail spuriously
    cmake "$@" || { sleep 2; cmake "$@"; }
}

# ---- build ----------------------------------------------------------------------
BIN=""
if [ -n "$GIVEN_BIN" ]; then
    [ -x "$GIVEN_BIN" ] || die "--aether-bin $GIVEN_BIN is not executable"
    BIN="$(cd "$(dirname "$GIVEN_BIN")" && pwd)/$(basename "$GIVEN_BIN")"
    record build SKIP "using --aether-bin; it was NOT built from the candidates"
elif want build; then
    build_aether() {
        cmake_configure -S "$AETHER_SRC" -B "$WORK/build-aether" -DCMAKE_BUILD_TYPE=Release \
            -DFETCHCONTENT_SOURCE_DIR_PSCAL_CORE="$WORK/src/pscal-core" \
            -DFETCHCONTENT_SOURCE_DIR_REA="$WORK/src/rea" \
        && cmake --build "$WORK/build-aether" --target aether -j "$JOBS"
    }
    if run_logged build build_aether; then
        BIN="$WORK/build-aether/aether"
        record build PASS "$(shasum -a 256 "$BIN" | awk '{print substr($1,1,16)}') (sha256 prefix)"
    else
        record build FAIL "see logs/build.log"
    fi
else
    record build SKIP "not selected"
fi

need_bin() {  # need_bin STEP: skip a step when there is no binary
    if [ -z "$BIN" ]; then
        record "$1" SKIP "no aether binary (build skipped or failed)"
        return 1
    fi
}

# ---- conformance ------------------------------------------------------------------
XFAIL_LINE=""
if want conformance && need_bin conformance; then
    run_logged conformance python3 "$AETHER_SRC/tests/backend_conformance/run.py" --aether "$BIN"
    rc=$?
    XFAIL_LINE="$(grep '^expected-fail:' "$WORK/logs/conformance.log" | tail -n 1)"
    counts="$(grep '^conformance:' "$WORK/logs/conformance.log" | tail -n 1)"
    if [ $rc -eq 0 ]; then
        record conformance PASS "${counts#conformance: }"
    else
        record conformance FAIL "${counts#conformance: }; $(grep -E '^(FAIL|XPASS) ' "$WORK/logs/conformance.log" | awk '{printf "%s %s; ", $1, $2}')"
    fi
elif ! want conformance; then
    record conformance SKIP "not selected"
fi

# ---- tests/run.sh, uncached then on a warm cache ------------------------------------
runsh_status() {  # runsh_status STEP RC [EXTRA]
    local xf
    xf="$(grep -c '^\[xfail\]' "$WORK/logs/$1.log" 2>/dev/null)"
    if [ "$2" -eq 0 ]; then record "$1" PASS "${xf:-0} xfail line(s) from run.sh${3:+; $3}"
    else record "$1" FAIL "see logs/$1.log${3:+; $3}"; fi
}
if want runsh && need_bin runsh; then
    mkdir -p "$WORK/home-nocache"
    run_logged runsh env HOME="$WORK/home-nocache" AETHER_BIN="$BIN" bash "$AETHER_SRC/tests/run.sh"
    runsh_status runsh $?
elif ! want runsh; then
    record runsh SKIP "not selected"
fi
if want runsh-warm && need_bin runsh-warm; then
    WRAP="$WORK/aether-cached"
    cat >"$WRAP" <<EOF
#!/usr/bin/env bash
# pin_gate: run aether with every --no-cache dropped, so run.sh uses the cache.
args=()
for a in "\$@"; do [ "\$a" = "--no-cache" ] || args+=("\$a"); done
exec "$BIN" "\${args[@]+"\${args[@]}"}"
EOF
    chmod +x "$WRAP"
    mkdir -p "$WORK/home-cache"
    run_logged runsh-cache-cold env HOME="$WORK/home-cache" AETHER_BIN="$WRAP" bash "$AETHER_SRC/tests/run.sh"
    runsh_status runsh-cache-cold $?
    touch "$WORK/warm.marker"
    run_logged runsh-cache-warm env HOME="$WORK/home-cache" AETHER_BIN="$WRAP" bash "$AETHER_SRC/tests/run.sh"
    rc=$?
    ncache="$(find "$WORK/home-cache" -name '*.bc' 2>/dev/null | wc -l | tr -d ' ')"
    nnew="$(find "$WORK/home-cache" -name '*.bc' -newer "$WORK/warm.marker" 2>/dev/null | wc -l | tr -d ' ')"
    runsh_status runsh-cache-warm $rc "$ncache cached programs, $nnew recompiled on the warm run"
elif ! want runsh-warm; then
    record runsh-warm SKIP "not selected"
fi

# ---- rea --------------------------------------------------------------------------
if want rea; then
    build_and_test_rea() {
        cmake_configure -S "$WORK/src/rea" -B "$WORK/build-rea" -DCMAKE_BUILD_TYPE=Release \
            -DFETCHCONTENT_SOURCE_DIR_PSCAL_CORE="$WORK/src/pscal-core" \
        && cmake --build "$WORK/build-rea" --target rea -j "$JOBS" \
        && env HOME="$WORK/home-rea" REA_BIN="$WORK/build-rea/rea" bash "$WORK/src/rea/tests/run.sh"
    }
    mkdir -p "$WORK/home-rea"
    if run_logged rea build_and_test_rea; then
        record rea PASS "rea tests/run.sh at ${REA_SHA:0:12} over pscal-core ${CORE_SHA:0:12}"
    else
        record rea FAIL "see logs/rea.log"
    fi
else
    record rea SKIP "not selected"
fi

# ---- umbrella -----------------------------------------------------------------------
# A scratch umbrella tree: the umbrella's HEAD, every submodule exported from
# the umbrella's own checkout at the gitlink HEAD records, then (for the
# candidate tree) components/pscal-core and components/rea at the candidates and
# components/aether from this working tree. The umbrella checkout is only read.
# A suite that fails is run again on a base tree (the umbrella exactly as its
# HEAD pins it, built only when needed), and only failures the base does not
# share fail the gate: a gate run on a host that lacks a tool some umbrella
# test needs reports those as pre-existing instead of blaming the pins.
UMB_BIN=""
prepare_umbrella() {  # prepare_umbrella TREE candidate|base
    local tree="$1" mode="$2" path sha
    rm -rf "$tree"
    git clone --quiet --no-recurse-submodules "$UMBRELLA" "$tree" || return 1
    git -C "$tree" config -f .gitmodules --get-regexp '^submodule\..*\.path$' | while read -r _ path; do
        rm -rf "${tree:?}/$path"; mkdir -p "$tree/$path"
        case "$mode:$path" in
            candidate:components/pscal-core) git -C "$WORK/src/pscal-core" archive "$CORE_SHA" | tar -x -C "$tree/$path" ;;
            candidate:components/rea) git -C "$WORK/src/rea" archive "$REA_SHA" | tar -x -C "$tree/$path" ;;
            candidate:components/aether)
                # Tracked files as they stand in the working tree.
                (cd "$AETHER_SRC" && git ls-files -z --cached -- . ':(exclude)external' \
                    | tar --null -T - -cf - 2>/dev/null) | tar -x -C "$tree/$path" ;;
            *)
                sha="$(git -C "$tree" ls-tree HEAD -- "$path" | awk '{print $3}')"
                if [ -n "$sha" ] && git -C "$UMBRELLA/$path" cat-file -e "$sha^{commit}" 2>/dev/null; then
                    git -C "$UMBRELLA/$path" archive "$sha" | tar -x -C "$tree/$path"
                else
                    echo "note: $path not available at ${sha:-?} in the umbrella checkout; left empty"
                fi ;;
        esac
    done
}
build_umbrella() {  # build_umbrella TREE
    # shellcheck disable=SC2086
    cmake_configure -S "$1" -B "$1/build" -DCMAKE_BUILD_TYPE=Release $UMB_CMAKE_ARGS \
    && cmake --build "$1/build" -j "$JOBS"
}
UMB_SUITES="ctest Tests/run_exsh_tests.sh Tests/run_pscalasm_tests.sh Tests/vm_fx_policy/run.sh Tests/vm_thread_stress/run.sh"
umb_suite_name() {
    [ "$1" = ctest ] && { echo umbrella-ctest; return; }
    echo "umbrella-$(echo "$1" | sed -e 's#^Tests/##' -e 's#/run\.sh$##' -e 's#^run_##' -e 's#_tests\.sh$##' -e 's#_#-#g')"
}
run_umb_suite() {  # run_umb_suite TREE SUITE LOGNAME
    mkdir -p "$WORK/home-umbrella"
    if [ "$2" = ctest ]; then
        run_logged "$3" env HOME="$WORK/home-umbrella" ctest --test-dir "$1/build" --output-on-failure \
            -R '^(pascal_tests|clike_tests|rea_tests|aether_tests|pscalvm_frontend_tests|json2bc_tests)$'
    else
        (cd "$1" && run_logged "$3" env HOME="$WORK/home-umbrella" bash "$2")
    fi
}
umb_failures() {  # umb_failures LOG: failing test names, one per line
    {
        grep -E '^\[FAIL\] ' "$1" | sed -E 's/^\[FAIL\] ([^ ]+).*/\1/'
        sed -n '/The following tests FAILED/,$p' "$1" | sed -nE 's/^[[:space:]]*[0-9]+ - ([^ ]+).*/\1/p'
    } | sort -u
}
if want umbrella; then
    if [ -z "$UMBRELLA" ]; then
        record umbrella SKIP "no umbrella (set PSCAL_UMBRELLA or pass --umbrella)"
    elif [ ! -f "$UMBRELLA/CMakeLists.txt" ] || [ ! -e "$UMBRELLA/.git" ]; then
        record umbrella FAIL "the --umbrella directory is not an umbrella checkout"
    else
        UTREE="$WORK/umbrella"; BTREE="$WORK/umbrella-base"; BASE_STATE=""
        if run_logged umbrella-prepare prepare_umbrella "$UTREE" candidate \
           && run_logged umbrella-build build_umbrella "$UTREE"; then
            UMB_BIN="$UTREE/build/bin"
            record umbrella-build PASS "umbrella $(git -C "$UMBRELLA" rev-parse --short HEAD) with components/ at the candidates"
            for suite in $UMB_SUITES; do
                name="$(umb_suite_name "$suite")"
                if [ "$suite" != ctest ] && [ ! -f "$UTREE/$suite" ]; then record "$name" SKIP "$suite not in this umbrella"; continue; fi
                if run_umb_suite "$UTREE" "$suite" "$name"; then record "$name" PASS "${suite/ctest/pascal clike rea aether pscalvm_frontend json2bc}"; continue; fi
                if [ -z "$BASE_STATE" ]; then
                    if run_logged umbrella-base-prepare prepare_umbrella "$BTREE" base \
                       && run_logged umbrella-base-build build_umbrella "$BTREE"; then BASE_STATE=ok; else BASE_STATE=failed; fi
                fi
                if [ "$BASE_STATE" != ok ]; then
                    record "$name" FAIL "fails, and the base umbrella would not build to compare (logs/umbrella-base-build.log)"; continue
                fi
                run_umb_suite "$BTREE" "$suite" "$name-base"; base_rc=$?
                cand_f="$(umb_failures "$WORK/logs/$name.log")"
                base_f="$(umb_failures "$WORK/logs/$name-base.log")"
                new_f="$(comm -23 <(echo "$cand_f") <(echo "$base_f") | grep . | tr '\n' ' ')"
                n_cand="$(echo "$cand_f" | grep -c .)"
                if [ -n "$new_f" ]; then
                    record "$name" FAIL "new failure(s) the base umbrella does not have: $new_f"
                elif [ "$n_cand" -gt 0 ] || [ $base_rc -ne 0 ]; then
                    record "$name" PASS "$n_cand failure(s), all also failing on the umbrella's own pins: $(echo "$cand_f" | tr '\n' ' ')"
                else
                    record "$name" FAIL "fails without a parseable test name, and the base passes (logs/$name.log)"
                fi
            done
        else
            record umbrella FAIL "see logs/umbrella-prepare.log, logs/umbrella-build.log"
        fi
    fi
else
    record umbrella SKIP "not selected"
fi

# ---- D19 probes (report only) ----------------------------------------------------------
D19_REPORT=""
if want d19; then
    pb="${PASCAL_BIN:-}"; cb="${CLIKE_BIN:-}"
    if [ -n "$UMB_BIN" ]; then pb="${pb:-$UMB_BIN/pascal}"; cb="${cb:-$UMB_BIN/clike}"; fi
    if [ -z "$pb$cb" ]; then
        record d19 SKIP "no pascal/clike binary (umbrella build or --pascal-bin/--clike-bin)"
    elif [ -z "$BIN" ]; then
        record d19 SKIP "no aether binary for the runner"
    else
        python3 "$AETHER_SRC/tests/backend_conformance/run.py" --aether "$BIN" \
            ${pb:+--pascal "$pb"} ${cb:+--clike "$cb"} \
            --only d19/pascal_int_wrap.pas d19/clike_int_wrap.cl >"$WORK/logs/d19.log" 2>&1
        D19_REPORT="$(grep -v -E '^(conformance|expected-fail):' "$WORK/logs/d19.log")"
        if grep -q 'moved' "$WORK/logs/d19.log"; then
            record d19 REPORT "probe output moved from the recorded lines: decide D19 from logs/d19.log"
        else
            record d19 REPORT "probe output matches the recorded lines"
        fi
    fi
else
    record d19 SKIP "not selected"
fi

# ---- corpus A/B -------------------------------------------------------------------------
if want corpus-ab; then
    [ -n "$CORPUS" ] || { [ -n "$UMBRELLA" ] && CORPUS="$UMBRELLA/Tests/aether_specialization/corpus_candidates"; }
    if [ -z "$CORPUS" ]; then
        record corpus-ab SKIP "no corpus (pass --corpus or set PSCAL_UMBRELLA)"
    elif [ ! -d "$CORPUS" ]; then
        record corpus-ab FAIL "the --corpus directory does not exist"
    elif [ -z "$BIN" ]; then
        record corpus-ab SKIP "no candidate aether binary"
    else
        BASE="$BASELINE_BIN"
        if [ -z "$BASE" ]; then
            # The base: aether HEAD (committed) at the pins HEAD records.
            build_base() {
                local bcore brea
                bcore="$(git -C "$AETHER_SRC" ls-tree HEAD external/pscal-core | awk '{print $3}')"
                brea="$(git -C "$AETHER_SRC" ls-tree HEAD external/rea | awk '{print $3}')"
                rm -rf "$WORK/base/aether"; mkdir -p "$WORK/base/aether"
                git -C "$AETHER_SRC" archive HEAD | tar -x -C "$WORK/base/aether" \
                && checkout_at "$CORE_REPO" "$bcore" "$WORK/base/pscal-core" \
                && checkout_at "$REA_REPO" "$brea" "$WORK/base/rea" \
                && cmake_configure -S "$WORK/base/aether" -B "$WORK/base/build" -DCMAKE_BUILD_TYPE=Release \
                    -DFETCHCONTENT_SOURCE_DIR_PSCAL_CORE="$WORK/base/pscal-core" \
                    -DFETCHCONTENT_SOURCE_DIR_REA="$WORK/base/rea" \
                && cmake --build "$WORK/base/build" --target aether -j "$JOBS"
            }
            if run_logged corpus-ab-base build_base; then BASE="$WORK/base/build/aether"; fi
        fi
        if [ -z "$BASE" ] || [ ! -x "$BASE" ]; then
            record corpus-ab FAIL "no base binary (see logs/corpus-ab-base.log)"
        else
            # shellcheck disable=SC2086
            run_logged corpus-ab python3 "$AETHER_SRC/tools/corpus_ab.py" --old "$BASE" --new "$BIN" \
                --corpus "$CORPUS" --jobs "$JOBS" $CORPUS_ARGS
            rc=$?
            line="$(grep '^corpus-ab:' "$WORK/logs/corpus-ab.log" | tail -n 1)"
            if [ $rc -eq 0 ]; then record corpus-ab PASS "${line#corpus-ab: }"; else record corpus-ab FAIL "${line#corpus-ab: }"; fi
        fi
    fi
else
    record corpus-ab SKIP "not selected"
fi

# ---- ASan lap (opt-in) ----------------------------------------------------------------
if want asan; then
    asan_lap() {
        cmake_configure -S "$AETHER_SRC" -B "$WORK/build-asan" -DCMAKE_BUILD_TYPE=RelWithDebInfo \
            -DCMAKE_C_FLAGS="-fsanitize=address -fno-omit-frame-pointer" \
            -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address" \
            -DFETCHCONTENT_SOURCE_DIR_PSCAL_CORE="$WORK/src/pscal-core" \
            -DFETCHCONTENT_SOURCE_DIR_REA="$WORK/src/rea" \
        && cmake --build "$WORK/build-asan" --target aether -j "$JOBS" || return 1
        local leaks=0
        [ "$(uname -s)" = "Linux" ] && leaks=1   # LeakSanitizer is Linux-only
        mkdir -p "$WORK/home-asan"
        env HOME="$WORK/home-asan" ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=$leaks:abort_on_error=0}" \
            AETHER_BIN="$WORK/build-asan/aether" bash "$AETHER_SRC/tests/run.sh" \
        && env ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=$leaks:abort_on_error=0}" \
            python3 "$AETHER_SRC/tests/backend_conformance/run.py" --aether "$WORK/build-asan/aether" --no-cache-modes --no-rss
    }
    if run_logged asan asan_lap; then record asan PASS "run.sh and conformance under AddressSanitizer"
    else record asan FAIL "see logs/asan.log"; fi
fi

# ---- summary -------------------------------------------------------------------------
{
    echo "pin_gate $( [ $FAILED -eq 0 ] && echo PASS || echo FAIL ) ($(date -u +%Y-%m-%dT%H:%MZ))"
    echo "  pscal-core ${CORE_SHA:0:12} $CORE_SUBJ"
    echo "  rea        ${REA_SHA:0:12} $REA_SUBJ"
    echo "  aether     ${AETHER_HEAD:0:12} $AETHER_DIRTY"
    echo "  rea standalone builds pscal-core GIT_TAG ${REA_CORE_TAG:-?}; aether pins ${CORE_SHA:0:12} and its pin wins"
    for r in "${RESULTS[@]+"${RESULTS[@]}"}"; do
        step="${r%%|*}"; rest="${r#*|}"; status="${rest%%|*}"; detail="${rest#*|}"
        printf '  %-26s %-6s %s\n' "$step" "$status" "$detail"
    done
    [ -n "$XFAIL_LINE" ] && echo "  conformance $XFAIL_LINE"
    if [ -n "$D19_REPORT" ]; then
        echo "  d19 probes:"
        echo "$D19_REPORT" | sed 's/^/    /'
    fi
} | sed -e "s#$WORK#<work>#g" -e "s#$AETHER_SRC#<aether>#g" -e "s#${HOME:-/nonexistent}#~#g" >"$SUMMARY"
echo
cat "$SUMMARY"
echo "  logs: $WORK/logs"

if [ $FAILED -eq 0 ] && [ $KEEP -eq 0 ]; then
    find "$WORK" -mindepth 1 -maxdepth 1 ! -name logs ! -name pin_gate.summary -exec rm -rf {} +
fi
exit $FAILED
