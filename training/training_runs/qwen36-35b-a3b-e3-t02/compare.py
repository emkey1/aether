#!/usr/bin/env python3
"""The reading README.md fixed before the results: epoch 3 minus epoch 2 in mean per-task pass rate, T=0.2, 5 repeats.

Run from the repo root. The task is the unit, because repeats of one task are not independent: a paired bootstrap over
tasks gives each 95% interval (10,000 resamples, seed 42).
"""

from __future__ import annotations

import collections
import gzip
import json
import os
import random
import re

MAIN = ["v2_pos", "hard_v2", "hard_nontoon", "cs"]
HELD = ["frontier", "frontier_algo", "frontier_spec", "sealed_toon"]
ROOT = "training/training_runs"
RESAMPLES = 10_000


def passed(attempt: dict) -> bool:
    run = attempt.get("run") or {}
    return bool(run.get("exact_stdout_match")) and run.get("returncode") == run.get("expected_returncode")


def wrong_output(attempt: dict) -> bool:
    """Ran to the expected return code with no coded diagnostic, and printed the wrong thing."""
    if passed(attempt):
        return False
    run = attempt.get("run") or {}
    if re.search(r"\b[A-Z]{2,}-\d{3}\b", str(run.get("stderr") or "")):
        return False
    return run.get("returncode") in (None, run.get("expected_returncode"))


def load(tag: str) -> dict:
    """{(suite, task): [result per repeat]} for one run's reports."""
    out = collections.defaultdict(list)
    for suite in MAIN + HELD:
        path = f"{ROOT}/{tag}/score/tuned__{suite}.json"
        if not os.path.exists(path):
            path += ".gz"
        with (gzip.open(path, "rt") if path.endswith(".gz") else open(path)) as handle:
            report = json.load(handle)
        for result in report["destinations"][0]["variants"][0]["results"]:
            out[(suite, result["task_id"])].append(result)
    return out


def rate(results: list[dict], index: int, test=passed) -> float:
    return sum(test(r["attempts"][index]) for r in results) / len(results)


def interval(diffs: list[float]) -> tuple[float, float, float]:
    rng = random.Random(42)
    n = len(diffs)
    means = sorted(sum(rng.choices(diffs, k=n)) / n for _ in range(RESAMPLES))
    return sum(diffs) / n, means[int(0.025 * RESAMPLES)], means[int(0.975 * RESAMPLES) - 1]


def audit(name: str, run: dict, reference: dict) -> None:
    results = [r for rs in run.values() for r in rs]
    attempts = [a for r in results for a in r["attempts"]]
    seeds = collections.Counter((r["attempts"][0].get("request") or {}).get("seed") for r in results)
    same = sum(run[k][0]["attempts"][0].get("prompt_sha256") == reference[k][0]["attempts"][0].get("prompt_sha256")
               for k in run)
    distinct = collections.Counter(len({r["attempts"][0].get("source_code") for r in rs}) for rs in run.values())
    print(f"{name}: {len(results)} cases on {len(run)} tasks, infra {sum(bool(r.get('infra_failed')) for r in results)}, "
          f"empty {sum(not (a.get('source_code') or '').strip() for a in attempts)}, "
          f"length caps {sum(a.get('finish_reason') == 'length' for a in attempts)}, seeds {sorted(seeds.items())}, "
          f"prompts matching T=0 {same}/{len(run)}, distinct first programs per task {sorted(distinct.items())}")


def main() -> None:
    e3, e2 = load("qwen36-35b-a3b-e3-t02"), load("qwen36-35b-a3b-e3-ep2-t02")
    t0_e3, t0_e2 = load("qwen36-35b-a3b-e3"), load("qwen36-35b-a3b-e3-ep2")
    audit("e3", e3, t0_e3)
    audit("e3 epoch 2", e2, t0_e2)
    print()
    print("mean per-task rate, epoch 3 / epoch 2, then the difference with its 95% interval (points; tasks in brackets)")
    for label, index, test in (("first attempt", 0, passed), ("after repair", -1, passed),
                               ("wrong output, first attempt", 0, wrong_output),
                               ("wrong output, after repair", -1, wrong_output)):
        print(f"\n{label}")
        for group, suites in (("main", MAIN), ("held-out", HELD)):
            keys = [k for k in e3 if k[0] in suites]
            a = [rate(e3[k], index, test) for k in keys]
            b = [rate(e2[k], index, test) for k in keys]
            mean, low, high = interval([x - y for x, y in zip(a, b)])
            n = len(keys)
            print(f"  {group:8s} {100 * sum(a) / n:5.1f}% ({sum(a):.1f}) / {100 * sum(b) / n:5.1f}% ({sum(b):.1f})   "
                  f"diff {100 * mean:+5.1f} [{100 * low:+5.1f}, {100 * high:+5.1f}]")
    print("\nby suite, first attempt (after repair), tasks' worth of mean rate, epoch 3 / epoch 2")
    for suite in MAIN + HELD:
        keys = [k for k in e3 if k[0] == suite]
        print(f"  {suite:14s} {sum(rate(e3[k], 0) for k in keys):5.1f} ({sum(rate(e3[k], -1) for k in keys):5.1f}) / "
              f"{sum(rate(e2[k], 0) for k in keys):5.1f} ({sum(rate(e2[k], -1) for k in keys):5.1f})  of {len(keys)}")
    print("\nT=0 against T=0.2, first attempt: how the one T=0 sample compares with its task's rate")
    for name, t0, t2 in (("e3", t0_e3, e3), ("e3 epoch 2", t0_e2, e2)):
        unanimous = sum(rate(t2[k], 0) in (0.0, 1.0) for k in t2)
        agree = sum(passed(t0[k][0]["attempts"][0]) == (rate(t2[k], 0) >= 0.5) for k in t2)
        print(f"  {name}: {unanimous} of {len(t2)} tasks unanimous at T=0.2; "
              f"the T=0 result matches the T=0.2 majority on {agree}")
    for label, group in (("main", MAIN), ("held-out", HELD)):
        keys = [k for k in e3 if k[0] in group]
        moved = sorted(((rate(e3[k], 0) - rate(e2[k], 0), k[1]) for k in keys), reverse=True)
        print(f"\nbiggest moves, {label}, first attempt, epoch 3 minus epoch 2 (of 5):")
        print("  " + ", ".join(f"{task} {round(5 * d):+d}" for d, task in moved if round(5 * d) >= 2))
        print("  " + ", ".join(f"{task} {round(5 * d):+d}" for d, task in moved if round(5 * d) <= -2))


if __name__ == "__main__":
    main()
