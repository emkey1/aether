#!/bin/bash
# gemma4-12b-late on the scoring Mac: the one scoring session of README.md. The two late runs (gemma4-12b-v3-late and
# -v3-late-s42, imported at int8 here) and the two all-layer runs (gemma4-12b-v3 and -v3-s42, the int8 models fv3
# answered with) answer fv3's 1,026 frozen tasks with no guide and no repair, in the order v3_late, v3, v3_late_s42,
# v3_s42, on one Ollama server version, graded by the 1.0 release binary. Pass or fail only: nothing here is solved,
# harvested or trained on.
#
# Before the first pass, each late run's merged export must be copied to $MERGED/<run>/merged_16bit and checked against
# the training host's sha256 (copy.ok), with its eval history beside it (eval_history.json, written on the training
# host when training ends). A run whose final eval loss is more than twice its best has diverged (README.md, the rule of
# ../../distill/c1-20261118/): this script stops before importing it. Both late runs are imported before the session
# writes session_server_version.txt, so no import falls inside the session.
#
# Run from a clone at the commit that fixed the reading: bash training/training_runs/gemma4-12b-late/score.sh
# Re-run to continue: the harness resumes and every pass has a .done marker. A changed server version stops it.
set -u
export PATH=/opt/homebrew/bin:/usr/local/bin:$PATH
SRC=$(cd "$(dirname "$0")/../../.." && pwd)
D=$SRC/training/training_runs/gemma4-12b-late
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

# 1. A late export copied and checked (copy.ok), with the all-layer run's config, tokenizer and template, and its own weights.
check_export() {  # run
  local run=$1 f
  [ -f "$MERGED/$run/copy.ok" ] || log "waiting for $MERGED/$run/copy.ok"
  until [ -f "$MERGED/$run/copy.ok" ]; do sleep 300; done
  for f in config.json tokenizer.json chat_template.jinja; do
    cmp -s "$MERGED/$run/merged_16bit/$f" "$MERGED/gemma4-12b-v3/merged_16bit/$f" || finish "FAILED $run: $f differs from gemma4-12b-v3's" 1
  done
  for other in gemma4-12b-v3 gemma4-12b-v3-s42; do
    cmp -s "$MERGED/$run/merged_16bit/model.safetensors" "$MERGED/$other/merged_16bit/model.safetensors" \
      && finish "FAILED $run: its weights are $other's" 1
  done
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

# 2. Import the late runs at int8, as fv3 imported the all-layer ones.
import_run() {  # tag run
  local tag=$1 run=$2 m=aether-$2-q8
  ollama show "$m-32k" > /dev/null 2>&1 && return 0
  ( . "$GUARD" "$m"
    printf 'FROM %s\n' "$MERGED/$run/merged_16bit" > "$D/Modelfile.$tag"
    printf 'FROM %s\nPARAMETER num_ctx 32768\n' "$m" > "$D/Modelfile.$tag.32k"
    log "import $m (int8), Ollama $(server_version)"
    { ollama create "$m" -q int8 -f "$D/Modelfile.$tag" && ollama create "$m-32k" -f "$D/Modelfile.$tag.32k"; } > "$D/import.$tag.log" 2>&1 )
}
for pair in v3_late:gemma4-12b-v3-late v3_late_s42:gemma4-12b-v3-late-s42; do
  tag=${pair%%:*} run=${pair#*:}
  [ -f "$D/filter_$tag.done" ] && continue
  check_export "$run"
  not_diverged "$run" || finish "FAILED $run diverged: not scored; retrain it once under a new tag (README.md)" 1
  import_run "$tag" "$run" || finish "FAILED import $tag" 1
done

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
pass v3_late aether-gemma4-12b-v3-late-q8-32k || finish "FAILED pass v3_late; re-run to resume" 1
pass v3 aether-gemma4-12b-v3-q8-32k || finish "FAILED pass v3; re-run to resume" 1
pass v3_late_s42 aether-gemma4-12b-v3-late-s42-q8-32k || finish "FAILED pass v3_late_s42; re-run to resume" 1
pass v3_s42 aether-gemma4-12b-v3-s42-q8-32k || finish "FAILED pass v3_s42; re-run to resume" 1
[ "$(server_version)" = "$(cat "$SESSION")" ] || finish "FAILED the server changed after the last pass" 1
python3 "$D/compare.py" 2>&1 | tee -a "$LOG"
finish "DONE" 0
