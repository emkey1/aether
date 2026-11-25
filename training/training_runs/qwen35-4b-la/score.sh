#!/bin/bash
# qwen35-4b-la on the scoring Mac: the one scoring session of README.md's amendment of 2026-11-25. The four runs answer
# the 2,802 frozen tasks of f1, f2, f3-20261103 and fv3-20261105 at bf16, with no guide and no repair, in the order
# la-s3407, base-s3407, la-s42, base-s42, on one Ollama server version, graded by the 1.0 release binary. Pass or fail
# only: nothing here is solved, harvested or trained on.
#
# Before it starts, each run's merged export is copied to $MERGED/<run>/merged_16bit and checked against the training
# host's sha256 (the file copy.ok marks it); this script waits for all four markers, imports all four at bf16 as
# qwen35-4b-v1 was imported, and only then writes session_server_version.txt and starts the first pass.
# Run from a clone at the commit that fixed the reading: bash training/training_runs/qwen35-4b-la/score.sh
# Re-run to continue: the harness resumes and every pass has a .done marker. A changed server version stops it.
set -u
export PATH=/opt/homebrew/bin:/usr/local/bin:$PATH
SRC=$(cd "$(dirname "$0")/../../.." && pwd)
D=$SRC/training/training_runs/qwen35-4b-la
SMOKE=$SRC/training/training_runs/qwen36-35b-a3b-v2
G=${AETHER_GRADER:-$HOME/aether-models/gemma4-12b-v2-q8/grader/aether-1.0-macos-arm64/bin/aether}
GUARD=${OLLAMA_GUARD:-$HOME/aether-models/ollama_guard.sh}
MERGED=${AETHER_MERGED_ROOT:-$HOME/aether-models/merged}
SESSION=$D/session_server_version.txt
FINISHED=$D/finished
LEAKS='["thinking", "<think", "okay, "]'
RUNS="qwen35-4b-la-la-s3407 qwen35-4b-la-base-s3407 qwen35-4b-la-la-s42 qwen35-4b-la-base-s42"
rm -f "$FINISHED"
LOG=$D/driver.log
log() { echo "[$(date -u +%Y-%m-%dT%H:%M:%SZ)] $*" | tee -a "$LOG"; }
finish() { log "$1"; echo "$1" > "$FINISHED"; exit "$2"; }
cd "$SRC" || finish "FAILED no clone" 2
[ -f "$GUARD" ] || finish "FAILED no Ollama guard at the expected path" 2
GSHA=$(shasum -a 256 "$G" | cut -d' ' -f1)
[ "$GSHA" = b968240e919cf7e577389cef8a3c66b9068d7c0fcb507ec9edd630fa6f5aed25 ] || finish "FAILED the grader is not the 1.0 release" 2
log "clone at $(git rev-parse --short HEAD); README $(git log -1 --format=%h -- "$D/README.md")"
git diff --quiet HEAD -- "$D/README.md" "$D/compare.py" "$D/score.sh" || finish "FAILED the reading differs from the committed one" 2

if [ ! -f "$D/merge.done" ]; then
  python3 - "$D/tasks_frozen.json" <<'PY' 2>&1 | tee -a "$LOG"
import json, sys
tasks = []
for batch in ("f1-20261103", "f2-20261103", "f3-20261103", "fv3-20261105"):
    tasks += json.load(open(f"training/distill/{batch}/tasks_frozen.json"))["tasks"]
if len(tasks) != 2802 or len({t["id"] for t in tasks}) != 2802:
    raise SystemExit(f"expected 2802 unique tasks, got {len(tasks)}")
json.dump({"version": "qwen35-4b-la", "description": "the frozen tasks of f1, f2, f3-20261103 and fv3-20261105, for filter passes by the four runs",
           "tasks": tasks}, open(sys.argv[1], "w"), indent=1, ensure_ascii=False)
print(f"merged {len(tasks)} frozen tasks")
PY
  [ "$(python3 -c 'import json,sys;print(len(json.load(open(sys.argv[1]))["tasks"]))' "$D/tasks_frozen.json" 2>/dev/null)" = 2802 ] && touch "$D/merge.done" \
    || finish "FAILED merge" 1
fi

server_version() { curl -s localhost:11434/api/version | python3 -c 'import json,sys;print(json.load(sys.stdin)["version"])'; }
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
json.dump({"destinations": [{"id": model.replace("aether-", "") + "-host-d-ollama", "type": "openai_chat_completions", "base_url": "http://localhost:11434/v1",
                             "api_key": "", "model": model, "temperature": 0, "max_output_tokens": 8192, "prompt_context_limit": 32768,
                             "request_timeout_seconds": 900, "request_max_retries": 2, "retry_backoff_seconds": 5, "cooldown_seconds": 0,
                             "extra_body": {"reasoning_effort": "none"}}]}, open(path, "w"), indent=1)
