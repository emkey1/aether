#!/usr/bin/env python3
"""The pilot's numbers: how often the fine-tune fails fresh tasks, by class, what the solvers recover, and each stage's time.

    python3 training/distill/f1-20261103/pilot_report.py [--batch DIR] [--target PAIRS]

--batch picks another slice, such as f2-20261103. Reads the batch's raw or packed reports. The fine-tune's programs are only read for pass or fail here too.
"""

from __future__ import annotations

import argparse
import collections
import gzip
import json
import pathlib

HERE = pathlib.Path(__file__).resolve().parent


def load(name: str) -> dict | None:
    raw, packed = HERE / f"{name}.json", HERE / f"report__{name}.json.gz"
    if raw.exists():
        return json.loads(raw.read_text(encoding="utf-8"))
    if packed.exists():
        with gzip.open(packed, "rt", encoding="utf-8") as handle:
            return json.load(handle)
    return None


def results(report: dict) -> list[dict]:
    return report["destinations"][0]["variants"][0]["results"]


def solved(report: dict | None) -> tuple[set, set]:
    """Tasks whose first program agreed with the Python, and tasks where any program did."""
    first, any_attempt = set(), set()
    for result in results(report) if report else []:
        attempts = result.get("attempts") or []
        exact = [bool((a.get("run") or {}).get("exact_stdout_match")) for a in attempts]
        if exact and exact[0]:
            first.add(result["task_id"])
        if any(exact):
            any_attempt.add(result["task_id"])
    return first, any_attempt


def minutes(report: dict | None) -> str:
    if not report or not report.get("updated_at_unix"):
        return "-"
    return f"{(report['updated_at_unix'] - report['created_at_unix']) / 60:.0f} min"


def short(category: str) -> str:
    return category.split(" (")[0].split(";")[0][:48]


def main() -> None:
    global HERE
    ap = argparse.ArgumentParser()
    ap.add_argument("--batch", type=pathlib.Path, default=HERE)
    ap.add_argument("--target", type=int, default=286, help="pairs to scale the batch to (d5 added 286)")
    args = ap.parse_args()
    HERE = args.batch
    doc = json.loads((HERE / "tasks.json").read_text(encoding="utf-8"))
    frozen = json.loads((HERE / "tasks_frozen.json").read_text(encoding="utf-8"))["tasks"]
    failed = json.loads((HERE / "tasks_failed.json").read_text(encoding="utf-8"))["tasks"]
    passed = json.loads((HERE / "tasks_passed.json").read_text(encoding="utf-8"))["tasks"]
    base, mimo = load("aether_base"), load("aether_mimo")

    requests = doc.get("requests", [])
    writing = sum(r["t1"] - r["t0"] for r in requests) / 60
    print(f"tasks: {len(doc['tasks'])} written in {len(requests)} requests, {writing:.0f} min of requests")
    print(f"frozen: {len(frozen)}; python {minutes(load('python'))}")
    measured = len(failed) + len(passed)
    classes = collections.Counter(t["filter"]["first_attempt"] for t in failed)
    print(f"filter: {load('filter')['destinations'][0]['model']}, failed {len(failed)} of {measured} "
          f"({100 * len(failed) / measured:.0f}%): {dict(classes)}; {minutes(load('filter'))}")

    print("\nby category: failed of tasks")
    all_tasks = failed + passed
    for category, count in collections.Counter(t["category"] for t in all_tasks).most_common():
        lost = sum(t["category"] == category for t in failed)
        print(f"  {lost:2d} of {count:2d}  {short(category)}")

    base_first, base_any = solved(base)
    mimo_first, mimo_any = solved(mimo)
    failed_ids = {t["id"] for t in failed}
    recovered = (base_any | mimo_any) & failed_ids
    print(f"\nsolve: guided 27B {len(base_any)} of {len(failed)} ({len(base_first)} first attempt), {minutes(base)}")
    print(f"       MiMo {len(mimo_any)} of {len(failed) - len(base_any)} left ({len(mimo_first)} first attempt), {minutes(mimo)}")
    print(f"       recovered {len(recovered)} of {len(failed)} ({100 * len(recovered) / max(len(failed), 1):.0f}%)")
    print("  by class, recovered of failed: " + ", ".join(
        f"{name} {sum(t['id'] in recovered for t in failed if t['filter']['first_attempt'] == name)} of {count}"
        for name, count in classes.items()))

    pairs_per_task = len(recovered) / max(len(frozen), 1)
    if pairs_per_task:
        print(f"\nscale: {pairs_per_task:.2f} pairs per frozen task, so {args.target} pairs needs about "
              f"{args.target / pairs_per_task:.0f} frozen tasks")


if __name__ == "__main__":
    main()
