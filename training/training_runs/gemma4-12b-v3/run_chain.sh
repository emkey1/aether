#!/bin/bash
# Trains and scores the three runs of training/PLAN-failure-targeted-batch.md one at a time, on the training host, then restarts its image
# and video services. Stops at the first failure and leaves them stopped. Re-running resumes: finished runs are skipped.
set -u
ROOT=$HOME/aether-train
LOG=$ROOT/gemma4-12b-v3/logs/chain.log
say() { echo "[$(date -u +%FT%TZ)] $*" | tee -a "$LOG"; }

run_one() {
  local tag=$1 dir=$ROOT/$1 merged=/storage/aether-train/$1/merged_16bit container=aether-train-$1 code
  if [ -f "$dir/score/tuned.done" ]; then say "$tag: already scored"; return 0; fi
  if [ ! -f "$merged/config.json" ]; then
    say "$tag: training"
    bash "$dir/train.sh" >> "$LOG" 2>&1 || { say "FAILED $tag: train.sh exit $?"; return 1; }
    sleep 60
    while [ "$(docker inspect -f '{{.State.Running}}' "$container" 2>/dev/null)" = "true" ]; do sleep 120; done
    code=$(docker inspect -f '{{.State.ExitCode}}' "$container" 2>/dev/null)
    if [ "$code" != 0 ] || [ ! -f "$merged/config.json" ]; then say "FAILED $tag: container exit ${code:-gone}, or no merged export"; return 1; fi
    docker logs "$container" > "$dir/logs/train_container.log" 2>&1
  fi
  say "$tag: scoring"
  bash "$dir/score.sh" >> "$LOG" 2>&1
  if [ ! -f "$dir/score/tuned.done" ]; then say "FAILED $tag: scoring did not finish"; return 1; fi
  say "$tag: done"
}

mkdir -p "$(dirname "$LOG")"
say "stopping the image and video services"
systemctl --user stop comfyui swarmui i2v-studio
sleep 20
free -g | sed -n 2p >> "$LOG"
for tag in gemma4-12b-v3 gemma4-12b-v2-s42 gemma4-12b-v3-s42; do
  run_one "$tag" || exit 1
done
say "restarting the image and video services"
systemctl --user start comfyui swarmui i2v-studio
say "chain finished"
