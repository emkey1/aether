#!/bin/bash
# Score one model on a scoring Mac through Ollama: import a merged export (or use an existing Ollama model) under the
# one-model lock, then run the Aether suites with no guide, optionally with a guide, and the Python baseline.
#
#   tools/score_ollama_model.sh --tag TAG --out DIR (--merged DIR | --model OLLAMA_MODEL) [options]
#
#   --suites "v2_pos hard_v2 ..."    Aether suites (bench/tasks_<suite>.json), no guide
#   --guided-suites "..."            the same model with a guide; needs --guide NAME=PATH
#   --python-suites "..."            the Python baseline (sandboxed) on the same suites
#   --quant int8                     import quantization (Ollama's safetensors import: int4/int8/nvfp4/mxfp4/mxfp8)
#   --ctx 32768                      num_ctx of the scoring variant (MODEL-<ctx/1024>k)
#   --repairs 2                      repair attempts per failed first attempt
#   --grader PATH --grader-sha256 S  the aether binary that grades; its sha256 must match
#   --smoke-tasks FILE               a few tasks run (and checked) before the suites
#   --leak PREFIX                    reasoning-leak prefix to refuse (repeatable; default: thinking, <think, okay,)
#
# Resumable: every suite has a .done marker and runs with --resume, so re-run the same command after any death. The
# Ollama server version is pinned by the first suite (DIR/ollama.version); a later suite on another version stops.
# Each report is checked (tools/check_bench_report.py): no all-empty answers, no reasoning traces, every case measured,
# and a guided report carries the named guide (name and sha256) while an unguided one carries none.
# Marker: DIR/finished holds DONE or FAILED <why>. Run long passes under tmux + caffeinate -i.
set -u
export PATH=/opt/homebrew/bin:/usr/local/bin:$PATH
SRC=$(cd "$(dirname "$0")/.." && pwd)
TAG= OUT= MERGED= MODEL= SUITES= GUIDED= PYS= GUIDE= QUANT=int8 CTX=32768 REPAIRS=2 GRADER= GSHA_WANT= SMOKE= LEAKS=()
while [ $# -gt 0 ]; do
  case $1 in
    --tag) TAG=$2;; --out) OUT=$2;; --merged) MERGED=$2;; --model) MODEL=$2;;
    --suites) SUITES=$2;; --guided-suites) GUIDED=$2;; --python-suites) PYS=$2;; --guide) GUIDE=$2;;
    --quant) QUANT=$2;; --ctx) CTX=$2;; --repairs) REPAIRS=$2;; --grader) GRADER=$2;; --grader-sha256) GSHA_WANT=$2;;
    --smoke-tasks) SMOKE=$2;; --leak) LEAKS+=("$2");;
    -h|--help) sed -n '2,24p' "$0"; exit 0;;
    *) echo "unknown argument: $1" >&2; exit 2;;
  esac
  shift 2
