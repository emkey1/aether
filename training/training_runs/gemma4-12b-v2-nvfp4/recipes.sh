#!/bin/bash
# The two more careful NVFP4 recipes, after the plain build's pass: each is built with quantize.py, then measured and scored by
# score.sh. MLP-only goes first because AWQ's export may not load. See README.md.
#
#   tmux new -d -s gemmarecipes "bash recipes.sh"
set -u
R=$(cd "$(dirname "$0")" && pwd)
ROOT=${GEMMA_WEIGHTS:-/storage/aether-train/gemma4-12b-v2}
DATA=${GEMMA_DATA:-$HOME/aether-train/gemma4-12b-v2/data}
IMAGE=aether-unsloth-gemma4:u202610-tf517
LOG=$R/recipes.log
log() { echo "[$(date -u +%Y-%m-%dT%H:%M:%SZ)] $*" | tee -a "$LOG"; }

log "waiting for the plain build's pass"
until grep -q "score pass finished" "$R/score/score.log" 2>/dev/null; do sleep 60; done
for RECIPE in mlp_only awq_lite; do
  OUTW=$ROOT/nvfp4_$RECIPE
  if ! grep -q '^done ' "$R/quantize.$RECIPE.log" 2>/dev/null; then
    mkdir -p "$OUTW"
    log "=== $RECIPE: building"
    docker rm -f gemma-nvfp4-quant > /dev/null 2>&1
    docker run --rm --name gemma-nvfp4-quant --gpus all --user "$(id -u):$(id -g)" -e HOME=/tmp -e USER=claw -e LOGNAME=claw \
      -e PYTHONUNBUFFERED=1 -e HF_HUB_OFFLINE=1 -v "$ROOT/merged_16bit:/weights:ro" -v "$DATA:/data:ro" -v "$OUTW:/out" -v "$R:/work:ro" \
      --entrypoint python3 "$IMAGE" /work/quantize.py --model /weights --data /data/aether_instruction_sft.jsonl --out /out \
      --samples 64 --recipe "$RECIPE" > "$R/quantize.$RECIPE.log" 2>&1
    grep -q '^done ' "$R/quantize.$RECIPE.log" || { log "FAIL $RECIPE: build failed; see quantize.$RECIPE.log"; continue; }
    log "$RECIPE built: $(du -sh "$OUTW" | cut -f1), quant_algo $(grep -o '"quant_algo": "[^"]*"' "$OUTW/hf_quant_config.json")"
  fi
  bash "$R/score.sh" "$RECIPE" >> "$LOG" 2>&1 || log "FAIL $RECIPE: score.sh exited nonzero; see score_$RECIPE/score.log"
done
log "recipes finished"
