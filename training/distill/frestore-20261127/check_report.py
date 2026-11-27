#!/usr/bin/env python3
"""Fail a frestore-20261127 pass whose report is incomplete or whose generations are empty or reasoning traces.

    check_report.py REPORT.json LEAK_PREFIXES_JSON

Reads the Aether results, or the Python results when the report has no Aether cases. A pass is complete only with
all 1,026 tasks measured and no infrastructure failure. Prints the counts either way.
"""
import json
import sys

TASKS = 1026
path, prefixes = sys.argv[1], tuple(p.lower() for p in json.loads(sys.argv[2]))
try:
    report = json.load(open(path))
except (OSError, ValueError) as exc:
    print(f"check: cannot read {path}: {exc}")
    sys.exit(1)
variant = report["destinations"][0]["variants"][0]
kind = "aether" if variant.get("results") else "python"
results = variant["results"] if kind == "aether" else variant.get("python_baseline_results") or []
summary = variant["summary"] if kind == "aether" else variant.get("python_baseline_summary") or {}


def first_source(result):
    attempts = result.get("attempts") or []
    return (attempts[0].get("source_code") if attempts else None) or result.get("source_code") or ""


empty = sum(1 for r in results if not first_source(r).strip())
leak = sum(1 for r in results if prefixes and first_source(r).lstrip().lower().startswith(prefixes))
infra = summary.get("infra_failed", 0)
tasks = len({r.get("task_id") for r in results})
print(f"check {path.rsplit('/', 1)[-1]} ({kind}): cases={len(results)} tasks={tasks} FA={summary.get('first_attempt_exact')} "
      f"empty={empty} leak={leak} infra={infra}")
bad = leak > 0 or empty == len(results) or infra > 0 or report.get("infra_failed") or tasks != TASKS or len(results) != TASKS
sys.exit(1 if bad else 0)
