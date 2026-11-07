#!/usr/bin/env python3
"""The reading fixed in README.md: the two seeds' TIES-merged weights at int8 against each parent, on the 1,776 frozen tasks. Run from the repo root, after the filter stage.

All answered with no guide and no repair, on the 1.0 grader. This reads pass or fail and each failure's class, never a model's program.
"""

from __future__ import annotations

import collections
import gzip
import json
import math
import pathlib
import sys

sys.path.insert(0, "tools")
import aether_distill as distill  # noqa: E402

HERE = pathlib.Path(__file__).resolve().parent
SLICES = ["f1-20261103", "f2-20261103", "f3-20261103"]
AREAS = {"core": distill.CATEGORIES, "JSON documents and data files": distill.TOON_FILE_CATEGORIES, "keyed data": distill.MAP_CATEGORIES}


def tasks(path: pathlib.Path) -> list[dict]:
    return json.loads(path.read_text(encoding="utf-8"))["tasks"]


def failures(path: pathlib.Path) -> dict:
    return {t["id"]: t["filter"]["first_attempt"] for t in tasks(path)}


def area(category: str) -> str:
    return next(name for name, categories in AREAS.items() if category in categories)


def short(category: str) -> str:
    return category.split(" (")[0].split(";")[0][:48]


def sign_test(a: int, b: int) -> float:
    n, k = a + b, min(a, b)
    return min(1.0, 2 * sum(math.comb(n, i) for i in range(k + 1)) / 2 ** n) if n else 1.0


def results(folder: pathlib.Path) -> dict:
    path = folder / "report__filter.json.gz"
    if not path.exists():
        path = folder / "filter.json"
    with (gzip.open(path, "rt") if path.suffix == ".gz" else open(path)) as handle:
        report = json.load(handle)
    return {r["task_id"]: r for r in report["destinations"][0]["variants"][0]["results"]}


def audit(name: str, run: dict, reference: dict) -> None:
    attempts = [a for r in run.values() for a in r["attempts"]]
    same = sum(run[k]["attempts"][0].get("prompt_sha256") == reference[k]["attempts"][0].get("prompt_sha256") for k in run)
    print(f"  {name:12s} {len(run)} tasks, infra {sum(bool(r.get('infra_failed')) for r in run.values())}, "
          f"empty {sum(not (a.get('source_code') or '').strip() for a in attempts)}, length caps {sum(a.get('finish_reason') == 'length' for a in attempts)}, "
          f"grader timeouts {sum(bool((a.get('run') or {}).get('timed_out')) for a in attempts)}, prompts matching the published seed {same}")


def main() -> None:
    published, everything, published_reports = {}, {}, {}
    for name in SLICES:
        folder = HERE.parent / name
        published |= failures(folder / "tasks_failed.json")
        everything |= {t["id"]: t for t in tasks(folder / "tasks_frozen.json")}
        published_reports |= results(folder)
    second = failures(HERE.parent / "fs42-20261105" / "tasks_failed.json")
    e3 = failures(HERE.parent / "fe3-20261104" / "tasks_failed.json")
    average = failures(HERE / "tasks_failed.json")
    measured = set(average) | {t["id"] for t in tasks(HERE / "tasks_passed.json")}
    if measured != set(everything):
        raise SystemExit("STOP: the task sets do not line up")

    print("audit")
    audit("published", published_reports, published_reports)
    audit("seed 42", results(HERE.parent / "fs42-20261105"), published_reports)
    audit("average", results(HERE), published_reports)

    total = len(measured)
    runs = {"published": published, "seed 42": second, "average": average, "e3": e3}
    print(f"\n{total} tasks, first attempt")
    for name, failed in runs.items():
        classes = collections.Counter(failed.values())
        print(f"  {name:10s} failed {len(failed):4d} ({100 * len(failed) / total:.1f}%), coded {classes['coded']}, return code {classes['rc']}, wrong output {classes['wrong']}")

    alone, parent_alone = len(set(average) - set(second)), len(set(second) - set(average))
    p = sign_test(alone, parent_alone)
    print(f"\nTHE READING: the average fails alone {alone}, seed 42 fails alone {parent_alone}, sign test p={p:.2g}")
    if p >= 0.05:
        print("  NO BETTER THAN SEED 42")
    else:
        print("  BETTER THAN BOTH PARENTS" if alone < parent_alone else "  WORSE THAN SEED 42")

    print("\nreported, not deciding")
    for name in ("published", "e3"):
        one, other = len(set(average) - set(runs[name])), len(set(runs[name]) - set(average))
        print(f"  against {name}: the average fails alone {one}, {name} fails alone {other}, p={sign_test(one, other):.2g}")
    disagree = set(published) ^ set(second)
    both = set(published) & set(second)
    neither = measured - set(published) - set(second)
    print(f"  tasks the parents disagree on: {len(disagree)}; the average passes {len(disagree - set(average))} ({100 * len(disagree - set(average)) / len(disagree):.0f}%)")
    print(f"  tasks both parents fail: {len(both)}; the average rescues {len(both - set(average))}")
    print(f"  tasks both parents pass: {len(neither)}; the average breaks {len(neither & set(average))}")
    print(f"  on tasks all three fail, the same class as seed 42 on {sum(average[i] == second[i] for i in both & set(average))} of {len(both & set(average))}")

    print("\nby area: tasks, published, seed 42, average, e3")
    for name in AREAS:
        ids = {i for i, t in everything.items() if area(t["category"]) == name}
        cells = "  ".join(f"{len(ids & set(runs[r])):3d} ({100 * len(ids & set(runs[r])) / len(ids):2.0f}%)" for r in runs)
        print(f"  {name:30s} {len(ids):4d}  {cells}")

    print("\nby category, most failures first: tasks, published, seed 42, average, e3")
    counts = collections.Counter(t["category"] for t in everything.values())
    rows = []
    for category, n in counts.items():
        ids = {i for i, t in everything.items() if t["category"] == category}
        rows.append((n, [len(ids & set(runs[r])) for r in runs], category))
    for n, cells, category in sorted(rows, key=lambda r: -sum(r[1][:3])):
        print(f"  {n:4d}  " + "  ".join(f"{c:3d}" for c in cells) + f"  {short(category)}")


if __name__ == "__main__":
    main()
