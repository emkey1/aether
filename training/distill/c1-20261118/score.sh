#!/bin/bash
# c1-20261118 on the scoring Mac: the one scoring session of README.md's amendment of 2026-11-25. The two targeted runs
# (gemma4-12b-v3 and -v3-s42, the int8 models fv3 answered with) and the two control runs (gemma4-12b-c1-r2 and
# -c1-s42, imported at int8 here) answer fv3's 1,026 frozen tasks with no guide and no repair, in the order v3, c1_s42,
# v3_s42, c1 (README.md's second amendment of 2026-11-25), on one Ollama server version, graded by the 1.0 release
# binary. Pass or fail only: nothing here is solved, harvested or trained on.
#
# gemma4-12b-c1 diverged in training and is never scored. Its retrain, gemma4-12b-c1-r2, answers as tag c1. Before the
# last pass this script waits for the retrain's export and eval history (eval_history.json beside copy.ok, written on
# the training host when training ends), stops if the final eval loss is more than twice the best, and imports it.
#
# The targeted runs keep the int8 models fv3 imported: on 2026-11-25 a scratch re-import of gemma4-12b-v3 at int8 on
# Ollama 0.40.17 gave the same 677 tensor digests and the same config as fv3's model, so the import step does not line
# up with the arm either.
#
# Before it starts, each control run's merged export is copied to $MERGED/<run>/merged_16bit and checked against the
# training host's sha256 (the file copy.ok marks it); this script waits for both markers, imports both control runs,
# and only then writes session_server_version.txt and starts the first pass.
# Run from a clone at the commit that fixed the reading: bash training/distill/c1-20261118/score.sh
# Re-run to continue: the harness resumes and every pass has a .done marker. A changed server version stops it.
set -u
export PATH=/opt/homebrew/bin:/usr/local/bin:$PATH
B=c1-20261118
SRC=$(cd "$(dirname "$0")/../../.." && pwd)
D=$SRC/training/distill/$B
FV3=$SRC/training/distill/fv3-20261105
Q8=$SRC/training/training_runs/gemma4-12b-v2-q8
G=${AETHER_GRADER:-$HOME/aether-models/gemma4-12b-v2-q8/grader/aether-1.0-macos-arm64/bin/aether}
GUARD=${OLLAMA_GUARD:-$HOME/aether-models/ollama_guard.sh}
MERGED=${AETHER_MERGED_ROOT:-$HOME/aether-models/merged}
SESSION=$D/session_server_version.txt
FINISHED=$D/finished
LEAKS='["thinking", "<think", "<|channel", "okay, "]'
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
[ "$(python3 -c 'import json,sys;print(len(json.load(open(sys.argv[1]))["tasks"]))' "$FV3/tasks_frozen.json")" = 1026 ] \
  || finish "FAILED fv3's frozen tasks are not the 1,026" 2

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
render_ok() {  # model: the same prompt rendering as the published int8 model, one BOS, thinking off
  python3 - "$1" aether-gemma4-12b-v2-q8 >> "$LOG" 2>&1 <<'PY'
import json, sys, urllib.request
new, published = sys.argv[1], sys.argv[2]
def render(model):
    body = {"model": model, "messages": [{"role": "user", "content": "PROMPT-HERE"}], "stream": False, "reasoning_effort": "none", "_debug_render_only": True, "max_tokens": 4}
    request = urllib.request.Request("http://localhost:11434/v1/chat/completions", json.dumps(body).encode(), {"Content-Type": "application/json"})
    return (json.load(urllib.request.urlopen(request, timeout=600)).get("_debug_info") or {}).get("rendered_template")
a, b = render(new), render(published)
print("rendered,", new, repr(a))
sys.exit(0 if a and a == b and a.count("<bos>") == 1 and a.endswith("<|channel>thought\n<channel|>") else 1)
PY
}

# 1. A control export copied and checked (copy.ok), with the targeted run's config, tokenizer and template.
check_export() {  # run
  local run=$1 f
  [ -f "$MERGED/$run/copy.ok" ] || log "waiting for $MERGED/$run/copy.ok"
  until [ -f "$MERGED/$run/copy.ok" ]; do sleep 300; done
  for f in config.json tokenizer.json chat_template.jinja; do
    cmp -s "$MERGED/$run/merged_16bit/$f" "$MERGED/gemma4-12b-v3/merged_16bit/$f" || finish "FAILED $run: $f differs from gemma4-12b-v3's" 1
  done
  cmp -s "$MERGED/$run/merged_16bit/model.safetensors" "$MERGED/gemma4-12b-v3/merged_16bit/model.safetensors" \
    && finish "FAILED $run: its weights are gemma4-12b-v3's" 1
  return 0
}
not_diverged() {  # run: the README's rule, final eval loss at most twice the best
  local run=$1 h=$MERGED/$1/eval_history.json
  [ -f "$h" ] || log "waiting for $h"
  until [ -f "$h" ]; do sleep 300; done
  python3 - "$h" >> "$LOG" 2>&1 <<'PY'
import json, sys
h = json.load(open(sys.argv[1]))
steps = sorted((int(s), float(l)) for s, l in h["eval"])
best, last = min(l for _, l in steps), steps[-1][1]
print(f"{h['run']}: {len(steps)} evals, best {best:.4f}, last {last:.4f} at step {steps[-1][0]}, ratio {last / best:.2f}")
sys.exit(0 if last <= 2 * best else 1)
PY
}
check_export gemma4-12b-c1-s42

