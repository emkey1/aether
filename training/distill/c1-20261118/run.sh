#!/bin/bash
# c1-20261118 on the scoring Mac: the control arm's pairs. The same solvers, guide and grader as f3-20261103 (the guided
# untrained Qwen3.8-27B under the Mac's one-model lock, then MiMo with the guide through T'Ra's open lane on what the
# 27B missed, both with two repairs), on 340 tasks 8-bit gemma4-12b-v2 passed (choose_tasks.py). Harvest keeps agreeing
# programs; check_pairs gates them; exactly 298 are kept (seeded), the targeted batch's count. It stops there: no
# dataset, no training.
# Run from a clone of the repo at the commit that fixed the reading: bash training/distill/c1-20261118/run.sh
# The two destination files are private (endpoints): DEST_DIR (default: the f3 batch folder of the clone that ran f3)
# must hold dest_base_guided_m5.json and dest_mimo_guided.json. Re-run to continue: each stage has a .done marker.
set -u
export PATH=/opt/homebrew/bin:/usr/local/bin:$PATH
B=c1-20261118
SRC=$(cd "$(dirname "$0")/../../.." && pwd)
D=$SRC/training/distill/$B
F3=$SRC/training/distill/f3-20261103
DEST_DIR=${DEST_DIR:-$HOME/aether-models/src-f1/training/distill/f3-20261103}
G=${AETHER_GRADER:-$HOME/aether-models/gemma4-12b-v2-q8/grader/aether-1.0-macos-arm64/bin/aether}
CARD=$D/medium_guide_1.0.md
GUIDE_SHA=b693a67bc16cba2f1e61c041903ecf338e39d2b4ad34c6bbae20b8e4b7f933d8
BASE_MODEL=aether-qwen38-27b-base-32k
KEEP=298
LOG=$D/driver.log
log() { echo "[$(date -u +%Y-%m-%dT%H:%M:%SZ)] $*" | tee -a "$LOG"; }
finish() { log "$1"; echo "$1" > "$D/finished"; exit "$2"; }
rm -f "$D/finished"
cd "$SRC" || finish "FAILED no clone" 2
GSHA=$(shasum -a 256 "$G" | cut -d' ' -f1)
[ "$GSHA" = b968240e919cf7e577389cef8a3c66b9068d7c0fcb507ec9edd630fa6f5aed25 ] || finish "FAILED the grader is not the 1.0 release" 2
git diff --quiet HEAD -- "$D/README.md" "$D/choose_tasks.py" "$D/tasks_passed.json" || finish "FAILED the reading or the task choice differs from the committed one" 2
for f in dest_base_guided_m5.json dest_mimo_guided.json; do
  [ -f "$D/$f" ] || cp "$DEST_DIR/$f" "$D/$f" || finish "FAILED no $f in $DEST_DIR" 2
done
git show 1.0:docs/aether_for_llms_medium_contexts.md > "$CARD"
[ "$(shasum -a 256 "$CARD" | cut -d' ' -f1)" = "$GUIDE_SHA" ] || finish "FAILED the guide is not the 1.0 medium guide" 2
log "clone at $(git rev-parse --short HEAD)"
bench() {  # destinations tasks report [extra...]
  local dest=$1 tasks=$2 report=$3; shift 3
  python3 -u tools/aether_doc_bench.py --destinations-config "$dest" --tasks "$tasks" --repeats 1 --seed-base 42 \
    --aether-bin "$G" --aether-bin-sha256 "$GSHA" --allow-skew --keep-loaded --resume --output-json "$report" \
    "$@" --text-summary
}
retry() {  # the harness exits 3 when a case failed in the infrastructure; --resume re-runs only those
  local n rc
  for n in 1 2 3; do "$@"; rc=$?; [ $rc -ne 3 ] && return $rc; log "infra failures (try $n), resuming"; sleep 60; done
  return $rc
}
check() {  # report: the guide is the 1.0 medium guide and every case was measured
  python3 tools/check_bench_report.py "$1" --complete --guide "card=$GUIDE_SHA" --leak thinking --leak "<think" | tee -a "$LOG"
  return "${PIPESTATUS[0]}"
}

