#!/usr/bin/env python3
"""The tables in RESULTS.md: Gemma 4 12B at its final step and step 575, against its bars. Run from the repo root."""

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
    "final": (f"{ROOT}/gemma4-12b-v2/score", "tuned"),
    "epoch 2": (f"{ROOT}/gemma4-12b-v2-ep2/score", "tuned"),
    "e3": (f"{ROOT}/qwen36-35b-a3b-e3/score", "tuned"),
    "e3 mxfp4": (f"{ROOT}/e3-int4-1.0-20261101/score/mxfp4", "tuned"),
    "guided": (f"{ROOT}/guided-gemma4-12b-1.0-20261031/score/g4", "guided"),
}


def passed(attempt: dict) -> bool:
    run = attempt.get("run") or {}
    return bool(run.get("exact_stdout_match")) and run.get("returncode") == run.get("expected_returncode")


def failure_class(attempt: dict) -> str:
    run = attempt.get("run") or {}
    if re.search(r"\b[A-Z]{2,}-\d{3}\b", str(run.get("stderr") or "")):
        return "coded"
    return "wrong" if run.get("returncode") in (None, run.get("expected_returncode")) else "rc"


def load(directory: str, prefix: str) -> dict:
    out = {}
    for suite in ALL:
        with gzip.open(f"{directory}/{prefix}__{suite}.json.gz", "rt") as handle:
            for result in json.load(handle)["destinations"][0]["variants"][0]["results"]:
                out[(suite, result["task_id"])] = result
    return out


def score(run: dict, suites: list[str], index: int) -> int:
    return sum(passed(r["attempts"][index]) for k, r in run.items() if k[0] in suites)


def sign_test(a: int, b: int) -> float:
    n = a + b
    return min(1.0, 2 * sum(math.comb(n, k) for k in range(min(a, b) + 1)) / 2 ** n) if n else 1.0


def main() -> None:
    runs = {name: load(*where) for name, where in RUNS.items()}
    e3 = runs["e3"]

    print("audit")
    for name, run in runs.items():
        attempts = [a for r in run.values() for a in r["attempts"]]
        same = sum(run[k]["attempts"][0].get("prompt_sha256") == e3[k]["attempts"][0].get("prompt_sha256") for k in run)
        print(f"  {name:9s} {len(run)} tasks, infra {sum(bool(r.get('infra_failed')) for r in run.values())}, "
              f"empty {sum(not (a.get('source_code') or '').strip() for a in attempts)}, "
              f"length caps {sum(a.get('finish_reason') == 'length' for a in attempts)}, prompts matching e3 {same}")

    print("\nfirst attempt (after repair)")
    for name, run in runs.items():
        print(f"  {name:9s} main {score(run, MAIN, 0)} ({score(run, MAIN, -1)})  "
              f"held-out {score(run, HELD, 0)} ({score(run, HELD, -1)})  all {score(run, ALL, 0)} ({score(run, ALL, -1)})")

    print("\nby suite: " + " / ".join(runs))
    for suite in ALL:
        cells = [f"{score(run, [suite], 0)} ({score(run, [suite], -1)})" for run in runs.values()]
        print(f"  {suite:14s} {sum(k[0] == suite for k in e3):3d}  " + " / ".join(cells))

    print("\nfailing programs, coded / rc / wrong: first attempt, after repair")
    for name, run in runs.items():
        cells = []
        for index in (0, -1):
            counts = collections.Counter(failure_class(r["attempts"][index]) for r in run.values()
                                         if not passed(r["attempts"][index]))
            cells.append(f"{counts['coded']} / {counts['rc']} / {counts['wrong']}")
        print(f"  {name:9s} {cells[0]:12s} {cells[1]}")

    final = runs["final"]
    print("\nfinal against each, tasks only one passes (two-sided sign test): first attempt, after repair")
    for name in ("epoch 2", "e3", "e3 mxfp4", "guided"):
        for label, suites in (("main", MAIN), ("held-out", HELD), ("all", ALL)):
            cells = []
            for index in (0, -1):
                a = sum(passed(final[k]["attempts"][index]) and not passed(runs[name][k]["attempts"][index])
                        for k in final if k[0] in suites)
                b = sum(passed(runs[name][k]["attempts"][index]) and not passed(final[k]["attempts"][index])
                        for k in final if k[0] in suites)
                cells.append(f"{a} to {b} (p={sign_test(a, b):.4f})")
            print(f"  {name:9s} {label:9s} {cells[0]:24s} {cells[1]}")

    second = runs["epoch 2"]
    changed = [k for k in final if passed(final[k]["attempts"][0]) != passed(second[k]["attempts"][0])]
    print(f"\nthird pass: {len(changed)} first attempts change, "
          f"{sum(passed(second[k]['attempts'][0]) for k in changed)} lost, "
          f"{sum(passed(final[k]['attempts'][0]) for k in changed)} gained; "
          f"{sum(final[k]['attempts'][0].get('source_code') == second[k]['attempts'][0].get('source_code') for k in final)} "
          f"first programs identical")

    print("\nfirst attempts' completion tokens, median")
    for name, run in runs.items():
        print(f"  {name:9s} {statistics.median((r['attempts'][0].get('usage') or {}).get('completion_tokens') or 0 for r in run.values())}")


if __name__ == "__main__":
    main()
