#!/bin/bash
# frestore-20261127 on the training host (README.md): build the two candidates, then one scoring session. Each model is
# served at bf16 from its merged export by one pinned vLLM image and answers fv3's 1,026 frozen tasks with no guide and
# no repair, WORKERS requests at a time: in Aether (graded by the 1.0 release built for Linux) and in Python (run by the
# host's python3 in the harness's docker sandbox). Order: r16 (Aether, Python), v3 (Aether, Python), base, e16, v3_late
# (Python). Pass or fail only: nothing here is solved, harvested or trained on.
#
# session_server_version.txt records the image, the grader, the worker count, the host's python3 and the sandbox image
# before the first pass; every pass checks them and stops if one changed.
#
# Run from a clone at the commit that fixed the reading: bash training/distill/frestore-20261127/score_host.sh
# Re-run to continue: the builds and every pass have .done markers, and the harness resumes.
set -u
SRC=$(cd "$(dirname "$0")/../../.." && pwd)
D=$SRC/training/distill/frestore-20261127
FV3=$SRC/training/distill/fv3-20261105
Q8=$SRC/training/training_runs/gemma4-12b-v2-q8
STORE=/storage/aether-train
OUT=$STORE/frestore-20261127
BASE=/storage/models/Gemma-4-12B-it
G=${AETHER_GRADER:-$HOME/aether-1.0/aether-1.0-src/build/aether}
IMAGE=sha256:30a38a1d74a17365eca400e83ffd885b250e0c8c0d3c5b508afa8c412d2ddf95  # vllm/vllm-openai, built 2026-07-13
SANDBOX=ubuntu:24.04
WORKERS=16
NAME=aether-frestore-serve PORT=8019
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
SANDBOX_ID=$(docker image inspect -f '{{.Id}}' "$SANDBOX" 2>/dev/null) || finish "FAILED no sandbox image $SANDBOX" 2
log "clone at $(git rev-parse --short HEAD); README $(git log -1 --format=%h -- "$D/README.md")"
git diff --quiet HEAD -- "$D/README.md" "$D/compare.py" "$D/score_host.sh" "$D/build_restore.py" \
  || finish "FAILED the reading differs from the committed one" 2
[ "$(python3 -c 'import json,sys;print(len(json.load(open(sys.argv[1]))["tasks"]))' "$FV3/tasks_frozen.json")" = 1026 ] \
  || finish "FAILED fv3's frozen tasks are not the 1,026" 2
STAMP=$(printf 'vllm image %s\ngrader %s\nworkers %s\npython %s\nsandbox %s %s\n' "$IMAGE" "$GSHA" "$WORKERS" \
  "$(python3 --version 2>&1)" "$SANDBOX" "$SANDBOX_ID")

# 1. The models. v3 and v3_late are their merged exports; base is the untrained model; r16 and e16 are built here.
declare -A MODEL=([v3]=$STORE/gemma4-12b-v3/merged_16bit [v3_late]=$STORE/gemma4-12b-v3-late/merged_16bit
                  [base]=$BASE [r16]=$OUT/r16/merged_16bit [e16]=$OUT/e16/merged_16bit)
for tag in v3 v3_late base; do
  for f in config.json tokenizer.json chat_template.jinja model.safetensors; do
    [ -s "${MODEL[$tag]}/$f" ] || finish "FAILED $tag: no $f in ${MODEL[$tag]}" 1
  done
  for f in config.json tokenizer.json chat_template.jinja; do  # one rendering for every model
    cmp -s "${MODEL[$tag]}/$f" "${MODEL[v3]}/$f" || finish "FAILED $tag: $f differs from gemma4-12b-v3's" 1
  done
done
build() {  # tag tuned-layers
  [ -f "$OUT/$1.done" ] && return 0
  log "--- build $1: layers $2 from gemma4-12b-v3, the other target layers from the base"
  python3 -u "$D/build_restore.py" --base "$BASE" --tuned "${MODEL[v3]}" --tuned-layers "$2" --out "${MODEL[$1]}" \
    >> "$D/build_$1.log" 2>&1 && tail -1 "$D/build_$1.log" | grep -qx BUILT || return 1
  cp "${MODEL[$1]}/restore_manifest.json" "$D/restore_manifest_$1.json" && touch "$OUT/$1.done"
}
mkdir -p "$OUT"
build r16 16-47 || finish "FAILED build r16; see build_r16.log" 1
build e16 0-15 || finish "FAILED build e16; see build_e16.log" 1

