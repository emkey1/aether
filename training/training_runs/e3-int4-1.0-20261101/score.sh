#!/bin/bash
# The released MoE fine-tune (qwen36-35b-a3b-e3) at 4-bit: does it keep its scores in a size that fits a 32 GB Mac?
# Its merged bf16 export imported into Ollama at nvfp4, mxfp4 and int4 and, as the same-stack anchor, the existing
# int8 import; no guide, the eight suites, graded by the 1.0 release binary. e3's published numbers come from vLLM at
# bf16, so the int8 run separates the serving stack from the 4-bit cost. See README.md.
#
# On the scoring Mac, from a clone of aether at main:
#   tmux new -d -s e3int4 "zsh -lc 'caffeinate -i bash training/training_runs/e3-int4-1.0-20261101/score.sh'"
# Re-run to continue: a finished suite has a .done marker and an unfinished one resumes from its checkpoint.
# SMOKE_ONLY=1 stops after each variant's import and smoke test.
set -u
export PATH=/opt/homebrew/bin:/usr/local/bin:$PATH
HERE=$(cd "$(dirname "$0")" && pwd)
B=$(cd "$HERE/../../.." && pwd)
W=${E3Q_WORK:-$HOME/aether-models/e3-int4-1.0-20261101}
MERGED=${E3_MERGED:-$HOME/aether-models/merged/qwen36-35b-a3b-e3/merged_16bit}
OUT=$W/score
LOG=$OUT/score.log
GRADER=$W/grader/aether-1.0-macos-arm64/bin/aether
REL=https://github.com/emkey1/aether/releases/download/1.0
LEAKS='["thinking", "<think", "okay, "]'
SUITES="v2_pos=bench/tasks_v2_pos.json hard_v2=bench/tasks_hard_v2.json hard_nontoon=bench/tasks_hard_nontoon.json cs=bench/tasks_cs.json frontier=bench/tasks_frontier.json frontier_algo=bench/tasks_frontier_algo.json frontier_spec=bench/tasks_frontier_spec.json sealed_toon=bench/tasks_sealed_toon.json"
mkdir -p "$OUT"
log() { echo "[$(date -u +%Y-%m-%dT%H:%M:%SZ)] $*" | tee -a "$LOG"; }

# The grader is the published 1.0 macOS binary, checked against the release's SHA256SUMS.
if [ ! -x "$GRADER" ]; then
  mkdir -p "$W/grader"
  ( cd "$W/grader" && curl -fsSLO "$REL/aether-1.0-macos-arm64.tar.gz" && curl -fsSLO "$REL/SHA256SUMS" \
    && grep ' aether-1.0-macos-arm64.tar.gz$' SHA256SUMS | shasum -a 256 -c - \
    && tar -xzf aether-1.0-macos-arm64.tar.gz ) >> "$LOG" 2>&1 || { log "ABORT cannot fetch or verify the 1.0 grader"; exit 2; }
fi
GRADER_VERSION=$("$GRADER" --version 2>&1 | head -1)
GRADER_SHA=$(shasum -a 256 "$GRADER" | cut -d' ' -f1)
log "grader $GRADER_VERSION sha256 $GRADER_SHA; harness $(git -C "$B" rev-parse --short HEAD)"
case "$GRADER_VERSION" in *"2026-10-30-1 (1.0)"*) ;; *) log "ABORT the grader is not the 1.0 release"; exit 2 ;; esac
echo "$GRADER_SHA" > "$OUT/grader.sha256"