if [ ! -f "$D/aether_base.done" ]; then
  log "--- aether_base: guided Qwen3.8-27B on the passed sample"
  ( . "$SRC/tools/ollama_guard.sh" "$BASE_MODEL"
    retry bench "$D/dest_base_guided_m5.json" "$D/tasks_passed.json" "$D/aether_base.json" \
      --doc "card=$CARD" --docs card --repair-attempts 2 >> "$D/aether_base.log" 2>&1
    rc=$?; ollama stop "$BASE_MODEL" >> "$D/aether_base.log" 2>&1; exit $rc ) \
    || finish "FAILED aether_base; re-run to resume" 1
  check "$D/aether_base.json" || finish "FAILED aether_base report check" 1
  touch "$D/aether_base.done"
fi
if [ ! -f "$D/aether_mimo.done" ]; then
  python3 - "$D/tasks_passed.json" "$D/aether_base.json" "$D/tasks_left.json" <<'PY' 2>&1 | tee -a "$LOG"
import json, sys
sample = json.load(open(sys.argv[1]))
r = json.load(open(sys.argv[2]))["destinations"][0]["variants"][0]["results"]
solved = {x["task_id"] for x in r if any((a.get("run") or {}).get("exact_stdout_match") for a in x.get("attempts", []))}
left = [t for t in sample["tasks"] if t["id"] not in solved]
json.dump(sample | {"description": "passed-sample tasks the guided 27B did not solve", "tasks": left},
          open(sys.argv[3], "w"), indent=1, ensure_ascii=False)
print(f"27B solved {len(solved)} of {len(sample['tasks'])}; left for MiMo: {len(left)}")
PY
  if [ "$(python3 -c 'import json,sys;print(len(json.load(open(sys.argv[1]))["tasks"]))' "$D/tasks_left.json")" -gt 0 ]; then
    log "--- aether_mimo: MiMo with the guide on the 27B's misses"
    retry bench "$D/dest_mimo_guided.json" "$D/tasks_left.json" "$D/aether_mimo.json" \
      --doc "card=$CARD" --docs card --repair-attempts 2 >> "$D/aether_mimo.log" 2>&1 || finish "FAILED aether_mimo; re-run to resume" 1
    check "$D/aether_mimo.json" || finish "FAILED aether_mimo report check" 1
  else
    python3 -c 'import json,sys;r=json.load(open(sys.argv[1]));r["destinations"][0]["variants"][0]["results"]=[];json.dump(r,open(sys.argv[2],"w"))' \
      "$D/aether_base.json" "$D/aether_mimo.json"
  fi
  touch "$D/aether_mimo.done"
fi
if [ ! -f "$D/harvest.done" ]; then
  python3 "$F3/merge_solvers.py" "$D" 2>&1 | tee -a "$LOG"
  [ "${PIPESTATUS[0]}" = 0 ] || finish "FAILED merge" 1
  python3 tools/aether_distill.py "$B" harvest --from passed 2>&1 | tee -a "$LOG"
  [ "${PIPESTATUS[0]}" = 0 ] || finish "FAILED harvest" 1
  python3 - "$D/pairs.json" "$KEEP" <<'PY' 2>&1 | tee -a "$LOG"
import json, random, sys
path, keep = sys.argv[1], int(sys.argv[2])
pairs = json.load(open(path))["pairs"]
json.dump({"pairs": pairs}, open(path.replace("pairs.json", "pairs_all.json"), "w"), indent=1, ensure_ascii=False)
if len(pairs) < keep:
    raise SystemExit(f"STOP: harvested {len(pairs)} pairs, fewer than {keep}")
chosen = sorted(random.Random(20261118).sample(pairs, keep), key=lambda p: p["id"])
json.dump({"pairs": chosen}, open(path, "w"), indent=1, ensure_ascii=False)
print(f"kept {keep} of {len(pairs)} harvested pairs (seeded); all are in pairs_all.json")
PY
  [ "${PIPESTATUS[0]}" = 0 ] || finish "FAILED choosing $KEEP pairs" 1
  python3 tools/aether_specialization_check_pairs.py "$D/pairs.json" --aether-bin "$G" 2>&1 | tail -5 | tee -a "$LOG"
  [ "${PIPESTATUS[0]}" = 0 ] || finish "FAILED check_pairs" 1
  touch "$D/harvest.done"
fi
finish "DONE $KEEP pairs in pairs.json; nothing trained" 0
