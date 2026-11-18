#!/usr/bin/env python3
"""The reading fixed in README.md: three epochs minus two on 2,802 fresh tasks, first attempts at int8. Run from the repo root.

Reads pass or fail and each failure's class. Prints no task id or program.
"""

from __future__ import annotations

import gzip
import json
import math
import pathlib
import random
import sys

sys.path.insert(0, "tools")
import aether_distill as distill  # noqa: E402

HERE = pathlib.Path(__file__).resolve().parent
RUNS = {"three epochs (e3)": "e3", "two epochs (e2)": "e2", "e3 at epoch 2": "e3_ep2"}
POOLS = {"f1 to f3 (1,776)": ("f1-20261103", "f2-20261103", "f3-20261103"), "fv3 (1,026)": ("fv3-20261105",)}
RESAMPLES = 10_000


def load(tag: str) -> dict:
    path = HERE / f"report__filter_{tag}.json.gz"
    if not path.exists():
        path = HERE / f"filter_{tag}.json"
    with (gzip.open(path, "rt") if path.suffix == ".gz" else open(path)) as handle:
        report = json.load(handle)
    return {r["task_id"]: r["attempts"][0] for r in report["destinations"][0]["variants"][0]["results"]}


def sign_test(a: int, b: int) -> float:
    n, k = a + b, min(a, b)
    return min(1.0, 2 * sum(math.comb(n, i) for i in range(k + 1)) / 2 ** n) if n else 1.0


def interval(diffs: list[float]) -> tuple[float, float, float]:
    rng = random.Random(42)
    n = len(diffs)
    means = sorted(sum(rng.choices(diffs, k=n)) / n for _ in range(RESAMPLES))
    return sum(diffs) / n, means[int(0.025 * RESAMPLES)], means[int(0.975 * RESAMPLES) - 1]


def reading(low: float, high: float) -> str:
    if low > 0:
        return "the third pass helps: three epochs stays"
    if high < 0:
        return "the third pass hurts: two epochs"
    return f"no readable difference: two epochs; a gain above {100 * high:.2f} points is ruled out"


def main() -> None:
    pools = {name: [t["id"] for b in batches
                    for t in json.loads((HERE.parent / b / "tasks_frozen.json").read_text(encoding="utf-8"))["tasks"]]
             for name, batches in POOLS.items()}
    keys = [k for ids in pools.values() for k in ids]
    if len(keys) != 2802 or len(set(keys)) != 2802:
        raise SystemExit(f"STOP: expected 2,802 unique tasks, found {len(keys)} ({len(set(keys))} unique)")
    runs = {name: load(tag) for name, tag in RUNS.items()}
    for name, run in runs.items():
        if set(run) != set(keys):
            raise SystemExit(f"STOP: {name} does not cover the 2,802 tasks ({len(run)} results)")
    failure = {name: {k: distill.filter_failure_class(run[k].get("run") or {}) for k in keys} for name, run in runs.items()}
    passed = {name: {k: failure[name][k] is None for k in keys} for name in runs}

    print("first attempt, 2,802 tasks: passed, failed, coded / return code / wrong output")
    for name, run in runs.items():
        classes = [c for c in failure[name].values() if c]
        empty = sum(1 for k in keys if not (run[k].get("source_code") or "").strip())
        capped = sum(1 for k in keys if run[k].get("finish_reason") == "length")
        print(f"  {name:20s} {len(keys) - len(classes):5d} {len(classes):5d}  "
              f"{classes.count('coded')} / {classes.count('rc')} / {classes.count('wrong')}"
              f"   empty answers {empty}, length caps {capped}")

    def compare(a: str, b: str, ids: list[str]) -> str:
        mean, low, high = interval([passed[a][k] - passed[b][k] for k in ids])
        gained = sum(1 for k in ids if passed[a][k] and not passed[b][k])
        lost = sum(1 for k in ids if passed[b][k] and not passed[a][k])
        return (f"{100 * mean:+.2f} points [{100 * low:+.2f}, {100 * high:+.2f}], "
                f"gained {gained}, lost {lost}, sign test p={sign_test(gained, lost):.3g}"), low, high

    three, two, mid = RUNS
    text, low, high = compare(three, two, keys)
    print(f"\nTHE READING: {three} minus {two}, all 2,802 tasks\n  {text}\n  -> {reading(low, high)}")
    print("\nreported, not deciding")
    for name, ids in pools.items():
        print(f"  {three} minus {two}, {name}: {compare(three, two, ids)[0]}")
    print(f"  {three} minus {mid}, all: {compare(three, mid, keys)[0]}")
    print(f"  {two} minus {mid}, all: {compare(two, mid, keys)[0]}")


if __name__ == "__main__":
    main()
