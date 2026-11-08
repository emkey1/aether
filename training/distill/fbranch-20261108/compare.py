#!/usr/bin/env python3
"""The reading fixed in README.md: the averaged late branches against the better branch, on the 1,026 fresh tasks of fv3-20261105. Run from the repo root."""

from __future__ import annotations

import collections
import gzip
import json
import math
import pathlib
import re

HERE = pathlib.Path(__file__).resolve().parent
FV3 = HERE.parent / "fv3-20261105"


def load(path: pathlib.Path) -> dict:
    if not path.exists():
        path = path.with_suffix("")
    with (gzip.open(path, "rt") if path.suffix == ".gz" else open(path)) as handle:
        return {r["task_id"]: r["attempts"][0] for r in json.load(handle)["destinations"][0]["variants"][0]["results"]}


def passed(a: dict) -> bool:
    r = a.get("run") or {}
    return bool(r.get("exact_stdout_match")) and r.get("returncode") == r.get("expected_returncode")


def kind(a: dict) -> str:
    r = a.get("run") or {}
    if re.search(r"\b[A-Z]{2,}-\d{3}\b", str(r.get("stderr") or "")):
        return "coded"
    return "wrong" if r.get("returncode") in (None, r.get("expected_returncode")) else "rc"


def sign_test(a: int, b: int) -> float:
    n, k = a + b, min(a, b)
    return min(1.0, 2 * sum(math.comb(n, i) for i in range(k + 1)) / 2 ** n) if n else 1.0


def versus(x: dict, y: dict, keys: set) -> tuple[int, int, float]:
    a = sum(not passed(x[k]) and passed(y[k]) for k in keys)
    b = sum(not passed(y[k]) and passed(x[k]) for k in keys)
    return a, b, sign_test(a, b)


def main() -> None:
    runs = {"branch s3407": load(HERE / "report__filter_s3407.json.gz"), "branch s42": load(HERE / "report__filter_s42.json.gz"),
            "average": load(HERE / "report__filter_avg.json.gz"), "v3": load(FV3 / "report__filter_v3.json.gz")}
    keys = set(runs["v3"])
    if any(set(r) != keys for r in runs.values()):
        raise SystemExit("STOP: the runs do not cover the same tasks")
    fails = {}
    for name, run in runs.items():
        c = collections.Counter(kind(a) for a in run.values() if not passed(a))
        fails[name] = sum(c.values())
        print(f"{name:13s} failed {fails[name]:4d} of {len(keys)}: coded {c['coded']}, return code {c['rc']}, wrong output {c['wrong']}")
    better = min(("branch s3407", "branch s42"), key=lambda n: fails[n])
    a, b, p = versus(runs["average"], runs[better], keys)
    print(f"\nTHE READING, average against the better branch ({better}): the average fails alone {a}, the branch {b}, p={p:.2g}")
    print("  " + ("LATE-BRANCH AVERAGING HELPS" if p < 0.05 and a < b else "WORSE THAN A BRANCH" if p < 0.05 else "NO BETTER THAN A BRANCH"))
    print("\nreported")
    for name in ("branch s3407", "branch s42", "average"):
        a, b, p = versus(runs[name], runs["v3"], keys)
        print(f"  {name} against v3: fails alone {a}, v3 fails alone {b}, p={p:.2g}")


if __name__ == "__main__":
    main()