# 2. Import the control runs at int8, as fv3 imported the targeted ones: c1_s42 before any pass, the retrain before its own.
import_run() {  # tag run
  local tag=$1 run=$2 m=aether-$2-q8
  ollama show "$m-32k" > /dev/null 2>&1 && return 0
  ( . "$GUARD" "$m"
    printf 'FROM %s\n' "$MERGED/$run/merged_16bit" > "$D/Modelfile.$tag"
    printf 'FROM %s\nPARAMETER num_ctx 32768\n' "$m" > "$D/Modelfile.$tag.32k"
    log "import $m (int8), Ollama $(server_version)"
    { ollama create "$m" -q int8 -f "$D/Modelfile.$tag" && ollama create "$m-32k" -f "$D/Modelfile.$tag.32k"; } > "$D/import.$tag.log" 2>&1 )
}
import_run c1_s42 gemma4-12b-c1-s42 || finish "FAILED import c1_s42" 1

# 3. The session: one server version, written before the first pass.
pass() {  # tag model
  local tag=$1 model=$2
  [ -f "$D/filter_$tag.done" ] && return 0
  ( . "$GUARD" "$model"
    server=$(server_version)
    [ -s "$SESSION" ] || echo "$server" > "$SESSION"
    [ "$server" = "$(cat "$SESSION")" ] || { log "the Ollama server is $server, the session runs on $(cat "$SESSION")"; exit 1; }
    ollama show "$model" > /dev/null 2>&1 || { log "no model $model"; exit 1; }
    log "=== $model, Ollama $server, $(ollama show "$model" | awk '/quantization/{print $2}'), $(ollama list | awk -v n="$model:latest" '$1==n{print $2, $3, $4}')"
    render_ok "${model%-32k}" || { log "render check failed: $tag"; exit 1; }
    dest_for "$model" "$D/dest_$tag.json"
    if [ ! -f "$D/smoke_$tag.ok" ]; then
      bench "$D/dest_$tag.json" "$Q8/smoke_tasks.json" "$D/smoke_$tag.json" >> "$D/filter_$tag.log" 2>&1
      python3 "$Q8/check_generations.py" "$D/smoke_$tag.json" "$LEAKS" >> "$LOG" 2>&1 || { log "smoke failed: $tag"; exit 1; }
      touch "$D/smoke_$tag.ok"
    fi
    log "--- filter $tag"
    retry bench "$D/dest_$tag.json" "$FV3/tasks_frozen.json" "$D/filter_$tag.json" >> "$D/filter_$tag.log" 2>&1
    rc=$?
    [ $rc -eq 0 ] && { python3 "$Q8/check_generations.py" "$D/filter_$tag.json" "$LEAKS" --complete >> "$LOG" 2>&1 || rc=5; }
    ollama stop "$model" >> "$D/filter_$tag.log" 2>&1; exit $rc ) && touch "$D/filter_$tag.done"
}
pass v3_c1s aether-gemma4-12b-v3-q8-32k || finish "FAILED pass v3_c1s; re-run to resume" 1
pass c1_s42 aether-gemma4-12b-c1-s42-q8-32k || finish "FAILED pass c1_s42; re-run to resume" 1
pass v3_s42_c1s aether-gemma4-12b-v3-s42-q8-32k || finish "FAILED pass v3_s42_c1s; re-run to resume" 1
if [ ! -f "$D/filter_c1.done" ]; then
  check_export gemma4-12b-c1-r2
  not_diverged gemma4-12b-c1-r2 || finish "FAILED gemma4-12b-c1-r2 diverged too: the arm has no seed-3407 run; the owner decides" 1
  import_run c1_r2 gemma4-12b-c1-r2 || finish "FAILED import c1_r2" 1
fi
pass c1 aether-gemma4-12b-c1-r2-q8-32k || finish "FAILED pass c1; re-run to resume" 1
[ "$(server_version)" = "$(cat "$SESSION")" ] || finish "FAILED the server changed after the last pass" 1
python3 "$D/compare.py" 2>&1 | tee -a "$LOG"
finish "DONE" 0
