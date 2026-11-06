#!/bin/bash
# What the model cards need beyond score.sh for gemma4-12b-v3 at 8 bits, on the same Mac and stack: generation speed against e3 at mxfp4, with nothing else loaded,
# and the fine-tune's Python. The untrained base's numbers do not change and come from ../gemma4-12b-v2-q8/. See README.md.
#   tmux new -d -s gemmav3card "zsh -lc 'caffeinate -i bash training/training_runs/gemma4-12b-v3-q8/card_numbers.sh'"
set -u
export PATH=/opt/homebrew/bin:/usr/local/bin:$PATH
HERE=$(cd "$(dirname "$0")" && pwd)
B=$(cd "$HERE/../../.." && pwd)
W=${GEMMA_Q8_WORK:-$HOME/aether-models/gemma4-12b-v3-q8}
OUT=$W/score
LOG=$W/card_numbers.log
GRADER=${GEMMA_GRADER:-$HOME/aether-models/gemma4-12b-v2-q8/grader/aether-1.0-macos-arm64/bin/aether}
GRADER_SHA=$(shasum -a 256 "$GRADER" | cut -d' ' -f1)
TUNED=aether-gemma4-12b-v3-q8-32k
SUITES="v2_pos=bench/tasks_v2_pos.json hard_v2=bench/tasks_hard_v2.json hard_nontoon=bench/tasks_hard_nontoon.json cs=bench/tasks_cs.json frontier=bench/tasks_frontier.json frontier_algo=bench/tasks_frontier_algo.json frontier_spec=bench/tasks_frontier_spec.json sealed_toon=bench/tasks_sealed_toon.json"
mkdir -p "$OUT"
log() { echo "[$(date -u +%Y-%m-%dT%H:%M:%SZ)] $*" | tee -a "$LOG"; }
[ "$GRADER_SHA" = b968240e919cf7e577389cef8a3c66b9068d7c0fcb507ec9edd630fa6f5aed25 ] || { log "ABORT the grader is not the 1.0 release"; exit 2; }
stop_others() {
  for L in $(ollama ps 2>/dev/null | awk 'NR>1{print $1}'); do
    [ "$L" = "$1:latest" ] || [ "$L" = "$1" ] || ollama stop "$L" >> "$LOG" 2>&1
  done
}
if [ ! -f "$W/speed.done" ]; then
  stop_others none
  log "--- speed"
  python3 -u "$HERE/../gemma4-12b-v2-q8/measure_speed.py" "$TUNED" aether-qwen36-35b-a3b-e3-mxfp4-32k 2>&1 | tee -a "$LOG" | tee "$W/speed.txt" \
    && [ "${PIPESTATUS[0]}" = 0 ] && touch "$W/speed.done" || log "WARN speed measurement failed"
fi
stop_others "$TUNED"
all=1
for pair in $SUITES; do
  suite=${pair%%=*}
  [ -f "$OUT/python__$suite.done" ] && continue
  log "--- tuned python__$suite"
  python3 -u "$B/tools/aether_doc_bench.py" --destinations-config "$HERE/destinations.q8.json" --tasks "$B/${pair#*=}" \
    --docs none --repeats 1 --repair-attempts 2 --seed-base 42 --python-baseline --skip-aether --allow-broken-oracle \
    --aether-bin "$GRADER" --aether-bin-sha256 "$GRADER_SHA" --allow-skew --keep-loaded \
    --resume --output-json "$OUT/python__$suite.json" --text-summary >> "$LOG" 2>&1 && touch "$OUT/python__$suite.done" || { all=0; log "WARN python__$suite; re-run to resume"; }
done
ollama stop "$TUNED" >> "$LOG" 2>&1
[ $all = 1 ] && log "card numbers finished"
