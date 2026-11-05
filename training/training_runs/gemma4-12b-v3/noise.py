#!/usr/bin/env python3
"""Seed noise and pair splits over the four runs, reported beside compare.py and not part of the fixed reading. Run from the repo root.

Reads pass or fail and the failure's class. Program text is only compared for equality, and nothing here prints a task id or a program.
"""

from __future__ import annotations

import math
import pathlib
import statistics
import sys

sys.path.insert(0, str(pathlib.Path(__file__).parent))
from compare import HELD, MAIN, failure_class, keys_of, load, passed  # noqa: E402

TAGS = {"v2 3407": "gemma4-12b-v2", "v2 42": "gemma4-12b-v2-s42", "v3 3407": "gemma4-12b-v3", "v3 42": "gemma4-12b-v3-s42"}
PAIRS = (("v2 42", "v2 3407"), ("v3 42", "v3 3407"), ("v3 3407", "v2 3407"), ("v3 42", "v2 42"))


def sign_test(a: int, b: int) -> float:
    n, k = a + b, min(a, b)
    return min(1.0, 2 * sum(math.comb(n, i) for i in range(k + 1)) / 2 ** n) if n else 1.0


def split(left: dict, right: dict, keys: list, index: int) -> str:
    a = sum(passed(left[k]["attempts"][index]) and not passed(right[k]["attempts"][index]) for k in keys)
    b = sum(passed(right[k]["attempts"][index]) and not passed(left[k]["attempts"][index]) for k in keys)
    return f"{a:2d} to {b:2d} (p={sign_test(a, b):.3f})"


def main() -> None:
    runs = {name: load(tag) for name, tag in TAGS.items()}
    reference = runs["v2 3407"]
    main_keys, held = keys_of(reference, MAIN), keys_of(reference, HELD)
    groups = (("main", main_keys), ("held-out", held), ("all", main_keys + held))

    print("tasks only one run passes, first attempt then after repair")
    for x, y in PAIRS:
        for name, keys in groups:
            print(f"  {x:8s} against {y:8s} {name:9s} {split(runs[x], runs[y], keys, 0)}   {split(runs[x], runs[y], keys, -1)}")

    print("\nheld-out, targeted against baseline at each seed: how the same task moved at the two seeds")
    for index, label in ((0, "first"), (-1, "repaired")):
        both_up = both_down = one_up = one_down = opposite = 0
        for k in held:
            d1 = passed(runs["v3 3407"][k]["attempts"][index]) - passed(runs["v2 3407"][k]["attempts"][index])
            d2 = passed(runs["v3 42"][k]["attempts"][index]) - passed(runs["v2 42"][k]["attempts"][index])
            both_up += d1 > 0 and d2 > 0
            both_down += d1 < 0 and d2 < 0
            opposite += d1 * d2 < 0
            one_up += d1 * d2 == 0 and d1 + d2 > 0
            one_down += d1 * d2 == 0 and d1 + d2 < 0
        print(f"  {label:9s} gained at both {both_up}, lost at both {both_down}, gained at one {one_up}, lost at one {one_down}, opposite {opposite}")

    print("\nfailures repair fixed, per run")
    for name, run in runs.items():
        cells = []
        for group, keys in groups:
            first = sum(not passed(run[k]["attempts"][0]) for k in keys)
            cells.append(f"{group} {first - sum(not passed(run[k]['attempts'][-1]) for k in keys)} of {first}")
        print(f"  {name:8s} " + ", ".join(cells))

    print("\nall 126 tasks: first (repaired), then coded / return code / wrong output at each")
    for name, run in runs.items():
        cells = []
        for index in (0, -1):
            classes = {"coded": 0, "rc": 0, "wrong": 0}
            for k in reference:
                if not passed(run[k]["attempts"][index]):
                    classes[failure_class(run[k]["attempts"][index])] += 1
            cells.append(f"{classes['coded']} / {classes['rc']} / {classes['wrong']}")
        first, repaired = (sum(passed(run[k]["attempts"][i]) for k in reference) for i in (0, -1))
        print(f"  {name:8s} {first} ({repaired})   {cells[0]:12s} {cells[1]}")

    print("\nfirst programs byte-identical across a pair, and each run's median completion")
    for x, y in PAIRS:
        same = sum(runs[x][k]["attempts"][0].get("source_code") == runs[y][k]["attempts"][0].get("source_code") for k in reference)
        print(f"  {x:8s} against {y:8s} {same} of {len(reference)}")
    for name, run in runs.items():
        tokens = [run[k]["attempts"][0].get("completion_tokens") or (run[k]["attempts"][0].get("usage") or {}).get("completion_tokens") for k in reference]
        print(f"  {name:8s} {statistics.median(t for t in tokens if t)} tokens")


if __name__ == "__main__":
    main()
