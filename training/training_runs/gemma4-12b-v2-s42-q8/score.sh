#!/bin/bash
# The Gemma 4 12B fine-tune's second seed (gemma4-12b-v2-s42, final step) at 8 bits: does its lead after repair survive in the form people run? Its merged
# 16-bit export imported into Ollama at int8; no guide, the eight suites, graded by the 1.0 release binary, with the recipe of ../gemma4-12b-v2-q8. See README.md.
#
# On the scoring Mac, from a clone of aether at main:
#   tmux new -d -s gemmas42q8 "zsh -lc 'caffeinate -i bash training/training_runs/gemma4-12b-v2-s42-q8/score.sh'"
# Re-run to continue: a finished suite has a .done marker and an unfinished one resumes from its checkpoint.
# SMOKE_ONLY=1 stops after the import and the smoke test.
set -u
export PATH=/opt/homebrew/bin:/usr/local/bin:$PATH
HERE=$(cd "$(dirname "$0")" && pwd)
B=$(cd "$HERE/../../.." && pwd)
Q8=$HERE/../gemma4-12b-v2-q8
W=${GEMMA_Q8_WORK:-$HOME/aether-models/gemma4-12b-v2-s42-q8}
MERGED=${GEMMA_MERGED:-$HOME/aether-models/merged/gemma4-12b-v2-s42/merged_16bit}
OUT=$W/score
LOG=$OUT/score.log
GRADER=${GEMMA_GRADER:-$HOME/aether-models/gemma4-12b-v2-q8/grader/aether-1.0-macos-arm64/bin/aether}
GRADER_1_0=b968240e919cf7e577389cef8a3c66b9068d7c0fcb507ec9edd630fa6f5aed25
LEAKS='["thinking", "<think", "<|channel", "okay, "]'
SUITES="v2_pos=bench/tasks_v2_pos.json hard_v2=bench/tasks_hard_v2.json hard_nontoon=bench/tasks_hard_nontoon.json cs=bench/tasks_cs.json frontier=bench/tasks_frontier.json frontier_algo=bench/tasks_frontier_algo.json frontier_spec=bench/tasks_frontier_spec.json sealed_toon=bench/tasks_sealed_toon.json"
M=aether-gemma4-12b-v2-s42-q8; MODEL=$M-32k
mkdir -p "$OUT"
log() { echo "[$(date -u +%Y-%m-%dT%H:%M:%SZ)] $*" | tee -a "$LOG"; }

GRADER_VERSION=$("$GRADER" --version 2>&1 | head -1)
GRADER_SHA=$(shasum -a 256 "$GRADER" | cut -d' ' -f1)
log "grader $GRADER_VERSION sha256 $GRADER_SHA; harness $(git -C "$B" rev-parse --short HEAD)"
[ "$GRADER_SHA" = "$GRADER_1_0" ] || { log "ABORT the grader is not the 1.0 release"; exit 2; }
echo "$GRADER_SHA" > "$OUT/grader.sha256"

[ -f "$OUT/all.done" ] && { log "SKIP every suite done"; exit 0; }
# One model resident at a time: stop anything else before an import or a load.
for L in $(ollama ps 2>/dev/null | awk 'NR>1{print $1}'); do
  [ "$L" = "$MODEL:latest" ] || [ "$L" = "$MODEL" ] || ollama stop "$L" >> "$LOG" 2>&1
done
if ! ollama show "$MODEL" > /dev/null 2>&1; then
  [ -f "$MERGED/config.json" ] || { log "FAIL no merged export at $MERGED"; exit 1; }
  printf 'FROM %s\n' "$MERGED" > "$W/Modelfile"
  printf 'FROM %s\nPARAMETER num_ctx 32768\n' "$M" > "$W/Modelfile.32k"
  log "import $M (int8)"
  { ollama create "$M" -q int8 -f "$W/Modelfile" && ollama create "$MODEL" -f "$W/Modelfile.32k"; } \
    > "$W/import.log" 2>&1 || { log "FAIL import failed; see import.log"; exit 1; }
fi
log "=== $MODEL, $(ollama show "$MODEL" | awk '/quantization/{print $2}'), $(ollama list | awk -v m="$MODEL:latest" '$1==m{print $3, $4}')"
run_suite() {  # name tasks repairs
  python3 -u "$B/tools/aether_doc_bench.py" --destinations-config "$HERE/destinations.q8.json" --tasks "$2" \
    --docs none --repeats 1 --repair-attempts "$3" --seed-base 42 \
    --aether-bin "$GRADER" --aether-bin-sha256 "$GRADER_SHA" --allow-skew \
    --resume --output-json "$OUT/tuned__$1.json" --text-summary >> "$LOG" 2>&1
}
if [ ! -f "$OUT/smoke.ok" ]; then
  run_suite smoke "$Q8/smoke_tasks.json" 0
  # No guide: a prompt is a few hundred tokens. Thinking off: a 3-line smoke answer takes under 200 completion
  # tokens, and the leak prefixes catch a trace left in the content.
  if ! python3 "$Q8/check_generations.py" "$OUT/tuned__smoke.json" "$LEAKS" >> "$LOG" 2>&1 \
     || ! python3 -c 'import json,sys; r=json.load(open(sys.argv[1]))["destinations"][0]["variants"][0]["results"]; t=[x.get("prompt_tokens_measured") or 0 for x in r]; c=[(x["attempts"][0].get("usage") or {}).get("completion_tokens") or 0 for x in r if x.get("attempts")]; print("smoke prompt tokens", t, "completion tokens", c); sys.exit(0 if r and max(t) < 3000 and c and max(c) < 1000 else 1)' "$OUT/tuned__smoke.json" >> "$LOG" 2>&1; then
    log "FAIL smoke: empty output, a reasoning trace, thinking on, or a guide in the prompt; not scoring it"
    ollama stop "$MODEL" >> "$LOG" 2>&1; exit 1
  fi
  touch "$OUT/smoke.ok"; log "SMOKE OK"
fi
[ "${SMOKE_ONLY:-0}" = 1 ] && { ollama stop "$MODEL" >> "$LOG" 2>&1; exit 0; }
ALL=1
for PAIR in $SUITES; do
  SUITE=${PAIR%%=*}
  [ -f "$OUT/$SUITE.done" ] && continue
  log "--- $SUITE"
  run_suite "$SUITE" "$B/${PAIR#*=}" 2
  if python3 "$Q8/check_generations.py" "$OUT/tuned__$SUITE.json" "$LEAKS" --complete >> "$LOG" 2>&1; then
    touch "$OUT/$SUITE.done"
  else
    ALL=0; log "WARN $SUITE incomplete or suspicious; re-run to resume"
  fi
done
[ $ALL -eq 1 ] && touch "$OUT/all.done" && log "DONE"
log "score pass finished"
