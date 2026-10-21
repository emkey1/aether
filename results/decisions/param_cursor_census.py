#!/usr/bin/env python3
"""PARAM-001 census (D77): which programs does the lost-cursor warning fire on?

Collects every Aether program the repo holds and compiles each one once
(`--no-run --no-cache`, deduplicated by source sha256) with
AETHER_EXPERIMENT_LOG set, then counts per status:

- known correct: the training pairs, the distilled pairs and the repair drills'
  fixed sources (training/), the corpus candidates, the dataset records
  (training/**/aether_*_sft.jsonl), the passing tests/ and examples/
  programs, and every archived benchmark attempt graded `pass`;
- failing: every other archived attempt, the drills' broken sources, and the
  tests/ `_fail`, `_warn` and backlog programs.

Archived attempts come from bench/results/**/*.json[.gz],
training/training_runs/**/*.json[.gz] and training/distill/*/report__aether*,
plus any --reports glob (reports kept outside the repo). A program counts as
correct only if every record of it is. Only programs that compile without an
error are judged: PARAM-001 is a warning on a program that compiles.

`naive` is the rule D77 rejected: a function assigns a scalar parameter and does
not return it (the `param-lost` log rows). `PARAM-001` is the shipped warning.

    python3 results/decisions/param_cursor_census.py --aether-bin build/aether \
        [--reports 'other/**/*.json'] [--json out.json] [--workers 6]
"""

from __future__ import annotations

import argparse
import collections
import concurrent.futures
import glob
import gzip
import hashlib
import json
import os
import pathlib
import subprocess
import sys
import tempfile

REPO = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools"))
import aether_doc_bench as adb  # noqa: E402


def collect(extra_reports: list[str]) -> list[dict]:
    out: list[dict] = []

    def add(group, ident, src, status, path=None, files=None, task=None):
        if src and src.strip():
            out.append({"group": group, "id": ident, "status": status, "source": src, "path": path,
                        "files": files, "task": task,
                        "sha": hashlib.sha256(src.encode("utf-8")).hexdigest()})

    training = REPO / "training"
    for p in [training / "seed_instruction_pairs.json"] + sorted((training / "distilled").glob("*.json")):
        for it in json.loads(p.read_text())["pairs"]:
            add("training", f"{p.name}:{it['id']}", it["solution"], "correct", files=it.get("files"))
    for p in sorted(training.glob("seed_repair_pairs*.json")):
        for it in json.loads(p.read_text())["pairs"]:
            add("training", f"{p.name}:{it['id']}", it["fixed_source"], "correct", files=it.get("files"))
            add("training", f"{p.name}:{it['id']}:broken", it["broken_source"], "failing", files=it.get("files"))
    skip = {".json", ".md", ".bak", ".txt", ".jsonl", ".py", ".out", ".gz", ".csv", ".tsv", ".log"}
    for f in sorted(training.rglob("*")):
        if f.is_file() and ("corpus_candidates" in str(f) or f.suffix == ".aether") and f.suffix not in skip:
            add("training", str(f.relative_to(REPO)), f.read_text(errors="replace"), "correct", path=str(f))
    for p in sorted(training.rglob("aether_*_sft.jsonl")):
        for line in p.read_text().splitlines():
            try:
                rec = json.loads(line)
            except ValueError:
                continue
            msgs = rec.get("messages") or []
            if msgs and msgs[-1].get("role") == "assistant":
                add("training", f"{p.name}:{rec.get('id')}", msgs[-1]["content"], "correct", files=rec.get("files"))
    for f in sorted((REPO / "tests").rglob("*.aether")):
        rel = str(f.relative_to(REPO))
        add("tests", rel, f.read_text(errors="replace"),
            "failing" if "_fail" in f.name or "_warn" in f.name or "/backlog/" in rel else "correct",
            path=str(f))
    for f in sorted((REPO / "examples").rglob("*")):
        if f.is_file() and f.suffix in ("", ".aether"):
            add("examples", str(f.relative_to(REPO)), f.read_text(errors="replace"), "correct", path=str(f))

    patterns = [str(REPO / "bench/results/**/*.json*"), str(REPO / "training/training_runs/**/*.json*"),
                str(REPO / "training/distill/*/report__aether*.json.gz")] + extra_reports
    for pattern in patterns:
        for path in sorted(glob.glob(pattern, recursive=True)):
            try:
                raw = pathlib.Path(path).read_bytes()
                report = json.loads(gzip.decompress(raw) if path.endswith(".gz") else raw)
            except (OSError, ValueError):
                continue
            if not isinstance(report, dict) or "destinations" not in report:
                continue
            for dest in report.get("destinations", []):
                for variant in dest.get("variants", []):
                    for case in variant.get("results", []):
                        for i, att in enumerate(case.get("attempts") or []):
                            if att.get("runner", "aether") != "aether":
                                continue
                            klass = adb.classify_attempt(att)
                            add("archived", f"{os.path.basename(path)}:{case.get('task_id')}:{i}",
                                att.get("source_code") or "", "correct" if klass == "pass" else "failing",
                                task=f"{case.get('task_id')} ({klass})")
    return out


