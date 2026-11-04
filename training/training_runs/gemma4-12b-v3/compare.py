#!/usr/bin/env python3
"""The reading fixed in training/PLAN-failure-targeted-batch.md before these runs were trained: targeted minus baseline. Run from the repo root.

The measure is the first-attempt pass rate on the 53 held-out tasks at T=0, averaged over each arm's two seeds. A paired bootstrap over
tasks gives the 95% interval (10,000 resamples, seed 42). The verdict is printed from the three conditions below and is not a judgment
made afterwards. This script reads pass or fail and the failure's class, never a held-out program or its output.
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
BASELINE = {"seed 3407": "gemma4-12b-v2", "seed 42": "gemma4-12b-v2-s42"}
TARGETED = {"seed 3407": "gemma4-12b-v3", "seed 42": "gemma4-12b-v3-s42"}
RESAMPLES = 10_000


def passed(attempt: dict) -> bool:
    run = attempt.get("run") or {}
    return bool(run.get("exact_stdout_match")) and run.get("returncode") == run.get("expected_returncode")


def failure_class(attempt: dict) -> str:
    run = attempt.get("run") or {}
    if re.search(r"\b[A-Z]{2,}-\d{3}\b", str(run.get("stderr") or "")):
        return "coded"
    return "wrong" if run.get("returncode") in (None, run.get("expected_returncode")) else "rc"


def load(tag: str) -> dict:
    """{(suite, task): result} for one run's reports."""
    out = {}
    for suite in MAIN + HELD:
        path = f"{ROOT}/{tag}/score/tuned__{suite}.json"
        if not os.path.exists(path):
            path += ".gz"
        with (gzip.open(path, "rt") if path.endswith(".gz") else open(path)) as handle:
            report = json.load(handle)
        for result in report["destinations"][0]["variants"][0]["results"]:
            out[(suite, result["task_id"])] = result
    return out


def arm_rates(runs: list[dict], keys: list, index: int) -> list[float]:
    """Each task's pass rate over the arm's seeds. index 0 is the first attempt, -1 is after repair."""
    return [sum(passed(run[k]["attempts"][index]) for run in runs) / len(runs) for k in keys]


def interval(diffs: list[float]) -> tuple[float, float, float]:
    rng = random.Random(42)
    n = len(diffs)
    means = sorted(sum(rng.choices(diffs, k=n)) / n for _ in range(RESAMPLES))
    return sum(diffs) / n, means[int(0.025 * RESAMPLES)], means[int(0.975 * RESAMPLES) - 1]


def keys_of(run: dict, suites: list[str]) -> list:
    return sorted(k for k in run if k[0] in suites)


def audit(name: str, run: dict, reference: dict) -> None:
    attempts = [a for r in run.values() for a in r["attempts"]]
    same = sum(run[k]["attempts"][0].get("prompt_sha256") == reference[k]["attempts"][0].get("prompt_sha256") for k in run)
    print(f"{name}: {len(run)} tasks, infra {sum(bool(r.get('infra_failed')) for r in run.values())}, "
          f"empty {sum(not (a.get('source_code') or '').strip() for a in attempts)}, "
          f"length caps {sum(a.get('finish_reason') == 'length' for a in attempts)}, prompts matching gemma4-12b-v2 {same}/{len(run)}")


def difference(label: str, targeted: list[dict], baseline: list[dict], keys: list, index: int) -> tuple[float, float, float]:
    a, b = arm_rates(targeted, keys, index), arm_rates(baseline, keys, index)
    mean, low, high = interval([x - y for x, y in zip(a, b)])
    print(f"  {label:18s} {sum(a):5.1f} / {sum(b):5.1f} of {len(keys)}   {100 * mean:+5.1f} points [{100 * low:+5.1f}, {100 * high:+5.1f}]")
    return mean, low, high


def main() -> None:
    base, target = {s: load(t) for s, t in BASELINE.items()}, {s: load(t) for s, t in TARGETED.items()}
    reference = base["seed 3407"]
    if len({frozenset(run) for run in [*base.values(), *target.values()]}) != 1:
        raise SystemExit("STOP: the runs do not cover the same tasks")
    for seed, run in base.items():
        audit(f"baseline, {seed}", run, reference)
    for seed, run in target.items():
        audit(f"targeted, {seed}", run, reference)

    b_runs, t_runs = list(base.values()), list(target.values())
    held, main_keys = keys_of(reference, HELD), keys_of(reference, MAIN)

    print("\nby run, first attempt (after repair), main / held-out")
    for arm, runs in (("baseline", base), ("targeted", target)):
        for seed, run in runs.items():
            cells = [f"{sum(passed(run[k]['attempts'][0]) for k in keys)} ({sum(passed(run[k]['attempts'][-1]) for k in keys)})"
                     for keys in (main_keys, held)]
            print(f"  {arm} {seed:10s} {cells[0]:9s} / {cells[1]}")

    print("\ndeciding, targeted / baseline in tasks' worth of mean rate, then the difference")
    mean, low, _ = difference("held-out, first", t_runs, b_runs, held, 0)
    _, _, main_high = difference("main, first", t_runs, b_runs, main_keys, 0)
    rates = [sum(arm_rates([run], held, 0)) / len(held) for run in b_runs]
    gap = abs(rates[0] - rates[1])
    target_rates = [sum(arm_rates([run], held, 0)) / len(held) for run in t_runs]
    print(f"  gap between the baseline seeds, held-out first attempt: {100 * gap:.1f} points; between the targeted seeds: "
          f"{100 * abs(target_rates[0] - target_rates[1]):.1f}")
    conditions = {"the 95% interval is above zero": low > 0, "the difference is larger than the baseline seeds' gap": mean > gap,
                  "main's interval is not wholly below zero": main_high >= 0}
    for text, held_true in conditions.items():
        print(f"  {'yes' if held_true else 'NO '}  {text}")
    print(f"\nVERDICT: {'A GAIN; run the control' if all(conditions.values()) else 'NO GAIN by the fixed reading'}")

    print("\nreported, not deciding")
    difference("held-out, repaired", t_runs, b_runs, held, -1)
    difference("main, repaired", t_runs, b_runs, main_keys, -1)
    for seed in BASELINE:
        difference(f"held-out, {seed}", [target[seed]], [base[seed]], held, 0)

    print("\nby suite, first attempt (after repair), tasks' worth of mean rate, targeted / baseline")
    for suite in MAIN + HELD:
        keys = keys_of(reference, [suite])
        print(f"  {suite:14s} {sum(arm_rates(t_runs, keys, 0)):5.1f} ({sum(arm_rates(t_runs, keys, -1)):5.1f}) / "
              f"{sum(arm_rates(b_runs, keys, 0)):5.1f} ({sum(arm_rates(b_runs, keys, -1)):5.1f})  of {len(keys)}")

    print("\nfailures by class per seed, coded / return code / wrong output, first attempt, then after repair")
    for name, keys in (("main", main_keys), ("held-out", held)):
        for arm, runs in (("baseline", base), ("targeted", target)):
            for seed, run in runs.items():
                cells = []
                for index in (0, -1):
                    classes = collections.Counter(failure_class(run[k]["attempts"][index]) for k in keys
                                                  if not passed(run[k]["attempts"][index]))
                    cells.append(f"{classes['coded']} / {classes['rc']} / {classes['wrong']}")
                print(f"  {name:8s} {arm} {seed:10s} {cells[0]:12s}   {cells[1]}")


if __name__ == "__main__":
    main()
