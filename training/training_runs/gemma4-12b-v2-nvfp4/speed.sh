#!/bin/bash
# NVFP4 against the 16-bit merge on one Blackwell host, in vLLM, with and without --enforce-eager (the flag the 16-bit
# scoring used). Leaves the host's other services up: they are idle, and the model fits beside them. See README.md.
#
#   tmux new -d -s gemmanvfp4 "bash speed.sh"
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=${GEMMA_WEIGHTS:-/storage/aether-train/gemma4-12b-v2}
BENCH=${GEMMA_BENCH:-$HOME/aether-train/gemma4-12b-v2/bench}
OUT=$HERE/speed
NAME=gemma-nvfp4-serve
mkdir -p "$OUT"
log() { echo "[$(date -u +%Y-%m-%dT%H:%M:%SZ)] $*" | tee -a "$OUT/speed.log"; }

for CASE in nvfp4:nvfp4:eager merged_16bit:bf16:eager nvfp4:nvfp4:graph merged_16bit:bf16:graph; do
  IFS=: read -r DIR TAG MODE <<< "$CASE"
  LABEL=$TAG-$MODE
  [ -f "$OUT/$LABEL.done" ] && continue
  docker rm -f "$NAME" > /dev/null 2>&1
  EXTRA=(); [ "$MODE" = eager ] && EXTRA=(--enforce-eager)
  log "=== $LABEL: serving"
  docker run -d --name "$NAME" --gpus all --ipc=host --shm-size=16g -v "$ROOT:$ROOT:ro" -p 8000:8000 vllm/vllm-openai:latest \
    --model "$ROOT/$DIR" --served-model-name "$LABEL" --dtype bfloat16 --max-model-len 32768 \
    --gpu-memory-utilization "${GEMMA_GPU_FRACTION:-0.35}" --port 8000 "${EXTRA[@]}" > /dev/null
  UP=0
  for _ in $(seq 1 120); do
    curl -sf -m 5 localhost:8000/health > /dev/null && { UP=1; break; }
    [ "$(docker inspect -f '{{.State.Running}}' "$NAME" 2>/dev/null)" = true ] || break
    sleep 10
  done
  docker logs "$NAME" > "$OUT/$LABEL.serve.log" 2>&1
  grep -hE "NVFP4 GEMM|quantization=|Using .*Kernel" "$OUT/$LABEL.serve.log" | cut -c1-200 | sort -u | head -6 | tee -a "$OUT/speed.log"
  if [ $UP = 1 ]; then
    python3 -u "$HERE/measure_speed.py" --bench "$BENCH" --model "$LABEL" 2>&1 | tee -a "$OUT/speed.log"
    [ "${PIPESTATUS[0]}" = 0 ] && touch "$OUT/$LABEL.done" || log "FAIL $LABEL: measurement failed"
  else
    log "FAIL $LABEL: the server did not come up; see $LABEL.serve.log"
    tail -5 "$OUT/$LABEL.serve.log" | cut -c1-240 | tee -a "$OUT/speed.log"
  fi
  docker logs "$NAME" > "$OUT/$LABEL.serve.log" 2>&1
  docker rm -f "$NAME" > /dev/null 2>&1
  sleep 10
done
log "speed pass finished"
