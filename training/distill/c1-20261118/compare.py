#!/usr/bin/env python3
"""The reading fixed in README.md: targeted minus control on fv3's 1,026 tasks, two seeds each. Run from the repo root.

Reads ../fv3-20261105/report__filter_{v2,v2_s42,v3,v3_s42}.json.gz and, beside this file,
report__filter_{c1,c1_s42}.json.gz (or filter_<tag>.json). Prints no task id or program.
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
FV3 = HERE.parent / "fv3-20261105"
ARMS = {"baseline": ((FV3, "v2"), (FV3, "v2_s42")), "targeted": ((FV3, "v3"), (FV3, "v3_s42")),
        "control": ((HERE, "c1"), (HERE, "c1_s42"))}
TASKS = 1026
RESAMPLES = 10_000


def load(where: pathlib.Path, tag: str) -> dict:
    path = where / f"report__filter_{tag}.json.gz"
    if not path.exists():
        path = where / f"filter_{tag}.json"
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
        return "targeting helps beyond the same amount of data"
    if high < 0:
        return "passed-task data helps more: stop filtering on failures"
    return f"not shown: the fv3 gain is explained by more data at this resolution (a targeting effect above {100 * high:.2f} points is ruled out)"


def main() -> None:
    keys = [t["id"] for t in json.loads((FV3 / "tasks_frozen.json").read_text(encoding="utf-8"))["tasks"]]
    if len(keys) != TASKS or len(set(keys)) != TASKS:
        raise SystemExit(f"STOP: expected {TASKS} unique fv3 tasks, found {len(keys)}")
    runs = {tag: load(where, tag) for members in ARMS.values() for where, tag in members}
    for tag, results in runs.items():
        if set(results) != set(keys):
            raise SystemExit(f"STOP: {tag} does not cover the {TASKS} tasks ({len(results)} results)")
    failure = {t: {k: distill.filter_failure_class(r[k].get("run") or {}) for k in keys} for t, r in runs.items()}
    passed = {t: {k: failure[t][k] is None for k in keys} for t in runs}

    print(f"first attempt, {TASKS} tasks: passed, failed, coded / return code / wrong output")
    for tag in runs:
        classes = [c for c in failure[tag].values() if c]
        print(f"  {tag:8s} {TASKS - len(classes):5d} {len(classes):5d}  "
              f"{classes.count('coded')} / {classes.count('rc')} / {classes.count('wrong')}")

    arm = {name: {k: sum(passed[t][k] for _, t in members) / len(members) for k in keys} for name, members in ARMS.items()}

    def arms(a: str, b: str) -> tuple[str, float, float]:
        mean, low, high = interval([arm[a][k] - arm[b][k] for k in keys])
        return f"{100 * mean:+.2f} points [{100 * low:+.2f}, {100 * high:+.2f}]", low, high

    def runs_diff(a: str, b: str) -> str:
        mean, low, high = interval([passed[a][k] - passed[b][k] for k in keys])
        gained = sum(1 for k in keys if passed[a][k] and not passed[b][k])
        lost = sum(1 for k in keys if passed[b][k] and not passed[a][k])
        return (f"{100 * mean:+.2f} points [{100 * low:+.2f}, {100 * high:+.2f}], gained {gained}, lost {lost}, "
                f"sign test p={sign_test(gained, lost):.3g}")

    text, low, high = arms("targeted", "control")
    print(f"\nTHE READING: targeted minus control, two seeds each, {TASKS} tasks\n  {text}\n  -> {reading(low, high)}")
    print("\nreported, not deciding")
    print(f"  control minus baseline: {arms('control', 'baseline')[0]}")
    print(f"  targeted minus baseline: {arms('targeted', 'baseline')[0]}")
    for (_, t), (_, c) in zip(ARMS["targeted"], ARMS["control"]):
        print(f"  {t} minus {c}: {runs_diff(t, c)}")
    for name, ((_, a), (_, b)) in ARMS.items():
        print(f"  seed gap, {name}: {runs_diff(a, b)}")


if __name__ == "__main__":
    main()
