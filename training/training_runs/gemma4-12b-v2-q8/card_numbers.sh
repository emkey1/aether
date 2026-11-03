#!/bin/bash
# What a model card needs beyond score.sh, on the same Mac and stack: generation speed against e3 at mxfp4, the fine-tune's
# Python, and the untrained base at int8 with no guide, in Aether and in Python. See README.md.
#
# After score.sh, with the base's files at $GEMMA_BASE, from a clone of aether at main:
#   tmux new -d -s gemmacard "zsh -lc 'caffeinate -i bash training/training_runs/gemma4-12b-v2-q8/card_numbers.sh'"
# Re-run to continue: a finished suite has a .done marker and an unfinished one resumes from its checkpoint.
set -u
export PATH=/opt/homebrew/bin:/usr/local/bin:$PATH
HERE=$(cd "$(dirname "$0")" && pwd)
B=$(cd "$HERE/../../.." && pwd)
W=${GEMMA_Q8_WORK:-$HOME/aether-models/gemma4-12b-v2-q8}
BASE=${GEMMA_BASE:-$HOME/aether-models/merged/gemma-4-12B-it}
OUT=$W/score
BASE_OUT=$W/base
LOG=$W/card_numbers.log
GRADER=$W/grader/aether-1.0-macos-arm64/bin/aether
GRADER_SHA=$(shasum -a 256 "$GRADER" | cut -d' ' -f1)
TUNED=aether-gemma4-12b-v2-q8-32k
M=aether-gemma4-12b-base-q8; BASE_MODEL=$M-32k
SUITES="v2_pos=bench/tasks_v2_pos.json hard_v2=bench/tasks_hard_v2.json hard_nontoon=bench/tasks_hard_nontoon.json cs=bench/tasks_cs.json frontier=bench/tasks_frontier.json frontier_algo=bench/tasks_frontier_algo.json frontier_spec=bench/tasks_frontier_spec.json sealed_toon=bench/tasks_sealed_toon.json"
mkdir -p "$BASE_OUT"
log() { echo "[$(date -u +%Y-%m-%dT%H:%M:%SZ)] $*" | tee -a "$LOG"; }
case "$("$GRADER" --version 2>&1 | head -1)" in *"2026-10-30-1 (1.0)"*) ;; *) log "ABORT the grader is not the 1.0 release"; exit 2 ;; esac
stop_others() {  # one model resident at a time
  for L in $(ollama ps 2>/dev/null | awk 'NR>1{print $1}'); do
    [ "$L" = "$1:latest" ] || [ "$L" = "$1" ] || ollama stop "$L" >> "$LOG" 2>&1
  done
}

if [ ! -f "$W/speed.done" ]; then
  stop_others none
  log "--- speed"
  python3 -u "$HERE/measure_speed.py" "$TUNED" aether-qwen36-35b-a3b-e3-mxfp4-32k 2>&1 | tee -a "$LOG" | tee "$W/speed.txt" \
    && [ "${PIPESTATUS[0]}" = 0 ] && touch "$W/speed.done" || log "WARN speed measurement failed"
fi

suites() {  # model_label destinations report_dir report_prefix extra...
  local label=$1 dest=$2 dir=$3 prefix=$4; shift 4
  local pair suite rc all=1
  for pair in $SUITES; do
    suite=${pair%%=*}
    [ -f "$dir/$prefix$suite.done" ] && continue
    log "--- $label $prefix$suite"
    python3 -u "$B/tools/aether_doc_bench.py" --destinations-config "$dest" --tasks "$B/${pair#*=}" \
      --docs none --repeats 1 --repair-attempts 2 --seed-base 42 "$@" \
      --aether-bin "$GRADER" --aether-bin-sha256 "$GRADER_SHA" --allow-skew --keep-loaded \
      --resume --output-json "$dir/$prefix$suite.json" --text-summary >> "$LOG" 2>&1
    rc=$?
    if [ $rc -eq 0 ]; then touch "$dir/$prefix$suite.done"; else all=0; log "WARN $prefix$suite exited $rc; re-run to resume"; fi
  done
  [ $all -eq 1 ]
}

stop_others "$TUNED"
suites tuned "$HERE/destinations.q8.json" "$OUT" python__ --python-baseline --skip-aether --allow-broken-oracle
ollama stop "$TUNED" >> "$LOG" 2>&1

if ! ollama show "$BASE_MODEL" > /dev/null 2>&1; then
  [ -f "$BASE/config.json" ] || { log "FAIL no base files at $BASE"; exit 1; }
  printf 'FROM %s\n' "$BASE" > "$W/Modelfile.base"
  printf 'FROM %s\nPARAMETER num_ctx 32768\n' "$M" > "$W/Modelfile.base.32k"
  log "import $M (int8)"
  { ollama create "$M" -q int8 -f "$W/Modelfile.base" && ollama create "$BASE_MODEL" -f "$W/Modelfile.base.32k"; } \
    > "$W/import.base.log" 2>&1 || { log "FAIL base import; see import.base.log"; exit 1; }
fi
sed "s/gemma4-12b-v2-q8/gemma4-12b-base-q8/g" "$HERE/destinations.q8.json" > "$W/destinations.base.json"
suites base "$W/destinations.base.json" "$BASE_OUT" python__ --python-baseline --skip-aether --allow-broken-oracle
suites base "$W/destinations.base.json" "$BASE_OUT" aether__
ollama stop "$BASE_MODEL" >> "$LOG" 2>&1
log "card numbers finished"