bench() {  # destinations tasks report [extra flags]
  local dest=$1 tasks=$2 report=$3
  shift 3
  AETHER_BENCH_WORKERS=$WORKERS AETHER_BENCH_SANDBOX_IMAGE=$SANDBOX python3 -u tools/aether_doc_bench.py \
    --destinations-config "$dest" --tasks "$tasks" --repeats 1 --seed-base 42 --aether-bin "$G" --aether-bin-sha256 "$GSHA" \
    --allow-skew --keep-loaded --resume --output-json "$report" --docs none --repair-attempts 0 --text-summary "$@"
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

# 2. The session: one image, grader, worker count, python3 and sandbox, written before the first pass.
serve() {  # tag
  local tag=$1 served=frestore-$1 i
  docker rm -f "$NAME" > /dev/null 2>&1
  docker run -d --name "$NAME" --gpus all --ipc=host --shm-size=16g -v /storage:/storage -p "$PORT:8000" "$IMAGE" \
    --model "${MODEL[$tag]}" --served-model-name "$served" --dtype bfloat16 --max-model-len 32768 \
    --gpu-memory-utilization 0.85 --port 8000 --enforce-eager >> "$D/serve_$tag.log" 2>&1 || { log "docker run failed: $tag"; return 1; }
  for i in $(seq 1 180); do
    curl -s -m 5 "localhost:$PORT/v1/models" | grep -q "\"$served\"" && break
    [ "$(docker inspect -f '{{.State.Running}}' "$NAME" 2>/dev/null)" = true ] || break
    sleep 10
  done
  curl -s -m 5 "localhost:$PORT/v1/models" | python3 -c 'import json,sys
m = json.load(sys.stdin)["data"][0]; sys.exit(0 if m["id"] == sys.argv[1] and m.get("root") == sys.argv[2] else 1)' "$served" "${MODEL[$tag]}" \
    || { log "not serving $tag"; docker logs --tail 60 "$NAME" >> "$D/serve_$tag.log" 2>&1; return 1; }
  dest_for "$served" "$D/dest_$tag.json"
  log "=== $tag: ${MODEL[$tag]}, bf16, image ${IMAGE:7:12}, $WORKERS workers"
  if [ ! -f "$D/smoke_$tag.ok" ]; then
    bench "$D/dest_$tag.json" "$Q8/smoke_tasks.json" "$D/smoke_$tag.json" >> "$D/pass_$tag.log" 2>&1
    python3 "$Q8/check_generations.py" "$D/smoke_$tag.json" "$LEAKS" >> "$LOG" 2>&1 || { log "smoke failed: $tag"; return 1; }
    touch "$D/smoke_$tag.ok"
  fi
}
pass() {  # kind tag
  local kind=$1 tag=$2 rc
  [ -f "$D/${kind}_$tag.done" ] && return 0
  [ -s "$SESSION" ] || printf '%s' "$STAMP" > "$SESSION"
  [ "$STAMP" = "$(cat "$SESSION")" ] || { log "the session ran on $(tr '\n' ' ' < "$SESSION"), now $(echo "$STAMP" | tr '\n' ' ')"; return 1; }
  log "--- $kind $tag"
  if [ "$kind" = python ]; then
    retry bench "$D/dest_$tag.json" "$FV3/tasks_frozen.json" "$D/python_$tag.json" --python-baseline --skip-aether >> "$D/pass_$tag.log" 2>&1
  else
    retry bench "$D/dest_$tag.json" "$FV3/tasks_frozen.json" "$D/aether_$tag.json" >> "$D/pass_$tag.log" 2>&1
  fi
  rc=$?
  [ $rc -eq 0 ] && { python3 "$D/check_report.py" "$D/${kind}_$tag.json" "$LEAKS" >> "$LOG" 2>&1 || rc=5; }
  [ $rc -eq 0 ] && touch "$D/${kind}_$tag.done"
  return $rc
}
model() {  # tag kinds...
  local tag=$1 kind todo=0
  shift
  for kind in "$@"; do [ -f "$D/${kind}_$tag.done" ] || todo=1; done
  [ $todo = 1 ] || return 0
  [ -s "$SESSION" ] && { [ "$STAMP" = "$(cat "$SESSION")" ] || { log "the session ran on $(tr '\n' ' ' < "$SESSION"), now $(echo "$STAMP" | tr '\n' ' ')"; return 1; }; }
  serve "$tag" || return 1
  for kind in "$@"; do pass "$kind" "$tag" || { docker logs "$NAME" > "$D/serve_$tag.log" 2>&1; return 1; }; done
  docker logs "$NAME" > "$D/serve_$tag.log" 2>&1
  docker rm -f "$NAME" > /dev/null 2>&1
}
model r16 aether python || finish "FAILED r16; re-run to resume" 1
model v3 aether python || finish "FAILED v3; re-run to resume" 1
model base python || finish "FAILED base; re-run to resume" 1
model e16 python || finish "FAILED e16; re-run to resume" 1
model v3_late python || finish "FAILED v3_late; re-run to resume" 1
python3 "$D/compare.py" 2>&1 | tee -a "$LOG"
finish "DONE" 0
