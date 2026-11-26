#!/bin/bash
# gemma4-12b-late on the training host: the preliminary scoring session of README.md's amendments of 2026-11-26
# (reported, not deciding; the Mac session of score.sh decides). Its reports are kept as report__host_<tag>.json.gz
# with its session_server_version.txt as session_host.txt, read by compare.py --host. The two late runs and the two all-layer runs (gemma4-12b-v3 and -v3-s42) are each served
# at bf16 from their merged export by one pinned vLLM image, with the driver's host-scoring flags, and answer fv3's
# 1,026 frozen tasks with no guide and no repair, WORKERS requests at a time, in the order v3_late, v3, v3_late_s42,
# v3_s42, graded by the 1.0 release built for Linux. Pass or fail only: nothing here is solved, harvested or trained on.
#
# Before the first pass, both late exports must be complete on the shared storage with their eval histories; a run
# whose final eval loss is more than twice its best has diverged (README.md) and stops the session before it is
# served. session_server_version.txt records the image, the grader and the worker count before the first pass; every
# pass checks all three and stops if one changed.
#
# Run from a clone at the commit that fixed the amendment: bash training/training_runs/gemma4-12b-late/score_host.sh
# Re-run to continue: the harness resumes and every pass has a .done marker.
set -u
SRC=$(cd "$(dirname "$0")/../../.." && pwd)
D=$SRC/training/training_runs/gemma4-12b-late
FV3=$SRC/training/distill/fv3-20261105
Q8=$SRC/training/training_runs/gemma4-12b-v2-q8
STORE=/storage/aether-train
G=${AETHER_GRADER:-$HOME/aether-1.0/aether-1.0-src/build/aether}
IMAGE=sha256:30a38a1d74a17365eca400e83ffd885b250e0c8c0d3c5b508afa8c412d2ddf95  # vllm/vllm-openai, built 2026-07-13
WORKERS=16
NAME=aether-late-serve PORT=8019
SESSION=$D/session_server_version.txt
FINISHED=$D/finished
LEAKS='["thinking", "<think", "<|channel", "okay, "]'
rm -f "$FINISHED"
LOG=$D/driver.log
log() { echo "[$(date -u +%Y-%m-%dT%H:%M:%SZ)] $*" | tee -a "$LOG"; }
finish() { log "$1"; echo "$1" > "$FINISHED"; exit "$2"; }
cleanup() { docker rm -f "$NAME" > /dev/null 2>&1; }
trap cleanup EXIT
cd "$SRC" || finish "FAILED no clone" 2
GSHA=$(sha256sum "$G" | cut -d' ' -f1)
[ "$GSHA" = 923415105622a84c42316ee50206e8459b557b2cce4b0fb4caf93446f6ff242b ] || finish "FAILED the grader is not the 1.0 release build" 2
case "$("$G" --version 2>&1 | head -1)" in *2026-10-30-1*) ;; *) finish "FAILED the grader is not language 2026-10-30-1" 2 ;; esac
docker image inspect "$IMAGE" > /dev/null 2>&1 || finish "FAILED no vLLM image $IMAGE" 2
log "clone at $(git rev-parse --short HEAD); README $(git log -1 --format=%h -- "$D/README.md")"
git diff --quiet HEAD -- "$D/README.md" "$D/compare.py" "$D/score_host.sh" || finish "FAILED the reading differs from the committed one" 2
[ "$(python3 -c 'import json,sys;print(len(json.load(open(sys.argv[1]))["tasks"]))' "$FV3/tasks_frozen.json")" = 1026 ] \
  || finish "FAILED fv3's frozen tasks are not the 1,026" 2
STAMP=$(printf 'vllm image %s\ngrader %s\nworkers %s\n' "$IMAGE" "$GSHA" "$WORKERS")

bench() {  # destinations tasks report
  AETHER_BENCH_WORKERS=$WORKERS python3 -u tools/aether_doc_bench.py --destinations-config "$1" --tasks "$2" --repeats 1 --seed-base 42 \
    --aether-bin "$G" --aether-bin-sha256 "$GSHA" --allow-skew --keep-loaded --resume --output-json "$3" --docs none \
    --repair-attempts 0 --text-summary
}
retry() {  # the harness exits 3 when a case failed in the infrastructure; --resume re-runs only those
  local n rc
  for n in 1 2 3; do "$@"; rc=$?; [ $rc -ne 3 ] && return $rc; log "infra failures (try $n), resuming"; sleep 60; done
  return $rc
}
dest_for() {  # served-name file
  python3 - "$1" "$2" "$PORT" <<'PY'
import json, sys
model, path, port = sys.argv[1:]
json.dump({"destinations": [{"id": model + "-host-a-vllm", "type": "openai_chat_completions", "base_url": f"http://localhost:{port}/v1",
                             "api_key": "", "model": model, "temperature": 0, "max_output_tokens": 8192, "prompt_context_limit": 32768,
                             "request_timeout_seconds": 3600, "request_max_retries": 2, "retry_backoff_seconds": 5, "cooldown_seconds": 0,
                             "extra_body": {"chat_template_kwargs": {"enable_thinking": False}}}]}, open(path, "w"), indent=1)
PY
}