def compile_one(aether: str, rec: dict) -> dict:
    with tempfile.TemporaryDirectory() as td:
        log = os.path.join(td, "param.log")
        if rec["path"]:
            cwd, target = os.path.dirname(rec["path"]), rec["path"]
        else:
            cwd, target = td, os.path.join(td, "prog.aether")
            pathlib.Path(target).write_text(rec["source"])
            for name, body in (rec.get("files") or {}).items():
                fp = pathlib.Path(td) / name
                fp.parent.mkdir(parents=True, exist_ok=True)
                fp.write_text(body)
        try:
            run = subprocess.run([aether, "--no-run", "--no-cache", target], cwd=cwd, capture_output=True,
                                 text=True, errors="replace", timeout=60, stdin=subprocess.DEVNULL,
                                 env=dict(os.environ, AETHER_EXPERIMENT_LOG=log))
            rc, err = run.returncode, run.stderr
        except subprocess.TimeoutExpired:
            rc, err = -1, ""
        rows = pathlib.Path(log).read_text().splitlines() if os.path.exists(log) else []
    return {"compiles": rc == 0, "naive": any(r.startswith("param-lost\t") for r in rows),
            "warnings": [l for l in err.splitlines() if "[PARAM-001]" in l]}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--aether-bin", required=True)
    ap.add_argument("--reports", action="append", default=[], help="another report glob (repeatable)")
    ap.add_argument("--json", type=pathlib.Path)
    ap.add_argument("--workers", type=int, default=6)
    args = ap.parse_args()
    aether = os.path.abspath(args.aether_bin)
    records = collect(args.reports)
    status = collections.defaultdict(set)
    first: dict[str, dict] = {}
    for r in records:
        status[r["sha"]].add(r["status"])
        first.setdefault(r["sha"], r)
    with concurrent.futures.ThreadPoolExecutor(args.workers) as pool:
        results = dict(zip(first, pool.map(lambda sha: compile_one(aether, first[sha]), first)))
    table = collections.Counter()
    hits = []
    for sha, res in results.items():
        st = status[sha]
        kind = "correct" if st == {"correct"} else "failing" if st == {"failing"} else "mixed"
        if not res["compiles"]:
            continue
        table[(kind, "compiles")] += 1
        table[(kind, "naive")] += res["naive"]
        table[(kind, "PARAM-001")] += bool(res["warnings"])
        if res["warnings"]:
            r = first[sha]
            hits.append({"status": kind, "group": r["group"], "id": r["id"], "task": r["task"], "sha256": sha,
                         "warnings": [w.split("warning: ", 1)[-1][:160] for w in res["warnings"]]})
    print(f"{len(records)} records, {len(first)} distinct programs")
    print(f"{'status':8} {'compile':>8} {'naive':>6} {'PARAM-001':>10}")
    for kind in ("correct", "failing", "mixed"):
        print(f"{kind:8} {table[(kind, 'compiles')]:8} {table[(kind, 'naive')]:6} {table[(kind, 'PARAM-001')]:10}")
    for h in hits:
        print(f"  {h['status']}: {h['group']} {h['id']} {h['task'] or ''}")
    if args.json:
        args.json.write_text(json.dumps({"table": {f"{k[0]}/{k[1]}": v for k, v in sorted(table.items())},
                                         "hits": hits}, indent=2) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
