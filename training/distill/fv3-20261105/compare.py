#!/usr/bin/env python3
"""The reading fixed in README.md: targeted minus baseline on new fresh tasks, first attempts at int8, averaged over each arm's two seeds. Run from the repo root.

Reads pass or fail, each failure's class, and which sorting calls a program makes. Prints no task id or program.
"""

from __future__ import annotations

import collections
import gzip
import json
import math
import pathlib
import random
import re
import statistics
import sys

sys.path.insert(0, "tools")
import aether_distill as distill  # noqa: E402

HERE = pathlib.Path(__file__).resolve().parent
RUNS = {"baseline, seed 3407": "v2", "baseline, seed 42": "v2_s42", "targeted, seed 3407": "v3", "targeted, seed 42": "v3_s42"}
RESAMPLES = 10_000
AREAS = {"core": distill.CATEGORIES, "JSON documents and data files": distill.TOON_FILE_CATEGORIES, "keyed data": distill.MAP_CATEGORIES}


def load(tag: str) -> dict:
    path = HERE / f"report__filter_{tag}.json.gz"
    if not path.exists():
        path = HERE / f"filter_{tag}.json"
    with (gzip.open(path, "rt") if path.suffix == ".gz" else open(path)) as handle:
        report = json.load(handle)
    return {r["task_id"]: r["attempts"][0] for r in report["destinations"][0]["variants"][0]["results"]}


def failure(attempt: dict) -> str | None:
    return distill.filter_failure_class(attempt.get("run") or {})


def sign_test(a: int, b: int) -> float:
    n, k = a + b, min(a, b)
    return min(1.0, 2 * sum(math.comb(n, i) for i in range(k + 1)) / 2 ** n) if n else 1.0


def interval(diffs: list[float]) -> tuple[float, float, float]:
    rng = random.Random(42)
    n = len(diffs)
    means = sorted(sum(rng.choices(diffs, k=n)) / n for _ in range(RESAMPLES))
    return sum(diffs) / n, means[int(0.025 * RESAMPLES)], means[int(0.975 * RESAMPLES) - 1]


def sorted_calls(source: str) -> tuple[bool, bool]:
    """Whether a program uses sorted_by or sorted_by_desc, and whether it passes sorted more than one argument."""
    by = bool(re.search(r"\bsorted_by(_desc)?\(", source))
    extra = False
    for match in re.finditer(r"\bsorted\(", source):
        depth, i, in_text = 1, match.end(), False
        while i < len(source) and depth:
            c = source[i]
            if c == '"':
                in_text = not in_text
            elif not in_text:
                depth += c in "([{"
                depth -= c in ")]}"
                extra = extra or (c == "," and depth == 1)
            i += 1
    return by, extra


def main() -> None:
    runs = {name: load(tag) for name, tag in RUNS.items()}
    tasks = {t["id"]: t for t in json.loads((HERE / "tasks_frozen.json").read_text(encoding="utf-8"))["tasks"]}
    if any(set(run) != set(tasks) for run in runs.values()):
        raise SystemExit("STOP: the runs do not cover the same tasks")
    reference = runs["baseline, seed 3407"]
    keys = sorted(tasks)
    total = len(keys)

    print("audit")
    for name, run in runs.items():
        attempts = list(run.values())
        print(f"  {name:20s} {len(run)} tasks, empty {sum(not (a.get('source_code') or '').strip() for a in attempts)}, "
              f"length caps {sum(a.get('finish_reason') == 'length' for a in attempts)}, "
              f"grader timeouts {sum(bool((a.get('run') or {}).get('timed_out')) for a in attempts)}, "
              f"prompts matching the first run {sum(run[k].get('prompt_sha256') == reference[k].get('prompt_sha256') for k in keys)}, "
              f"grader {sorted({str((a.get('run') or {}).get('binary_sha256'))[:12] for a in attempts})}")

    print(f"\nfirst attempt, {total} tasks: passed, failed, coded / return code / wrong output")
    for name, run in runs.items():
        classes = collections.Counter(failure(a) for a in run.values() if failure(a))
        print(f"  {name:20s} {total - sum(classes.values()):4d} {sum(classes.values()):4d}   {classes['coded']} / {classes['rc']} / {classes['wrong']}")

    baseline = [runs["baseline, seed 3407"], runs["baseline, seed 42"]]
    targeted = [runs["targeted, seed 3407"], runs["targeted, seed 42"]]
    diffs = [sum(not failure(r[k]) for r in targeted) / 2 - sum(not failure(r[k]) for r in baseline) / 2 for k in keys]
    mean, low, high = interval(diffs)
    print(f"\nTHE READING: targeted minus baseline {100 * mean:+.2f} points, 95% interval [{100 * low:+.2f}, {100 * high:+.2f}], {total * high:.0f} tasks of {total} at the upper end")
    print(f"  {'A GAIN' if low > 0 else 'A LOSS' if high < 0 else 'NO READABLE DIFFERENCE'}")

    print("\nreported, not deciding")
    for seed in ("3407", "42"):
        t, b = runs[f"targeted, seed {seed}"], runs[f"baseline, seed {seed}"]
        gained, lost = sum(bool(failure(b[k])) and not failure(t[k]) for k in keys), sum(bool(failure(t[k])) and not failure(b[k]) for k in keys)
        print(f"  seed {seed}: targeted gains {gained}, loses {lost}, p={sign_test(gained, lost):.2g}")
    for label, first, second in (("baseline", *baseline), ("targeted", *targeted)):
        only_first, only_second = sum(bool(failure(second[k])) and not failure(first[k]) for k in keys), sum(bool(failure(first[k])) and not failure(second[k]) for k in keys)
        print(f"  {label} seeds: seed 3407 alone passes {only_first}, seed 42 alone passes {only_second}, p={sign_test(only_first, only_second):.2g}")

    print("\nby area, failures: tasks, then each run")
    for area, categories in AREAS.items():
        ids = [k for k in keys if tasks[k]["category"] in categories]
        cells = "  ".join(f"{sum(bool(failure(run[k])) for k in ids):3d}" for run in runs.values())
        print(f"  {area:30s} {len(ids):4d}  {cells}")

    print("\ndoes a taught idiom transfer: programs using sorted_by, passing of those; programs passing sorted an extra argument")
    for name, run in runs.items():
        calls = {k: sorted_calls(run[k].get("source_code") or "") for k in keys}
        by = [k for k in keys if calls[k][0]]
        print(f"  {name:20s} {len(by):4d} {sum(not failure(run[k]) for k in by):4d}   {sum(calls[k][1] for k in keys):4d}")

    print("\nmedian completion tokens")
    for name, run in runs.items():
        print(f"  {name:20s} {statistics.median((a.get('usage') or {}).get('completion_tokens') or 0 for a in run.values())}")


if __name__ == "__main__":
    main()
