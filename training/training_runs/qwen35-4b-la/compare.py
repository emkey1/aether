#!/usr/bin/env python3
"""The reading fixed in README.md: la minus base, two seeds each, on 2,802 fresh tasks. Run from the repo root.

A gain here makes the names a candidate only; the three-seed reading that can change a target is fixed in README.md
(amendment of 2026-11-25) and gets its own script before its runs are scored.

Reads report__filter_<run>.json.gz (or filter_<run>.json) beside this file, one per run. Prints no task id or program.
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
DISTILL = HERE.parents[1] / "distill"
ARMS = {"base": ("qwen35-4b-la-base-s3407", "qwen35-4b-la-base-s42"),
        "la": ("qwen35-4b-la-la-s3407", "qwen35-4b-la-la-s42")}
BATCHES = ("f1-20261103", "f2-20261103", "f3-20261103", "fv3-20261105")
TASKS = 2802
RESAMPLES = 10_000


def load(run: str) -> dict:
    path = HERE / f"report__filter_{run}.json.gz"
    if not path.exists():
        path = HERE / f"filter_{run}.json"
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


def seed_check(mean: float, pair_totals: list[int], seed_only: tuple[float, float, float]) -> tuple[bool, str]:
    """The seed check of 2026-11-24 (README.md): every la run beats every base run, and the
    difference is larger than either end of the seed-only contrast's interval."""
    ordered = all(total > 0 for total in pair_totals)
    reach = max(abs(seed_only[1]), abs(seed_only[2]))
    why = []
    if not ordered:
        why.append("not every la run beats every base run")
    if not mean > reach:
        why.append(f"the difference is within the seed-only contrast's reach ({100 * reach:.2f} points)")
    return not why, "; ".join(why)


def reading(low: float, high: float, seed_gap: float, clear: bool, why: str) -> str:
    if low > 0 and (low + high) / 2 > seed_gap and clear:
        return ("a gain: the names are a candidate and no target changes yet (README.md, amendment of 2026-11-25); "
                "next, a third seed per arm and all six runs answered in one session")
    if low > 0:
        reasons = [why] if why else []
        if not (low + high) / 2 > seed_gap:
            reasons.insert(0, f"base seeds differ by {100 * seed_gap:.2f} points")
        return f"not separable from seed noise ({'; '.join(reasons)}): no change"
    if high < 0:
        return "a loss: no change"
    return f"no readable difference: no change; a gain above {100 * high:.2f} points is ruled out"


SESSION_VERSION = HERE / "session_server_version.txt"


def main() -> None:
    if not SESSION_VERSION.is_file() or not SESSION_VERSION.read_text(encoding="utf-8").strip():
        raise SystemExit(f"STOP: no {SESSION_VERSION.name}: the four passes must record their one server version")
    print(f"session server version: {SESSION_VERSION.read_text(encoding='utf-8').strip()}")
    keys = [t["id"] for b in BATCHES
            for t in json.loads((DISTILL / b / "tasks_frozen.json").read_text(encoding="utf-8"))["tasks"]]
    if len(keys) != TASKS or len(set(keys)) != TASKS:
        raise SystemExit(f"STOP: expected {TASKS} unique tasks, found {len(keys)} ({len(set(keys))} unique)")
    runs = {run: load(run) for arm in ARMS.values() for run in arm}
    for name, results in runs.items():
        if set(results) != set(keys):
            raise SystemExit(f"STOP: {name} does not cover the {TASKS} tasks ({len(results)} results)")
    failure = {name: {k: distill.filter_failure_class(r[k].get("run") or {}) for k in keys} for name, r in runs.items()}
    passed = {name: {k: failure[name][k] is None for k in keys} for name in runs}

    print(f"first attempt, {TASKS} tasks: passed, failed, coded / return code / wrong output")
    for name, results in runs.items():
        classes = [c for c in failure[name].values() if c]
        empty = sum(1 for k in keys if not (results[k].get("source_code") or "").strip())
        capped = sum(1 for k in keys if results[k].get("finish_reason") == "length")
        print(f"  {name:26s} {TASKS - len(classes):5d} {len(classes):5d}  "
              f"{classes.count('coded')} / {classes.count('rc')} / {classes.count('wrong')}"
              f"   empty answers {empty}, length caps {capped}")

    def rate(run: str) -> float:
        return sum(passed[run].values()) / TASKS

    def pairwise(a: str, b: str) -> str:
        mean, low, high = interval([passed[a][k] - passed[b][k] for k in keys])
        gained = sum(1 for k in keys if passed[a][k] and not passed[b][k])
        lost = sum(1 for k in keys if passed[b][k] and not passed[a][k])
        return (f"{100 * mean:+.2f} points [{100 * low:+.2f}, {100 * high:+.2f}], gained {gained}, lost {lost}, "
                f"sign test p={sign_test(gained, lost):.3g}")

    arm_pass = {arm: {k: sum(passed[r][k] for r in members) / len(members) for k in keys} for arm, members in ARMS.items()}
    mean, low, high = interval([arm_pass["la"][k] - arm_pass["base"][k] for k in keys])
    seed_gap = abs(rate(ARMS["base"][0]) - rate(ARMS["base"][1]))
    (l1, l2), (b1, b2) = ARMS["la"], ARMS["base"]
    # A contrast with one run of each arm on each side: it holds no la effect.
    seed_only = interval([(passed[l1][k] + passed[b2][k]) / 2 - (passed[l2][k] + passed[b1][k]) / 2 for k in keys])
    pair_totals = [sum(passed[la_run].values()) - sum(passed[base_run].values())
                   for la_run in (l1, l2) for base_run in (b1, b2)]
    clear, why = seed_check(mean, pair_totals, seed_only)
    print(f"\nTHE READING: la minus base, two seeds each, all {TASKS} tasks")
    print(f"  {100 * mean:+.2f} points [{100 * low:+.2f}, {100 * high:+.2f}]; base seeds differ by {100 * seed_gap:.2f}")
    print(f"  seed check: seed-only contrast, {{{l1}, {b2}}} minus {{{l2}, {b1}}}: {100 * seed_only[0]:+.2f} points "
          f"[{100 * seed_only[1]:+.2f}, {100 * seed_only[2]:+.2f}]; la-run minus base-run totals "
          f"{', '.join(f'{total:+d}' for total in pair_totals)}")
    print(f"  -> {reading(low, high, seed_gap, clear, why)}")
    print("\nreported, not deciding")
    for (la_run, base_run) in zip(ARMS["la"], ARMS["base"]):
        print(f"  {la_run} minus {base_run}: {pairwise(la_run, base_run)}")
    for arm, (a, b) in ARMS.items():
        print(f"  seed gap, {arm}: {pairwise(a, b)}")
    for la_run, base_run in ((l1, b2), (l2, b1)):
        print(f"  {la_run} minus {base_run}: {pairwise(la_run, base_run)}")


if __name__ == "__main__":
    main()
