#!/bin/bash
# Serve + benchmark one cs-aug18-precision-grid model: simple/large/cs, no-guide
# (--docs none), matching the cs-aug4 board convention (temperature 0.2, seed 42,
# repair-attempts 1). Serves via vLLM on host-b (serve_any.sh, port 8019), frees the
# GPU afterward. Run from the repo root (the aether checkout).
#
# Usage: ./bench/bench_cs_aug18_precision.sh <tag>
#   e.g. ./bench/bench_cs_aug18_precision.sh qwen3-8b-nothink-4bit-cs-aug18
set -euo pipefail

TAG=${1:?usage: bench_cs_aug18_precision.sh <tag>}
DEST="host-b-$TAG"
CFG=bench/destinations.cs_aug18_precision.json
OUTDIR=bench/out/cs_aug18_precision
mkdir -p "$OUTDIR"

echo "=== checking host-b free memory before serving ==="
# Each vLLM container lifecycle (crash OR clean exit) appears to leave some unified
# memory unreclaimed at the driver level on this host -- confirmed accumulating
# across multiple successful/failed runs, only fully cleared by a full reboot (not
# systemctl restart docker, not docker rm -f). Rather than fail late, proactively
# reboot if headroom is too thin for a safe serve. MIN_FREE_GB is conservative: 40GiB
# comfortably covers a 9B bf16 model (~17GiB weights) + KV cache at max-model-len 16384.
MIN_FREE_GB=40
free_gb=$(ssh host-b "free -g | awk '/Mem:/{print \$4}'")
if [ "${free_gb:-0}" -lt "$MIN_FREE_GB" ]; then
  echo "host-b free memory (${free_gb}GiB) below ${MIN_FREE_GB}GiB threshold -- rebooting to clear the leak"
  ssh host-b "sudo reboot" || true
  echo "waiting for host-b to go down..."
  for i in $(seq 1 20); do
    ssh -o ConnectTimeout=5 -o BatchMode=yes host-b "echo up" >/dev/null 2>&1 || break
    sleep 5
  done
  echo "waiting for host-b to come back..."
  for i in $(seq 1 60); do
    ssh -o ConnectTimeout=8 -o BatchMode=yes host-b "echo up" >/dev/null 2>&1 && break
    sleep 10
  done
  sleep 15
  free_gb=$(ssh host-b "free -g | awk '/Mem:/{print \$4}'")
  echo "post-reboot free memory: ${free_gb}GiB"
fi

echo "=== serving $TAG on host-b (vLLM, port 8019) ==="
# 0.65 (~79GiB) comfortably covers a 9B model's weights + KV cache at max-model-len
# 16384; kept below serve_any.sh's 0.85 default given host-b's memory headroom is
# already tight even right after a reboot/cleanup.
ssh host-b "cd ~/training/aether-qwen-coder-30b-unsloth && ./serve_any.sh $TAG 16384 0.65"

echo "=== waiting for vLLM readiness ==="
ready=0
for i in $(seq 1 60); do
  if curl -sf "http://host-b:8019/v1/models" >/dev/null 2>&1; then
    echo "ready after ${i}0s"
    ready=1
    break
  fi
  # Bail early if the container already died (e.g. OOM at startup) instead of
  # burning the full 10-minute timeout polling a server that will never answer.
  if [ "$(ssh host-b "docker inspect --format '{{.State.Running}}' aether-bench-vllm-q7 2>/dev/null")" != "true" ]; then
    echo "vLLM container exited during startup -- aborting"
    break
  fi
  sleep 10
done
if [ "$ready" -ne 1 ]; then
  echo "ERROR: vLLM never became ready for $TAG -- not running the benchmark against a dead server" >&2
  ssh host-b "docker logs aether-bench-vllm-q7 2>&1 | tail -30" >&2
  ssh host-b "docker rm -f aether-bench-vllm-q7 >/dev/null 2>&1 || true"
  exit 1
fi

for suite in simple large cs; do
  case "$suite" in
    simple) tasks=bench/tasks_v2_pos.json ;;
    large)  tasks=bench/tasks_hard.json ;;
    cs)     tasks=bench/tasks_cs.json ;;
  esac
  echo "=== $TAG / $suite ==="
  python3 tools/aether_doc_bench.py \
    --tasks "$tasks" \
    --docs none \
    --repair-attempts 1 \
    --destinations-config "$CFG" \
    --destination "$DEST" \
    --output-json "$OUTDIR/${TAG}_${suite}.json" \
    --text-summary --progress
done

echo "=== freeing GPU: stopping vLLM container ==="
ssh host-b "docker rm -f aether-bench-vllm-q7 >/dev/null 2>&1 || true"
echo "=== done: $TAG ==="
