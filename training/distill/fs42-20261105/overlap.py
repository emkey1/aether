#!/usr/bin/env python3
"""The reading fixed in README.md: seed 42 of Gemma at int8 against the published seed, on the 1,776 frozen tasks. Run from the repo root, after the filter stage.

Both seeds answered with no guide and no repair, on the 1.0 grader. This reads pass or fail and each failure's class, never a model's program.
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
    published, everything, paired, published_reports = {}, {}, set(), {}
    for name in SLICES:
        folder = HERE.parent / name
        published |= {t["id"]: t["filter"]["first_attempt"] for t in tasks(folder / "tasks_failed.json")}
        everything |= {t["id"]: t for t in tasks(folder / "tasks_frozen.json")}
        paired |= {p["id"] for p in json.loads((folder / "pairs.json").read_text(encoding="utf-8"))["pairs"]}
        published_reports |= results(folder)
    failed, passed = tasks(HERE / "tasks_failed.json"), tasks(HERE / "tasks_passed.json")
    second = {t["id"]: t["filter"]["first_attempt"] for t in failed}
    measured = {t["id"] for t in failed} | {t["id"] for t in passed}
    e3 = {t["id"]: t["filter"]["first_attempt"] for t in tasks(HERE.parent / "fe3-20261104" / "tasks_failed.json")}
    if measured != set(everything) or not paired <= set(published):
        raise SystemExit("STOP: the task sets do not line up")

    print("audit")
    audit("published", published_reports, published_reports)
    audit("seed 42", results(HERE), published_reports)

    total = len(measured)
    both = set(published) & set(second)
    only_published, only_second = len(published) - len(both), len(second) - len(both)
    print(f"\n{total} tasks; the published seed failed {len(published)} ({100 * len(published) / total:.1f}%), seed 42 failed {len(second)} ({100 * len(second) / total:.1f}%)")
    for name, failures in (("published", published), ("seed 42", second)):
        classes = collections.Counter(failures.values())
        print(f"  {name:10s} coded {classes['coded']}, return code {classes['rc']}, wrong output {classes['wrong']}")
    print(f"both failed {len(both)}; only the published seed {only_published}; only seed 42 {only_second}; neither {total - len(set(published) | set(second))}")

    p = sign_test(only_published, only_second)
    stronger = "seed 42" if only_published > only_second else "the published seed"
    print(f"\nREADING 1: seed 42 fails alone {only_second}, the published seed fails alone {only_published}, sign test p={p:.2g}")
    print(f"  {stronger + ' is stronger on fresh first attempts' if p < 0.05 else 'this cannot separate the two seeds on fresh first attempts'}")

    passes = len(paired - set(second))
    share = passes / len(paired)
    verdict = "supports it" if share >= 1 / 3 else "against it" if share < 1 / 5 else "unclear"
    print(f"\nREADING 2: of the {len(paired)} pairs' tasks seed 42 passes {passes} ({100 * share:.0f}%): {verdict}")

    print("\nreported, not deciding")
    expected = len(published) * len(second) / total
    print(f"both failed if unrelated, about {expected:.0f}, so {len(both) / expected:.1f} times that")
    print(f"of the published seed's failures seed 42 also fails {100 * len(both) / len(published):.0f}%; e3 at mxfp4 fails {100 * len(set(published) & set(e3)) / len(published):.0f}%")
    print(f"on tasks both seeds failed, the same class {sum(published[i] == second[i] for i in both)} of {len(both)}")
    print(f"fail at both seeds {len(both)}: {len(both & paired)} have a pair, {len(both - paired)} do not; also failed by e3 {len(both & set(e3))}")
    print(f"e3's {len(e3)} failures: {len(set(e3) & both)} fail at both Gemma seeds, {len(set(e3) & (set(published) ^ set(second)))} at one, {len(set(e3) - set(published) - set(second))} at neither")

    print("\nthe published seed's failures by class: failures, also failed by seed 42")
    for name in ("coded", "rc", "wrong"):
        ids = {i for i, c in published.items() if c == name}
        print(f"  {name:6s} {len(ids):4d}  {len(ids & both):4d} ({100 * len(ids & both) / max(1, len(ids)):.0f}%)")

    print("\nby area: tasks, published failed, seed 42 failed, both")
    for name in AREAS:
        ids = {i for i, t in everything.items() if area(t["category"]) == name}
        print(f"  {name:30s} {len(ids):4d}  {len(ids & set(published)):3d} ({100 * len(ids & set(published)) / len(ids):2.0f}%)  "
              f"{len(ids & set(second)):3d} ({100 * len(ids & set(second)) / len(ids):2.0f}%)  {len(ids & both):3d}")

    print("\nby category, most failures first: tasks, published failed, seed 42 failed, both")
    counts = collections.Counter(t["category"] for t in everything.values())
    rows = []
    for category, n in counts.items():
        ids = {i for i, t in everything.items() if t["category"] == category}
        rows.append((len(ids & set(published)) + len(ids & set(second)), n, len(ids & set(published)), len(ids & set(second)), len(ids & both), category))
    for _, n, a, b, c, category in sorted(rows, key=lambda r: -r[0]):
        print(f"  {n:4d}  {a:3d}  {b:3d}  {c:3d}  {short(category)}")


if __name__ == "__main__":
    main()