# 1. Both late exports complete, with v3's config, tokenizer and template and their own weights, and not diverged.
check_export() {  # run
  local run=$1 f other
  for f in config.json tokenizer.json chat_template.jinja model.safetensors; do
    [ -s "$STORE/$run/merged_16bit/$f" ] || finish "FAILED $run: no $f in its export" 1
  done
  for f in config.json tokenizer.json chat_template.jinja; do
    cmp -s "$STORE/$run/merged_16bit/$f" "$STORE/gemma4-12b-v3/merged_16bit/$f" || finish "FAILED $run: $f differs from gemma4-12b-v3's" 1
  done
  for other in gemma4-12b-v3 gemma4-12b-v3-s42; do
    cmp -s "$STORE/$run/merged_16bit/model.safetensors" "$STORE/$other/merged_16bit/model.safetensors" \
      && finish "FAILED $run: its weights are $other's" 1
  done
  python3 - "$STORE/$run/eval_history.json" >> "$LOG" 2>&1 <<'PY' || finish "FAILED $run diverged: not scored; retrain it once under a new tag (README.md)" 1
import json, sys
h = json.load(open(sys.argv[1]))
steps = sorted((int(s), float(l)) for s, l in h["eval"])
best, last = min(l for _, l in steps), steps[-1][1]
print(f"{h['run']}: {len(steps)} evals, best {best:.4f}, last {last:.4f} at step {steps[-1][0]}, ratio {last / best:.2f}")
sys.exit(0 if last <= 2 * best else 1)
PY
}
check_export gemma4-12b-v3-late
check_export gemma4-12b-v3-late-s42

# 2. The session: one image, one grader, one worker count, written before the first pass.
pass() {  # tag run
  local tag=$1 run=$2 served=late-$1 i rc
  [ -f "$D/filter_$tag.done" ] && return 0
  [ -s "$SESSION" ] || printf '%s' "$STAMP" > "$SESSION"
  [ "$STAMP" = "$(cat "$SESSION")" ] || { log "the session ran on $(tr '\n' ' ' < "$SESSION"), now $(echo "$STAMP" | tr '\n' ' ')"; return 1; }
  docker rm -f "$NAME" > /dev/null 2>&1
  docker run -d --name "$NAME" --gpus all --ipc=host --shm-size=16g -v /storage:/storage -p "$PORT:8000" "$IMAGE" \
    --model "$STORE/$run/merged_16bit" --served-model-name "$served" --dtype bfloat16 --max-model-len 32768 \
    --gpu-memory-utilization 0.85 --port 8000 --enforce-eager >> "$D/serve_$tag.log" 2>&1 || { log "docker run failed: $tag"; return 1; }
  for i in $(seq 1 180); do
    curl -s -m 5 "localhost:$PORT/v1/models" | grep -q "\"$served\"" && break
    [ "$(docker inspect -f '{{.State.Running}}' "$NAME" 2>/dev/null)" = true ] || break
    sleep 10
  done
  curl -s -m 5 "localhost:$PORT/v1/models" | python3 -c 'import json,sys
m = json.load(sys.stdin)["data"][0]; sys.exit(0 if m["id"] == sys.argv[1] and m.get("root") == sys.argv[2] else 1)' "$served" "$STORE/$run/merged_16bit" \
    || { log "not serving $run: $tag"; docker logs --tail 60 "$NAME" >> "$D/serve_$tag.log" 2>&1; return 1; }
  log "=== $tag: $run, bf16, image ${IMAGE:7:12}, $WORKERS workers"
  dest_for "$served" "$D/dest_$tag.json"
  if [ ! -f "$D/smoke_$tag.ok" ]; then
    bench "$D/dest_$tag.json" "$Q8/smoke_tasks.json" "$D/smoke_$tag.json" >> "$D/filter_$tag.log" 2>&1
    python3 "$Q8/check_generations.py" "$D/smoke_$tag.json" "$LEAKS" >> "$LOG" 2>&1 || { log "smoke failed: $tag"; return 1; }
    touch "$D/smoke_$tag.ok"
  fi
  log "--- filter $tag"
  retry bench "$D/dest_$tag.json" "$FV3/tasks_frozen.json" "$D/filter_$tag.json" >> "$D/filter_$tag.log" 2>&1
  rc=$?
  [ $rc -eq 0 ] && { python3 "$Q8/check_generations.py" "$D/filter_$tag.json" "$LEAKS" --complete >> "$LOG" 2>&1 || rc=5; }
  docker logs "$NAME" > "$D/serve_$tag.log" 2>&1
  docker rm -f "$NAME" > /dev/null 2>&1
  [ $rc -eq 0 ] && touch "$D/filter_$tag.done"
  return $rc
}
pass v3_late gemma4-12b-v3-late || finish "FAILED pass v3_late; re-run to resume" 1
pass v3 gemma4-12b-v3 || finish "FAILED pass v3; re-run to resume" 1
pass v3_late_s42 gemma4-12b-v3-late-s42 || finish "FAILED pass v3_late_s42; re-run to resume" 1
pass v3_s42 gemma4-12b-v3-s42 || finish "FAILED pass v3_s42; re-run to resume" 1
python3 "$D/compare.py" 2>&1 | tee -a "$LOG"
finish "DONE" 0