PY
}
render_same() {  # model reference: the four runs render a prompt identically
  python3 - "$1" "$2" >> "$LOG" 2>&1 <<'PY'
import json, sys, urllib.request
new, ref = sys.argv[1], sys.argv[2]
def render(model):
    body = {"model": model, "messages": [{"role": "user", "content": "PROMPT-HERE"}], "stream": False, "reasoning_effort": "none", "_debug_render_only": True, "max_tokens": 4}
    request = urllib.request.Request("http://localhost:11434/v1/chat/completions", json.dumps(body).encode(), {"Content-Type": "application/json"})
    return (json.load(urllib.request.urlopen(request, timeout=600)).get("_debug_info") or {}).get("rendered_template")
a, b = render(new), render(ref)
print("rendered,", new, repr(a))
sys.exit(0 if a and a == b and "PROMPT-HERE" in a else 1)
PY
}

# 1. All four exports copied and checked (copy.ok), with one config, tokenizer and template, and four sets of weights.
FIRST=${RUNS%% *}
for run in $RUNS; do
  [ -f "$MERGED/$run/copy.ok" ] || log "waiting for $MERGED/$run/copy.ok"
  until [ -f "$MERGED/$run/copy.ok" ]; do sleep 300; done
  for f in config.json tokenizer.json chat_template.jinja; do
    cmp -s "$MERGED/$run/merged_16bit/$f" "$MERGED/$FIRST/merged_16bit/$f" || finish "FAILED $run: $f differs from $FIRST's" 1
  done
done
python3 - "$MERGED" $RUNS <<'PY' 2>&1 | tee -a "$LOG"
import json, pathlib, sys
root, runs = pathlib.Path(sys.argv[1]), sys.argv[2:]
weights = {}
for run in runs:
    index = json.loads((root / run / "merged_16bit" / "model.safetensors.index.json").read_text())
    weights[run] = tuple(sorted(index["weight_map"].items()))
names = {w for w in weights.values()}
if len(names) != 1:
    raise SystemExit("FAILED the four exports name different tensors")
hashes = {run: (root / run / "hash.local").read_text() for run in runs}
if len(set(hashes.values())) != len(runs):
    raise SystemExit("FAILED two exports have the same files")
print(f"four exports: one tensor list ({len(weights[runs[0]])} tensors), four different sets of files")
PY
[ "${PIPESTATUS[0]}" -eq 0 ] || finish "FAILED the exports check" 1

# 2. Import all four at bf16, as qwen35-4b-v1 was imported, before any pass of the session.
for run in $RUNS; do
  m=aether-$run
  ollama show "$m-32k" > /dev/null 2>&1 && continue
  ( . "$GUARD" "$m"
    printf 'FROM %s\n' "$MERGED/$run/merged_16bit" > "$D/Modelfile.$run"
    printf 'FROM %s\nPARAMETER num_ctx 32768\n' "$m" > "$D/Modelfile.$run.32k"
    log "import $m (bf16), Ollama $(server_version)"
    { ollama create "$m" -f "$D/Modelfile.$run" && ollama create "$m-32k" -f "$D/Modelfile.$run.32k"; } > "$D/import.$run.log" 2>&1 ) \
    || finish "FAILED import $run" 1
done

# 3. The session: one server version, written before the first pass.
pass() {  # run
  local run=$1 model=aether-$1-32k
  [ -f "$D/filter_$run.done" ] && return 0
  ( . "$GUARD" "$model"
    server=$(server_version)
    [ -s "$SESSION" ] || echo "$server" > "$SESSION"
    [ "$server" = "$(cat "$SESSION")" ] || { log "the Ollama server is $server, the session runs on $(cat "$SESSION")"; exit 1; }
    ollama show "$model" > /dev/null 2>&1 || { log "no model $model"; exit 1; }
    log "=== $model, Ollama $server, $(ollama show "$model" | awk '/quantization/{print $2}'), $(ollama list | awk -v n="$model:latest" '$1==n{print $2, $3, $4}')"
    render_same "aether-$run" "aether-$FIRST" || { log "render check failed: $run"; exit 1; }
    dest_for "$model" "$D/dest_$run.json"
    if [ ! -f "$D/smoke_$run.ok" ]; then
      bench "$D/dest_$run.json" "$SMOKE/smoke_tasks.json" "$D/smoke_$run.json" >> "$D/filter_$run.log" 2>&1
      python3 "$SMOKE/check_generations.py" "$D/smoke_$run.json" "$LEAKS" >> "$LOG" 2>&1 || { log "smoke failed: $run"; exit 1; }
      touch "$D/smoke_$run.ok"
    fi
    log "--- filter $run"
    retry bench "$D/dest_$run.json" "$D/tasks_frozen.json" "$D/filter_$run.json" >> "$D/filter_$run.log" 2>&1
    rc=$?
    [ $rc -eq 0 ] && { python3 "$SMOKE/check_generations.py" "$D/filter_$run.json" "$LEAKS" --complete >> "$LOG" 2>&1 || rc=5; }
    ollama stop "$model" >> "$D/filter_$run.log" 2>&1; exit $rc ) && touch "$D/filter_$run.done"
}
for run in $RUNS; do pass "$run" || finish "FAILED pass $run; re-run to resume" 1; done
[ "$(server_version)" = "$(cat "$SESSION")" ] || finish "FAILED the server changed after the last pass" 1
python3 "$D/compare.py" 2>&1 | tee -a "$LOG"
finish "DONE" 0
