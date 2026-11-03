#!/bin/bash
# The NVFP4 build on Blackwell: no guide, the eight suites, the 1.0 grader and the serving flags of ../gemma4-12b-v2's 16-bit
# scoring. Only the weights change, and the GPU memory fraction, so it fits beside the host's idle services. See README.md.
#
#   tmux new -d -s gemmanvfp4score "bash score.sh"
# Re-run to continue: a finished suite has a .done marker and an unfinished one resumes from its checkpoint.
set -u
R=$(cd "$(dirname "$0")" && pwd)
B=${GEMMA_BENCH:-$HOME/aether-train/gemma4-12b-v2/bench}
ROOT=${GEMMA_WEIGHTS:-/storage/aether-train/gemma4-12b-v2}
GRADER=${GEMMA_GRADER:-$HOME/aether-1.0/aether-1.0-src/build/aether}
OUT=$R/score
LOG=$OUT/score.log
NAME=gemma4-12b-v2-nvfp4
SERVE=aether-serve-$NAME
LEAKS='["<|channel", "thought\n"]'
SUITES="v2_pos=bench/tasks_v2_pos.json hard_v2=bench/tasks_hard_v2.json hard_nontoon=bench/tasks_hard_nontoon.json cs=bench/tasks_cs.json frontier=bench/tasks_frontier.json frontier_algo=bench/tasks_frontier_algo.json frontier_spec=bench/tasks_frontier_spec.json sealed_toon=bench/tasks_sealed_toon.json"
mkdir -p "$OUT"
log() { echo "[$(date -u +%Y-%m-%dT%H:%M:%SZ)] $*" | tee -a "$LOG"; }
cleanup() { docker rm -f "$SERVE" > /dev/null 2>&1; log "served model stopped"; }
trap cleanup EXIT

GRADER_VERSION=$("$GRADER" --version 2>&1 | head -1)
GRADER_SHA=$(sha256sum "$GRADER" | cut -d' ' -f1)
log "grader $GRADER_VERSION sha256 $GRADER_SHA"
case "$GRADER_VERSION" in *2026-10-30-1*) ;; *) log "ABORT grader is not language 2026-10-30-1"; exit 2 ;; esac
echo "$GRADER_SHA" > "$OUT/grader.sha256"
[ -f "$OUT/all.done" ] && { log "SKIP every suite done"; exit 0; }

docker rm -f "$SERVE" > /dev/null 2>&1
docker run -d --name "$SERVE" --gpus all --ipc=host --shm-size=16g -v "$ROOT:$ROOT:ro" -p 8021:8000 vllm/vllm-openai:latest \
  --model "$ROOT/nvfp4" --served-model-name "$NAME" --dtype bfloat16 --max-model-len 32768 \
  --gpu-memory-utilization "${GEMMA_GPU_FRACTION:-0.35}" --port 8000 --enforce-eager >> "$LOG" 2>&1 \
  || { log "FAIL docker run"; exit 1; }
READY=0
for _ in $(seq 1 180); do
  curl -s --max-time 5 http://localhost:8021/v1/models 2>/dev/null | grep -q "\"$NAME\"" && { READY=1; break; }
  [ "$(docker inspect -f '{{.State.Running}}' "$SERVE" 2>/dev/null)" = true ] || break
  sleep 10
done
[ $READY = 1 ] || { log "FAIL never became ready"; docker logs --tail 80 "$SERVE" >> "$LOG" 2>&1; exit 1; }
log "READY $(docker logs "$SERVE" 2>&1 | grep -oE 'Using [A-Za-z0-9]+ for NVFP4 GEMM' | sort -u | head -1)"

run_suite() {  # name tasks repairs
  python3 -u "$B/tools/aether_doc_bench.py" --destinations-config "$R/destinations.nvfp4.json" --tasks "$2" \
    --docs none --repeats 1 --repair-attempts "$3" --seed-base 42 \
    --aether-bin "$GRADER" --aether-bin-sha256 "$GRADER_SHA" --allow-skew --keep-loaded \
    --resume --output-json "$OUT/tuned__$1.json" --text-summary >> "$LOG" 2>&1
}
if [ ! -f "$OUT/smoke.ok" ]; then
  run_suite smoke "$R/smoke_tasks.json" 0
  python3 "$R/check_generations.py" "$OUT/tuned__smoke.json" "$LEAKS" >> "$LOG" 2>&1 \
    || { log "FAIL smoke: empty output or a reasoning trace; not scoring it"; exit 1; }
  touch "$OUT/smoke.ok"; log "SMOKE OK"
fi
ALL=1
for PAIR in $SUITES; do
  SUITE=${PAIR%%=*}
  [ -f "$OUT/$SUITE.done" ] && continue
  log "--- $SUITE"
  run_suite "$SUITE" "$B/${PAIR#*=}" 2
  if python3 "$R/check_generations.py" "$OUT/tuned__$SUITE.json" "$LEAKS" --complete >> "$LOG" 2>&1; then
    touch "$OUT/$SUITE.done"
  else
    ALL=0; log "WARN $SUITE incomplete or suspicious; re-run to resume"
  fi
done
[ $ALL -eq 1 ] && touch "$OUT/all.done" && log "DONE"
log "score pass finished"
