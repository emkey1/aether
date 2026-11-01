#!/bin/bash
# The untrained Qwen3.6-35B-A3B at mxfp4 with the 1.0 medium guide in every prompt, on the eight suites, graded by the
# 1.0 release binary. The same guide, grader, settings and smoke test as ../guided-base-1.0-20261031; only the
# quantization changes (int8 there). It completes the 32 GB comparison in ../e3-int4-1.0-20261101. See README.md.
#
# On the scoring Mac, from a clone of aether at main:
#   tmux new -d -s guidedmx "zsh -lc 'caffeinate -i bash training/training_runs/guided-base-mxfp4-1.0-20261101/score.sh'"
# Re-run to continue: a finished suite has a .done marker and an unfinished one resumes from its checkpoint.
# SMOKE_ONLY=1 stops after the smoke test.
set -u
export PATH=/opt/homebrew/bin:/usr/local/bin:$PATH
HERE=$(cd "$(dirname "$0")" && pwd)
B=$(cd "$HERE/../../.." && pwd)
W=${GUIDED_WORK:-$HOME/aether-models/guided-base-mxfp4-1.0-20261101}
MERGED=${BASE_MERGED:-$HOME/aether-models/merged/qwen36-35b-a3b-base/merged_16bit}
OUT=$W/score
LOG=$OUT/score.log
CARD=$W/medium_guide_1.0.md
GRADER=$W/grader/aether-1.0-macos-arm64/bin/aether
REL=https://github.com/emkey1/aether/releases/download/1.0
GUIDE_SHA=b693a67bc16cba2f1e61c041903ecf338e39d2b4ad34c6bbae20b8e4b7f933d8
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
log "grader $GRADER_VERSION sha256 $GRADER_SHA"
case "$GRADER_VERSION" in *"2026-10-30-1 (1.0)"*) ;; *) log "ABORT the grader is not the 1.0 release"; exit 2 ;; esac
echo "$GRADER_SHA" > "$OUT/grader.sha256"

# The guide is the medium guide at the 1.0 tag.
git -C "$B" fetch -q --tags origin 2>/dev/null
git -C "$B" show 1.0:docs/aether_for_llms_medium_contexts.md > "$CARD" 2>> "$LOG"
[ "$(shasum -a 256 "$CARD" | cut -d' ' -f1)" = "$GUIDE_SHA" ] || { log "ABORT the guide is not the 1.0 medium guide"; exit 2; }
log "guide $(sed -n 3p "$CARD") sha256 $GUIDE_SHA; harness $(git -C "$B" rev-parse --short HEAD)"

TAG=mxfp4; M=aether-qwen36-35b-a3b-base-mxfp4; MODEL=$M-32k
D="$OUT/$TAG"; mkdir -p "$D"
[ -f "$D/all.done" ] && { log "every suite done"; exit 0; }
# One model resident at a time: stop anything else before an import or a load.
for L in $(ollama ps 2>/dev/null | awk 'NR>1{print $1}'); do
  [ "$L" = "$MODEL:latest" ] || [ "$L" = "$MODEL" ] || ollama stop "$L" >> "$LOG" 2>&1
done
if ! ollama show "$MODEL" > /dev/null 2>&1; then
  [ -f "$MERGED/config.json" ] || { log "FAIL no merged export at $MERGED"; exit 2; }
  printf 'FROM %s\n' "$MERGED" > "$W/Modelfile.$TAG"
  printf 'FROM %s\nPARAMETER num_ctx 32768\n' "$M" > "$W/Modelfile.$TAG.32k"
  log "import $M (mxfp4)"
  { ollama create "$M" -q mxfp4 -f "$W/Modelfile.$TAG" && ollama create "$MODEL" -f "$W/Modelfile.$TAG.32k"; } \
    > "$W/import.$TAG.log" 2>&1 || { log "FAIL import failed; see import.$TAG.log"; exit 2; }
fi
log "=== $TAG: $MODEL, $(ollama show "$MODEL" | awk '/quantization/{print $2}'), $(ollama list | awk -v m="$MODEL:latest" '$1==m{print $3, $4}')"
run_suite() {  # name tasks repairs
  python3 -u "$B/tools/aether_doc_bench.py" --destinations-config "$HERE/destinations.$TAG.json" --tasks "$2" \
    --doc "card=$CARD" --docs card --repeats 1 --repair-attempts "$3" --seed-base 42 \
    --aether-bin "$GRADER" --aether-bin-sha256 "$GRADER_SHA" --allow-skew \
    --resume --output-json "$D/guided__$1.json" --text-summary >> "$LOG" 2>&1
}
if [ ! -f "$D/smoke.ok" ]; then
  run_suite smoke "$HERE/smoke_tasks.json" 0
  # A guided prompt is about 14K tokens. Thinking off: a 3-line smoke answer takes under 200 completion tokens.
  if ! python3 "$HERE/check_generations.py" "$D/guided__smoke.json" "$LEAKS" >> "$LOG" 2>&1 \
     || ! python3 -c 'import json,sys; r=json.load(open(sys.argv[1]))["destinations"][0]["variants"][0]["results"]; t=[x.get("prompt_tokens_measured") or 0 for x in r]; c=[(x["attempts"][0].get("usage") or {}).get("completion_tokens") or 0 for x in r if x.get("attempts")]; print("smoke prompt tokens", t, "completion tokens", c); sys.exit(0 if r and min(t) > 10000 and c and max(c) < 1000 else 1)' "$D/guided__smoke.json" >> "$LOG" 2>&1; then
    log "FAIL smoke: empty output, a reasoning trace, thinking on, or no guide in the prompt; not scoring it"
    ollama stop "$MODEL" >> "$LOG" 2>&1; exit 1
  fi
  touch "$D/smoke.ok"; log "SMOKE OK"
fi
[ "${SMOKE_ONLY:-0}" = 1 ] && { ollama stop "$MODEL" >> "$LOG" 2>&1; log "score pass finished"; exit 0; }
ALL=1
for PAIR in $SUITES; do
  SUITE=${PAIR%%=*}
  [ -f "$D/$SUITE.done" ] && continue
  log "--- $TAG $SUITE"
  run_suite "$SUITE" "$B/${PAIR#*=}" 2
  if python3 "$HERE/check_generations.py" "$D/guided__$SUITE.json" "$LEAKS" --complete >> "$LOG" 2>&1; then
    touch "$D/$SUITE.done"
  else
    ALL=0; log "WARN $TAG $SUITE incomplete or suspicious; re-run to resume"
  fi
done
[ $ALL -eq 1 ] && touch "$D/all.done" && log "DONE $TAG"
ollama stop "$MODEL" >> "$LOG" 2>&1
log "score pass finished"