# TAG:QUANT, in order of interest: the two that quantize every expert weight (about 21 GB), the int8 anchor (named q8,
# as in the earlier runs; it normally exists already), then int4, which keeps the experts' down projections at 8 bits
# (27 GB). Each is imported from the merged export when missing, then copied with a 32K context.
for V in nvfp4:nvfp4 mxfp4:mxfp4 q8:int8 int4:int4; do
  TAG=${V%%:*}; Q=${V#*:}
  M=aether-qwen36-35b-a3b-e3-$TAG; MODEL=$M-32k
  D="$OUT/$TAG"; mkdir -p "$D"
  [ -f "$D/all.done" ] && { log "SKIP $TAG (every suite done)"; continue; }
  # One model resident at a time: stop anything else before an import or a load.
  for L in $(ollama ps 2>/dev/null | awk 'NR>1{print $1}'); do
    [ "$L" = "$MODEL:latest" ] || [ "$L" = "$MODEL" ] || ollama stop "$L" >> "$LOG" 2>&1
  done
  if ! ollama show "$MODEL" > /dev/null 2>&1; then
    [ -f "$MERGED/config.json" ] || { log "FAIL $TAG: no merged export at $MERGED"; continue; }
    printf 'FROM %s\n' "$MERGED" > "$W/Modelfile.$TAG"
    printf 'FROM %s\nPARAMETER num_ctx 32768\n' "$M" > "$W/Modelfile.$TAG.32k"
    log "import $M ($Q)"
    { ollama create "$M" -q "$Q" -f "$W/Modelfile.$TAG" && ollama create "$MODEL" -f "$W/Modelfile.$TAG.32k"; } \
      > "$W/import.$TAG.log" 2>&1 || { log "FAIL $TAG: import failed; see import.$TAG.log"; continue; }
  fi
  log "=== $TAG: $MODEL, $(ollama show "$MODEL" | awk '/quantization/{print $2}'), $(ollama list | awk -v m="$MODEL:latest" '$1==m{print $3, $4}')"
  run_suite() {  # name tasks repairs
    python3 -u "$B/tools/aether_doc_bench.py" --destinations-config "$HERE/destinations.$TAG.json" --tasks "$2" \
      --docs none --repeats 1 --repair-attempts "$3" --seed-base 42 \
      --aether-bin "$GRADER" --aether-bin-sha256 "$GRADER_SHA" --allow-skew \
      --resume --output-json "$D/tuned__$1.json" --text-summary >> "$LOG" 2>&1
  }
  if [ ! -f "$D/smoke.ok" ]; then
    run_suite smoke "$HERE/smoke_tasks.json" 0
    # No guide: a prompt is a few hundred tokens. Thinking off: a 3-line smoke answer takes under 200 completion
    # tokens, and the leak prefixes catch a trace left in the content.
    if ! python3 "$HERE/check_generations.py" "$D/tuned__smoke.json" "$LEAKS" >> "$LOG" 2>&1 \
       || ! python3 -c 'import json,sys; r=json.load(open(sys.argv[1]))["destinations"][0]["variants"][0]["results"]; t=[x.get("prompt_tokens_measured") or 0 for x in r]; c=[(x["attempts"][0].get("usage") or {}).get("completion_tokens") or 0 for x in r if x.get("attempts")]; print("smoke prompt tokens", t, "completion tokens", c); sys.exit(0 if r and max(t) < 3000 and c and max(c) < 1000 else 1)' "$D/tuned__smoke.json" >> "$LOG" 2>&1; then
      log "FAIL $TAG smoke: empty output, a reasoning trace, thinking on, or a guide in the prompt; not scoring it"
      ollama stop "$MODEL" >> "$LOG" 2>&1; continue
    fi
    touch "$D/smoke.ok"; log "SMOKE OK $TAG"
  fi
  [ "${SMOKE_ONLY:-0}" = 1 ] && { ollama stop "$MODEL" >> "$LOG" 2>&1; continue; }
  ALL=1
  for PAIR in $SUITES; do
    SUITE=${PAIR%%=*}
    [ -f "$D/$SUITE.done" ] && continue
    log "--- $TAG $SUITE"
    run_suite "$SUITE" "$B/${PAIR#*=}" 2
    if python3 "$HERE/check_generations.py" "$D/tuned__$SUITE.json" "$LEAKS" --complete >> "$LOG" 2>&1; then
      touch "$D/$SUITE.done"
    else
      ALL=0; log "WARN $TAG $SUITE incomplete or suspicious; re-run to resume"
    fi
  done
  [ $ALL -eq 1 ] && touch "$D/all.done" && log "DONE $TAG"
  ollama stop "$MODEL" >> "$LOG" 2>&1
done
log "score pass finished"
