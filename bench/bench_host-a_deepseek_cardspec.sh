#!/bin/bash
# Benchmark DeepSeek-V4-Flash-0731 (vLLM across host-a rank0/API + host-b rank1) on
# the current guide-size board's methodology: four suites, --docs full,medium,
# --repair-attempts 2. Run from the repo root (the aether checkout).
set -uo pipefail

CFG=bench/destinations.host-a_deepseek_v4_flash.json
DEST=host-a-deepseek-v4-flash-0731
OUTDIR=bench/out/host-a_deepseek_v4_flash_cardspec
AETHER_BIN=${AETHER_BIN:-/usr/local/bin/aether}
mkdir -p "$OUTDIR"

for suite in tasks_v2_pos tasks_hard_v2 tasks_cs tasks_hard_nontoon; do
    echo "=== $suite ==="
    python3 tools/aether_doc_bench.py \
        --destinations-config "$CFG" \
        --destination "$DEST" \
        --tasks "bench/$suite.json" \
        --docs full,medium \
        --repair-attempts 2 \
        --aether-bin "$AETHER_BIN" \
        --output-json "$OUTDIR/$suite.json" \
        --text-summary --progress
    echo "=== $suite done (exit $?) ==="
done
echo "ALL SUITES COMPLETE"
