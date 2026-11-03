#!/bin/bash
# The Gemma 4 12B fine-tune (gemma4-12b-v2, final step) at 4 bits on the scoring Mac: its merged 16-bit export imported into
# Ollama at nvfp4 and mxfp4; no guide, the eight suites, graded by the 1.0 release binary, with the recipe of
# ../gemma4-12b-v2-q8. Ollama's resident size is logged after every suite. e3 at mxfp4 then repeats its own pass, only to read
# its resident size over a full pass; ../e3-int4-1.0-20261101 holds its scores. See README.md.
#
# On the scoring Mac, from a clone of aether at main:
#   tmux new -d -s gemma4bit "zsh -lc 'caffeinate -i bash training/training_runs/gemma4-12b-v2-4bit/score.sh'"
# Re-run to continue: a finished suite has a .done marker and an unfinished one resumes from its checkpoint.
set -u
export PATH=/opt/homebrew/bin:/usr/local/bin:$PATH
HERE=$(cd "$(dirname "$0")" && pwd)
B=$(cd "$HERE/../../.." && pwd)
W=${GEMMA_4BIT_WORK:-$HOME/aether-models/gemma4-12b-v2-4bit}
MERGED=${GEMMA_MERGED:-$HOME/aether-models/merged/gemma4-12b-v2/merged_16bit}
OUT=$W/score
LOG=$OUT/score.log
GRADER=${GEMMA_GRADER:-$HOME/aether-models/gemma4-12b-v2-q8/grader/aether-1.0-macos-arm64/bin/aether}
LEAKS='["thinking", "<think", "<|channel", "okay, "]'
SUITES="v2_pos=bench/tasks_v2_pos.json hard_v2=bench/tasks_hard_v2.json hard_nontoon=bench/tasks_hard_nontoon.json cs=bench/tasks_cs.json frontier=bench/tasks_frontier.json frontier_algo=bench/tasks_frontier_algo.json frontier_spec=bench/tasks_frontier_spec.json sealed_toon=bench/tasks_sealed_toon.json"
mkdir -p "$OUT"
log() { echo "[$(date -u +%Y-%m-%dT%H:%M:%SZ)] $*" | tee -a "$LOG"; }

GRADER_VERSION=$("$GRADER" --version 2>&1 | head -1)
GRADER_SHA=$(shasum -a 256 "$GRADER" | cut -d' ' -f1)
log "grader $GRADER_VERSION sha256 $GRADER_SHA; harness $(git -C "$B" rev-parse --short HEAD)"
case "$GRADER_VERSION" in *"2026-10-30-1 (1.0)"*) ;; *) log "ABORT the grader is not the 1.0 release"; exit 2 ;; esac
[ "$GRADER_SHA" = b968240e919cf7e577389cef8a3c66b9068d7c0fcb507ec9edd630fa6f5aed25 ] || { log "ABORT the grader's hash is not the release's"; exit 2; }

stop_others() {  # one model resident at a time
  for L in $(ollama ps 2>/dev/null | awk 'NR>1{print $1}'); do
    [ "$L" = "$1:latest" ] || [ "$L" = "$1" ] || ollama stop "$L" >> "$LOG" 2>&1
  done
}
resident() {  # the model's line from ollama ps
  log "resident $1: $(ollama ps 2>/dev/null | awk -v m="$2:latest" '$1==m{print $3, $4}')"
}

for TAG in nvfp4 mxfp4; do
  M=aether-gemma4-12b-v2-$TAG; MODEL=$M-32k
  D="$OUT/$TAG"; mkdir -p "$D"
  [ -f "$D/all.done" ] && { log "SKIP $TAG (every suite done)"; continue; }
  stop_others "$MODEL"
  if ! ollama show "$MODEL" > /dev/null 2>&1; then
    [ -f "$MERGED/config.json" ] || { log "FAIL $TAG: no merged export at $MERGED"; continue; }
    printf 'FROM %s\n' "$MERGED" > "$W/Modelfile.$TAG"
    printf 'FROM %s\nPARAMETER num_ctx 32768\n' "$M" > "$W/Modelfile.$TAG.32k"
    log "import $M ($TAG)"
    { ollama create "$M" -q "$TAG" -f "$W/Modelfile.$TAG" && ollama create "$MODEL" -f "$W/Modelfile.$TAG.32k"; } \
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
    if ! python3 "$HERE/check_generations.py" "$D/tuned__smoke.json" "$LEAKS" >> "$LOG" 2>&1 \
       || ! python3 -c 'import json,sys; r=json.load(open(sys.argv[1]))["destinations"][0]["variants"][0]["results"]; t=[x.get("prompt_tokens_measured") or 0 for x in r]; c=[(x["attempts"][0].get("usage") or {}).get("completion_tokens") or 0 for x in r if x.get("attempts")]; print("smoke prompt tokens", t, "completion tokens", c); sys.exit(0 if r and max(t) < 3000 and c and max(c) < 1000 else 1)' "$D/tuned__smoke.json" >> "$LOG" 2>&1; then
      log "FAIL $TAG smoke: empty output, a reasoning trace, thinking on, or a guide in the prompt; not scoring it"
      ollama stop "$MODEL" >> "$LOG" 2>&1; continue
    fi
    touch "$D/smoke.ok"; log "SMOKE OK $TAG"
  fi
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
    resident "$TAG" "$MODEL"
  done
  [ $ALL -eq 1 ] && touch "$D/all.done" && log "DONE $TAG"
  python3 -u "$B/training/training_runs/gemma4-12b-v2-q8/measure_speed.py" "$MODEL" 2>&1 | tee -a "$LOG" | tee "$D/speed.txt"
done

# e3 at mxfp4 over a full pass, for its resident size; these reports stay here and only ../e3-int4-1.0-20261101's are its scores.
E3=aether-qwen36-35b-a3b-e3-mxfp4-32k
D="$OUT/e3_resident"; mkdir -p "$D"
if [ ! -f "$D/all.done" ]; then
  stop_others "$E3"
  for PAIR in $SUITES; do
    SUITE=${PAIR%%=*}
    python3 -u "$B/tools/aether_doc_bench.py" --destinations-config "$B/training/training_runs/e3-int4-1.0-20261101/destinations.mxfp4.json" \
      --tasks "$B/${PAIR#*=}" --docs none --repeats 1 --repair-attempts 2 --seed-base 42 \
      --aether-bin "$GRADER" --aether-bin-sha256 "$GRADER_SHA" --allow-skew \
      --resume --output-json "$D/tuned__$SUITE.json" --text-summary >> "$D/run.log" 2>&1
    resident e3-mxfp4 "$E3"
  done
  ollama stop "$E3" >> "$LOG" 2>&1
  touch "$D/all.done"
fi
log "4-bit pass finished"
