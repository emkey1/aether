#!/usr/bin/env python3
"""Does e3 fail the tasks 8-bit Gemma failed? Run from the repo root, after the filter stage of this batch.

Both models answered the 1,776 frozen tasks of f1, f2 and f3-20261103 with no guide and no repair, on the 1.0 grader. This reads pass or fail and
each failure's class, never a model's program. The pairs named here are the approved ones of those three batches.
"""

from __future__ import annotations

import collections
import json
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


def main() -> None:
    gemma, everything, paired = {}, {}, set()
    for name in SLICES:
        folder = HERE.parent / name
        gemma |= {t["id"]: t["filter"]["first_attempt"] for t in tasks(folder / "tasks_failed.json")}
        everything |= {t["id"]: t for t in tasks(folder / "tasks_frozen.json")}
        paired |= {p["id"] for p in json.loads((folder / "pairs.json").read_text(encoding="utf-8"))["pairs"]}
    e3_failed, e3_passed = tasks(HERE / "tasks_failed.json"), tasks(HERE / "tasks_passed.json")
    e3 = {t["id"]: t["filter"]["first_attempt"] for t in e3_failed}
    measured = {t["id"] for t in e3_failed} | {t["id"] for t in e3_passed}
    if measured != set(everything) or not paired <= set(gemma):
        raise SystemExit("STOP: the task sets do not line up")

    total = len(measured)
    both = set(gemma) & set(e3)
    expected = len(gemma) * len(e3) / total
    print(f"{total} tasks; Gemma failed {len(gemma)} ({100 * len(gemma) / total:.0f}%), e3 failed {len(e3)} ({100 * len(e3) / total:.0f}%)")
    for name, failures in (("Gemma", gemma), ("e3", e3)):
        classes = collections.Counter(failures.values())
        print(f"  {name:6s} coded {classes['coded']}, return code {classes['rc']}, wrong output {classes['wrong']}")
    print(f"\nboth failed {len(both)}; only Gemma {len(gemma) - len(both)}; only e3 {len(e3) - len(both)}; "
          f"neither {total - len(set(gemma) | set(e3))}")
    print(f"both failed if unrelated, about {expected:.0f}, so {len(both) / expected:.1f} times that")
    print(f"of Gemma's failures e3 also failed {100 * len(both) / len(gemma):.0f}%; of e3's failures Gemma also failed {100 * len(both) / len(e3):.0f}%")
    agree = sum(gemma[i] == e3[i] for i in both)
    print(f"on tasks both failed, the same class {agree} of {len(both)}")

    print(f"\nthe {len(paired)} pairs: e3 failed {len(paired & set(e3))} of those tasks ({100 * len(paired & set(e3)) / len(paired):.0f}%)")
    covered = set(e3) & paired
    print(f"e3's {len(e3)} failures: {len(covered)} already have a pair; {len(both - paired)} Gemma also failed and the solvers missed; "
          f"{len(set(e3) - set(gemma))} Gemma passed, never solved")

    print("\nby area: tasks, Gemma failed, e3 failed, both")
    for name in AREAS:
        ids = {i for i, t in everything.items() if area(t["category"]) == name}
        print(f"  {name:30s} {len(ids):4d}  {len(ids & set(gemma)):3d} ({100 * len(ids & set(gemma)) / len(ids):2.0f}%)  "
              f"{len(ids & set(e3)):3d} ({100 * len(ids & set(e3)) / len(ids):2.0f}%)  {len(ids & both):3d}")

    print("\nby category, e3's failures first: tasks, Gemma failed, e3 failed")
    counts = collections.Counter(t["category"] for t in everything.values())
    rows = [(sum(everything[i]["category"] == c for i in e3), sum(everything[i]["category"] == c for i in gemma), n, c) for c, n in counts.items()]
    for e3_count, gemma_count, n, category in sorted(rows, key=lambda r: (-r[0], -r[1])):
        print(f"  {n:4d}  {gemma_count:3d}  {e3_count:3d}  {short(category)}")


if __name__ == "__main__":
    main()
