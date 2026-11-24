#!/usr/bin/env python3
"""What the 2026-11-24 harness changes do to the archived attempts.

1. Extraction: for every archived attempt that stored the model's raw reply,
   the program today's sanitize_code() extracts against the stored one (and
   the pre-change function against the stored one, as a control).
2. Line endings: run every distinct archived Aether program once and count
   the runs whose raw stdout or stderr holds a carriage return.

usage: bench_archive_probe.py NEW_TREE OLD_HARNESS AETHER_BIN OUT_JSON
"""
import gzip, importlib.util, json, pathlib, sys, threading, hashlib, collections

new_tree, old_harness, aether_bin, out_path = sys.argv[1:5]
new_tree = pathlib.Path(new_tree)
sys.path.insert(0, str(new_tree / "tools"))
import aether_doc_bench as adb  # noqa: E402
import replay_bench  # noqa: E402

spec = importlib.util.spec_from_file_location("adb_old", old_harness)
old = importlib.util.module_from_spec(spec)
sys.modules["adb_old"] = old
spec.loader.exec_module(old)

paths = sorted(p for p in (new_tree / "bench" / "results").rglob("*") if p.is_file()
               and (p.name.endswith(".json") or p.name.endswith(".json.gz")))
resolver = replay_bench.TaskResolver()
counts = collections.Counter()
diffs = []
for path in paths:
    try:
        raw = gzip.decompress(path.read_bytes()).decode("utf-8") if path.suffix == ".gz" else path.read_text("utf-8")
        report = json.loads(raw)
    except Exception:
        counts["unreadable"] += 1
        continue
    if not isinstance(report, dict) or "destinations" not in report:
        continue
    counts["reports"] += 1
    for dest in report.get("destinations") or []:
        for variant in dest.get("variants") or []:
            for key in adb.CASE_KEYS:
                for case in variant.get(key) or []:
                    for index, attempt in enumerate(case.get("attempts") or []):
                        counts["attempts"] += 1
                        text = (attempt.get("generation") or {}).get("raw_text")
                        if not isinstance(text, str):
                            counts["no_raw_reply"] += 1
                            continue
                        counts["with_raw_reply"] += 1
                        runner = attempt.get("runner", "aether")
                        stored = attempt.get("source_code") or ""
                        was = old.sanitize_code(text)
                        now = adb.sanitize_code(text, runner)
                        if "```" in text:
                            counts["fenced_replies"] += 1
                        if was != stored:
                            counts["old_function_differs_from_stored"] += 1
                        if now != was:
                            counts["new_differs_from_old"] += 1
                            diffs.append({
                                "report": str(path.relative_to(new_tree)), "destination": dest.get("destination_id"),
                                "doc": variant.get("doc_name"), "key": key, "task_id": case.get("task_id"),
                                "repeat_index": case.get("repeat_index", 0), "attempt": index, "runner": runner,
                                "stored_exact": bool((attempt.get("run") or {}).get("exact_stdout_match")),
                                "tasks_file": report.get("tasks_file"), "tasks_version": report.get("tasks_version"),
                                "old": was, "new": now, "raw": text,
                            })

# Re-grade each changed Aether extraction with the same binary, old and new.
class Args:
    pass
args = Args()
args.aether_bin = pathlib.Path(aether_bin)
args.aether_args = []
args.sandbox_deny = "net,proc"
for d in diffs:
    if d["runner"] != "aether":
        continue
    rawtask, basis = resolver.resolve(d["tasks_file"], d["tasks_version"], d["task_id"])
    if rawtask is None:
        d["regrade"] = f"task not resolved ({basis})"
        continue
    task = replay_bench.to_task(rawtask)
    d["old_exact"] = bool(adb.compile_and_run(task, d["old"], args)["exact_stdout_match"]) if d["old"].strip() else False
    d["new_exact"] = bool(adb.compile_and_run(task, d["new"], args)["exact_stdout_match"]) if d["new"].strip() else False

# Carriage returns: every distinct archived Aether program, run once.
local = threading.local()
cr_hits = []
real_decode = adb._decode_output
def decode(data):
    if b"\r" in data:
        cr_hits.append((getattr(local, "what", None), data[:200].decode("utf-8", "replace")))
    return real_decode(data)
adb._decode_output = decode
real_run = adb.compile_and_run
def run(task, source, a):
    local.what = (task.task_id, hashlib.sha256(source.encode()).hexdigest()[:16])
    return real_run(task, source, a)
adb.compile_and_run = run
replay_out = pathlib.Path(out_path).with_suffix(".replay.json")
rc = replay_bench.main(["bench/results/**/*.json", "bench/results/**/*.json.gz", "--aether-bin", aether_bin,
                        "--report-json", str(replay_out)])
replay = json.loads(replay_out.read_text())

summary = {
    "counts": dict(counts),
    "changed_extractions": len(diffs),
    "changed_by_verdict": dict(collections.Counter(
        (f"{d.get('old_exact')}->{d.get('new_exact')}" if d["runner"] == "aether" else d["runner"]) for d in diffs)),
    "replay": {k: replay.get(k) for k in ("attempts_replayed", "programs_run")},
    "carriage_return_runs": len(cr_hits),
    "carriage_return_programs": sorted({str(w) for w, _ in cr_hits})[:50],
}
pathlib.Path(out_path).write_text(json.dumps({"summary": summary, "diffs": diffs, "cr": cr_hits[:200]}, indent=1))
print(json.dumps(summary, indent=1))