done
[ ${#LEAKS[@]} -eq 0 ] && LEAKS=("thinking" "<think" "okay, ")
die() { echo "$*" >&2; exit 2; }
[ -n "$TAG" ] && [ -n "$OUT" ] || die "--tag and --out are required"
[ -n "$MERGED" ] || [ -n "$MODEL" ] || die "one of --merged or --model is required"
[ -z "$GUIDED" ] || [ -n "$GUIDE" ] || die "--guided-suites needs --guide NAME=PATH"
[ -n "$GRADER" ] && [ -n "$GSHA_WANT" ] || die "--grader and --grader-sha256 are required"
[ -n "$SUITES$GUIDED$PYS" ] || die "nothing to score: give --suites, --guided-suites or --python-suites"
for S in $SUITES $GUIDED $PYS; do [ -f "$SRC/bench/tasks_$S.json" ] || die "no suite bench/tasks_$S.json"; done
mkdir -p "$OUT" && OUT=$(cd "$OUT" && pwd)
LOG=$OUT/score.log FINISHED=$OUT/finished
rm -f "$FINISHED"
log() { echo "[$(date -u +%Y-%m-%dT%H:%M:%SZ)] $*" | tee -a "$LOG"; }
finish() { log "$1"; echo "$1" > "$FINISHED"; exit "$2"; }

GSHA=$(shasum -a 256 "$GRADER" 2>/dev/null | cut -d' ' -f1)
[ "$GSHA" = "$GSHA_WANT" ] || finish "FAILED the grader's sha256 is ${GSHA:-missing}, expected $GSHA_WANT" 2
if [ -n "$GUIDE" ]; then
  GNAME=${GUIDE%%=*} GPATH=${GUIDE#*=}
  [ -s "$GPATH" ] || finish "FAILED the guide $GPATH is missing or empty" 2
  GUIDE_SHA=$(shasum -a 256 "$GPATH" | cut -d' ' -f1)
fi
[ -n "$MODEL" ] || MODEL=aether-$TAG-$QUANT
SERVE=$MODEL-$((CTX / 1024))k
log "tag $TAG, model $SERVE, harness $(git -C "$SRC" rev-parse --short HEAD)$(git -C "$SRC" diff --quiet HEAD -- tools bench || echo '+dirty'), grader $("$GRADER" --version 2>/dev/null | head -1) sha256 $GSHA"

. "$SRC/tools/ollama_guard.sh" "$SERVE"

if ! ollama show "$MODEL" > /dev/null 2>&1; then
  [ -n "$MERGED" ] || finish "FAILED no Ollama model $MODEL and no --merged export to import" 2
  [ -f "$MERGED/config.json" ] || finish "FAILED $MERGED has no config.json (not a merged export)" 2
  mkdir -p "$OUT/import"
  printf 'FROM %s\n' "$MERGED" > "$OUT/import/Modelfile"
  log "import $MODEL ($QUANT) from $MERGED"
  ollama create "$MODEL" -q "$QUANT" -f "$OUT/import/Modelfile" > "$OUT/import/import.log" 2>&1 \
    || finish "FAILED import: $(tail -1 "$OUT/import/import.log" | tr -d '\r' | cut -c1-200)" 3
fi
if ! ollama show "$SERVE" > /dev/null 2>&1; then
  printf 'FROM %s\nPARAMETER num_ctx %s\n' "$MODEL" "$CTX" > "$OUT/Modelfile.ctx"
  ollama create "$SERVE" -f "$OUT/Modelfile.ctx" >> "$LOG" 2>&1 || finish "FAILED creating $SERVE" 3
fi
server=$(curl -s localhost:11434/api/version | python3 -c 'import json,sys;print(json.load(sys.stdin)["version"])')
[ -f "$OUT/ollama.version" ] || echo "$server" > "$OUT/ollama.version"
[ "$server" = "$(cat "$OUT/ollama.version")" ] \
  || finish "FAILED the Ollama server is $server, earlier suites ran on $(cat "$OUT/ollama.version")" 2
log "Ollama $server; $(ollama show "$SERVE" | awk '/quantization/{print "quantization", $2}'); $(ollama list | awk -v n="$SERVE:latest" '$1==n{print $2, $3, $4}')"

python3 - "$TAG" "$SERVE" "$CTX" "$OUT/dest.json" <<'PY'
import json, sys
tag, model, ctx, path = sys.argv[1:]
json.dump({"destinations": [{"id": f"{tag}-ollama", "type": "openai_chat_completions",
                             "base_url": "http://localhost:11434/v1", "api_key": "", "model": model, "temperature": 0,
                             "max_output_tokens": 8192, "prompt_context_limit": int(ctx),
                             "request_timeout_seconds": 900, "request_max_retries": 2, "retry_backoff_seconds": 5,
                             "cooldown_seconds": 0, "extra_body": {"reasoning_effort": "none"}}]},
          open(path, "w"), indent=1)
PY
LEAK_ARGS=(); for p in "${LEAKS[@]}"; do LEAK_ARGS+=(--leak "$p"); done
cd "$SRC" || finish "FAILED no clone" 2

bench() {  # report tasks extra-args...
  local report=$1 tasks=$2; shift 2
  python3 -u tools/aether_doc_bench.py --destinations-config "$OUT/dest.json" --tasks "$tasks" --repeats 1 \
    --seed-base 42 --repair-attempts "$REPAIRS" --aether-bin "$GRADER" --aether-bin-sha256 "$GSHA" --allow-skew \
    --keep-loaded --resume --output-json "$report" --text-summary "$@"
}
run() {  # kind suite check-args -- bench-args
  local kind=$1 S=$2; shift 2
  local checks=() n rc
  while [ $# -gt 0 ] && [ "$1" != -- ]; do checks+=("$1"); shift; done
  shift
  [ -f "$OUT/${kind}__$S.done" ] && return 0
  log "--- $kind $S"
  for n in 1 2 3; do  # the harness exits 3 when cases failed in the infrastructure; --resume re-runs only those
    bench "$OUT/${kind}__$S.json" "bench/tasks_$S.json" "$@" >> "$OUT/${kind}__$S.log" 2>&1
    rc=$?; [ $rc -ne 3 ] && break; log "infrastructure failures (try $n), resuming"; sleep 60
  done
  [ $rc -eq 0 ] || finish "FAILED $kind $S: harness exit $rc (see ${kind}__$S.log); re-run to resume" 1
  python3 tools/check_bench_report.py "$OUT/${kind}__$S.json" --complete ${checks[@]+"${checks[@]}"} | tee -a "$LOG"
  [ "${PIPESTATUS[0]}" -eq 0 ] || finish "FAILED $kind $S: the report check refused it" 1
  touch "$OUT/${kind}__$S.done"
}

if [ -n "$SMOKE" ] && [ ! -f "$OUT/smoke.done" ]; then
  log "--- smoke"
  bench "$OUT/smoke.json" "$SMOKE" --docs none >> "$OUT/smoke.log" 2>&1
  python3 tools/check_bench_report.py "$OUT/smoke.json" --complete --no-guide "${LEAK_ARGS[@]}" | tee -a "$LOG"
  [ "${PIPESTATUS[0]}" -eq 0 ] || finish "FAILED smoke" 1
  touch "$OUT/smoke.done"
fi
for S in $SUITES; do run aether "$S" --no-guide "${LEAK_ARGS[@]}" -- --docs none; done
for S in $GUIDED; do
  run guided "$S" --guide "$GNAME=$GUIDE_SHA" "${LEAK_ARGS[@]}" -- --doc "$GNAME=$GPATH" --docs "$GNAME"
done
for S in $PYS; do
  run python "$S" "${LEAK_ARGS[@]}" -- --python-baseline --skip-aether --allow-broken-oracle --docs none
done
ollama stop "$SERVE" >> "$LOG" 2>&1
finish "DONE" 0
