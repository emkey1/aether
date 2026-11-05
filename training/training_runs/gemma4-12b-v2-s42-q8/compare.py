#!/usr/bin/env python3
"""The reading fixed in README.md before scoring: the second seed at 8 bits against the published int8 form. Run from the repo root.

Reads pass or fail and the failure's class. Program text is only compared for equality, and nothing here prints a task id or a program.
"""

from __future__ import annotations

import collections
import gzip
import json
import math
import re
import statistics

MAIN = ["v2_pos", "hard_v2", "hard_nontoon", "cs"]
HELD = ["frontier", "frontier_algo", "frontier_spec", "sealed_toon"]
ALL = MAIN + HELD
ROOT = "training/training_runs"
RUNS = {
    "s42 q8": f"{ROOT}/gemma4-12b-v2-s42-q8/score",
    "s42 16": f"{ROOT}/gemma4-12b-v2-s42/score",
    "pub q8": f"{ROOT}/gemma4-12b-v2-q8/score",
    "pub 16": f"{ROOT}/gemma4-12b-v2/score",
    "e3 mxfp4": f"{ROOT}/e3-int4-1.0-20261101/score/mxfp4",
    "e3 q8": f"{ROOT}/e3-int4-1.0-20261101/score/q8",
    "e3": f"{ROOT}/qwen36-35b-a3b-e3/score",
}


def passed(attempt: dict) -> bool:
    run = attempt.get("run") or {}
    return bool(run.get("exact_stdout_match")) and run.get("returncode") == run.get("expected_returncode")


def failure_class(attempt: dict) -> str:
    run = attempt.get("run") or {}
    if re.search(r"\b[A-Z]{2,}-\d{3}\b", str(run.get("stderr") or "")):
        return "coded"
    return "wrong" if run.get("returncode") in (None, run.get("expected_returncode")) else "rc"


def load(directory: str) -> dict:
    out = {}
    for suite in ALL:
        with gzip.open(f"{directory}/tuned__{suite}.json.gz", "rt") as handle:
            for result in json.load(handle)["destinations"][0]["variants"][0]["results"]:
                out[(suite, result["task_id"])] = result
    return out


def score(run: dict, suites: list[str], index: int) -> int:
    return sum(passed(r["attempts"][index]) for k, r in run.items() if k[0] in suites)


def sign_test(a: int, b: int) -> float:
    n = a + b
    return min(1.0, 2 * sum(math.comb(n, k) for k in range(min(a, b) + 1)) / 2 ** n) if n else 1.0


def split(left: dict, right: dict, suites: list[str], index: int) -> tuple[int, int, float]:
    a = sum(passed(left[k]["attempts"][index]) and not passed(right[k]["attempts"][index]) for k in left if k[0] in suites)
    b = sum(passed(right[k]["attempts"][index]) and not passed(left[k]["attempts"][index]) for k in left if k[0] in suites)
    return a, b, sign_test(a, b)


def main() -> None:
    runs = {name: load(where) for name, where in RUNS.items()}
    new, published = runs["s42 q8"], runs["pub q8"]
    if len({frozenset(run) for run in runs.values()}) != 1:
        raise SystemExit("STOP: the runs do not cover the same tasks")

    print("audit")
    for name, run in runs.items():
        attempts = [a for r in run.values() for a in r["attempts"]]
        same = sum(run[k]["attempts"][0].get("prompt_sha256") == published[k]["attempts"][0].get("prompt_sha256") for k in run)
        print(f"  {name:9s} {len(run)} tasks, infra {sum(bool(r.get('infra_failed')) for r in run.values())}, "
              f"empty {sum(not (a.get('source_code') or '').strip() for a in attempts)}, "
              f"length caps {sum(a.get('finish_reason') == 'length' for a in attempts)}, prompts matching the published int8 {same}")

    print("\nfirst attempt (after repair)")
    for name, run in runs.items():
        print(f"  {name:9s} main {score(run, MAIN, 0)} ({score(run, MAIN, -1)})  "
              f"held-out {score(run, HELD, 0)} ({score(run, HELD, -1)})  all {score(run, ALL, 0)} ({score(run, ALL, -1)})")

    a, b, p = split(new, published, ALL, -1)
    first_a, first_b, first_p = split(new, published, ALL, 0)
    conditions = {f"after repair, all 126: {a} to {b}, p={p:.4f}, more in its favour at p < 0.05": a > b and p < 0.05,
                  f"first attempts, all 126: {first_a} to {first_b}, p={first_p:.4f}, not lower at p < 0.05": not (first_b > first_a and first_p < 0.05)}
    print("\nthe fixed reading, the second seed at int8 against the published int8")
    for text, held_true in conditions.items():
        print(f"  {'yes' if held_true else 'NO '}  {text}")
    print(f"\nVERDICT: {'IT KEEPS THE LEAD; publishing is the owner decision' if all(conditions.values()) else 'IT DOES NOT KEEP THE LEAD; the published form stays'}")

    print("\nreported, not deciding")
    print("the second seed at int8 against each, tasks only one passes: first attempt, after repair")
    for name in runs:
        if name == "s42 q8":
            continue
        for label, suites in (("main", MAIN), ("held-out", HELD), ("all", ALL)):
            cells = ["{} to {} (p={:.3f})".format(*split(new, runs[name], suites, index)) for index in (0, -1)]
            print(f"  {name:9s} {label:9s} {cells[0]:22s} {cells[1]}")

    print("\nby suite: " + " / ".join(runs))
    for suite in ALL:
        cells = [f"{score(run, [suite], 0)} ({score(run, [suite], -1)})" for run in runs.values()]
        print(f"  {suite:14s} {sum(k[0] == suite for k in new):3d}  " + " / ".join(cells))

    print("\nfailing programs, coded / rc / wrong: first attempt, after repair, failures repair fixed")
    for name, run in runs.items():
        cells = []
        for index in (0, -1):
            counts = collections.Counter(failure_class(r["attempts"][index]) for r in run.values() if not passed(r["attempts"][index]))
            cells.append(f"{counts['coded']} / {counts['rc']} / {counts['wrong']}")
        failed = [r for r in run.values() if not passed(r["attempts"][0])]
        print(f"  {name:9s} {cells[0]:12s} {cells[1]:12s} {sum(passed(r['attempts'][-1]) for r in failed)} of {len(failed)}")

    print("\nfirst programs byte-identical")
    for x, y in (("s42 q8", "s42 16"), ("pub q8", "pub 16"), ("s42 q8", "pub q8")):
        same = sum(runs[x][k]["attempts"][0].get("source_code") == runs[y][k]["attempts"][0].get("source_code") for k in new)
        print(f"  {x} against {y}: {same} of {len(new)}")

    print("\nmedian completion tokens, first attempts")
    for name, run in runs.items():
        print(f"  {name:9s} {statistics.median((r['attempts'][0].get('usage') or {}).get('completion_tokens') or 0 for r in run.values())}")


if __name__ == "__main__":
    main()
