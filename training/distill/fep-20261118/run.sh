#!/bin/bash
# fep-20261118 on the scoring Mac: e3, e2 and e3 at epoch 2 answer the 2,802 frozen tasks of f1, f2, f3-20261103 and fv3-20261105 at int8, with no
# guide and no repair. Pass or fail only: nothing here is solved, harvested or trained on. Graded by the 1.0 release binary.
# Run from a clone of this repo at the commit that fixed the reading: bash training/distill/fep-20261118/run.sh
# Re-run to continue: the harness resumes and every pass has a .done marker. Ollama work waits for the Mac's one-model lock.
set -u
export PATH=/opt/homebrew/bin:/usr/local/bin:$PATH
B=fep-20261118
SRC=$(cd "$(dirname "$0")/../../.." && pwd)
D=$SRC/training/distill/$B
SMOKE=$SRC/training/training_runs/qwen36-35b-a3b-v2
G=${AETHER_GRADER:-$HOME/aether-models/gemma4-12b-v2-q8/grader/aether-1.0-macos-arm64/bin/aether}
GUARD=${OLLAMA_GUARD:-$HOME/aether-models/ollama_guard.sh}
FINISHED=$D/finished
LEAKS='["thinking", "<think", "okay, "]'
rm -f "$FINISHED"
LOG=$D/driver.log
log() { echo "[$(date -u +%Y-%m-%dT%H:%M:%SZ)] $*" | tee -a "$LOG"; }
finish() { log "$1"; echo "$1" > "$FINISHED"; exit "$2"; }
cd "$SRC" || finish "FAILED no clone" 2
[ -f "$GUARD" ] || finish "FAILED no Ollama guard at the expected path" 2
GSHA=$(shasum -a 256 "$G" | cut -d' ' -f1)
[ "$GSHA" = b968240e919cf7e577389cef8a3c66b9068d7c0fcb507ec9edd630fa6f5aed25 ] || finish "FAILED the grader is not the 1.0 release" 2
log "clone at $(git rev-parse --short HEAD); README $(git log -1 --format=%h -- "$D/README.md")"
git diff --quiet HEAD -- "$D/README.md" "$D/compare.py" || finish "FAILED the reading differs from the committed one" 2

if [ ! -f "$D/merge.done" ]; then
  python3 - "$D/tasks_frozen.json" <<'PY' 2>&1 | tee -a "$LOG"
import json, sys
tasks = []
for batch in ("f1-20261103", "f2-20261103", "f3-20261103", "fv3-20261105"):
    tasks += json.load(open(f"training/distill/{batch}/tasks_frozen.json"))["tasks"]
if len(tasks) != 2802 or len({t["id"] for t in tasks}) != 2802:
    raise SystemExit(f"expected 2802 unique tasks, got {len(tasks)}")
json.dump({"version": "fep-20261118", "description": "the frozen tasks of f1, f2, f3-20261103 and fv3-20261105, for filter passes by three MoE adapters",
           "tasks": tasks}, open(sys.argv[1], "w"), indent=1, ensure_ascii=False)
print(f"merged {len(tasks)} frozen tasks")
PY
  [ "$(python3 -c 'import json,sys;print(len(json.load(open(sys.argv[1]))["tasks"]))' "$D/tasks_frozen.json" 2>/dev/null)" = 2802 ] && touch "$D/merge.done" \
    || finish "FAILED merge" 1
fi

bench() {  # destinations tasks report
  python3 -u tools/aether_doc_bench.py --destinations-config "$1" --tasks "$2" --repeats 1 --seed-base 42 --aether-bin "$G" --aether-bin-sha256 "$GSHA" \
    --allow-skew --keep-loaded --resume --output-json "$3" --docs none --repair-attempts 0 --text-summary
}
retry() {  # the harness exits 3 when a case failed in the infrastructure; --resume re-runs only those
  local n rc
  for n in 1 2 3; do "$@"; rc=$?; [ $rc -ne 3 ] && return $rc; log "infra failures (try $n), resuming"; sleep 60; done
  return $rc
}
dest_for() {  # model file
  python3 - "$1" "$2" <<'PY'
import json, sys
model, path = sys.argv[1:]
json.dump({"destinations": [{"id": model.replace("aether-", "") + "-m5ollama", "type": "openai_chat_completions", "base_url": "http://localhost:11434/v1",
                             "api_key": "", "model": model, "temperature": 0, "max_output_tokens": 8192, "prompt_context_limit": 32768,
                             "request_timeout_seconds": 900, "request_max_retries": 2, "retry_backoff_seconds": 5, "cooldown_seconds": 0,
                             "extra_body": {"reasoning_effort": "none"}}]}, open(path, "w"), indent=1)
PY
}
pass() {  # tag model
  local tag=$1 model=$2
  [ -f "$D/filter_$tag.done" ] && return 0
  ( . "$GUARD" "$model"
    server=$(curl -s localhost:11434/api/version | python3 -c 'import json,sys;print(json.load(sys.stdin)["version"])')
    [ -f "$D/ollama.version" ] || echo "$server" > "$D/ollama.version"
    [ "$server" = "$(cat "$D/ollama.version")" ] || { log "the Ollama server is $server, the first pass ran on $(cat "$D/ollama.version")"; exit 1; }
    ollama show "$model" > /dev/null 2>&1 || { log "no model $model"; exit 1; }
    log "=== $model, Ollama $server, $(ollama show "$model" | awk '/quantization/{print $2}'), $(ollama list | awk -v n="$model:latest" '$1==n{print $2, $3, $4}')"
    dest_for "$model" "$D/dest_$tag.json"
    if [ ! -f "$D/smoke_$tag.ok" ]; then
      bench "$D/dest_$tag.json" "$SMOKE/smoke_tasks.json" "$D/smoke_$tag.json" >> "$D/filter_$tag.log" 2>&1
      python3 "$SMOKE/check_generations.py" "$D/smoke_$tag.json" "$LEAKS" >> "$LOG" 2>&1 || { log "smoke failed: $tag"; exit 1; }
      touch "$D/smoke_$tag.ok"
    fi
    log "--- filter $tag"
    retry bench "$D/dest_$tag.json" "$D/tasks_frozen.json" "$D/filter_$tag.json" >> "$D/filter_$tag.log" 2>&1
    rc=$?
    [ $rc -eq 0 ] && { python3 "$SMOKE/check_generations.py" "$D/filter_$tag.json" "$LEAKS" --complete >> "$LOG" 2>&1 || rc=5; }
    ollama stop "$model" >> "$D/filter_$tag.log" 2>&1; exit $rc ) && touch "$D/filter_$tag.done"
}
pass e3 aether-qwen36-35b-a3b-e3-q8-32k || finish "FAILED pass e3; re-run to resume" 1
pass e2 aether-qwen36-35b-a3b-e2-q8-32k || finish "FAILED pass e2; re-run to resume" 1
pass e3_ep2 aether-qwen36-35b-a3b-e3-ep2-q8-32k || finish "FAILED pass e3_ep2; re-run to resume" 1
python3 "$D/compare.py" 2>&1 | tee -a "$LOG"
finish "DONE" 0
