#!/usr/bin/env python3
"""The repair test fixed in README.md: v3 against seed 42's baseline after two repair rounds, over the 1,026 fresh tasks. Run from the repo root."""

from __future__ import annotations

import gzip
import json
import math
import pathlib

HERE = pathlib.Path(__file__).resolve().parent


def load(name: str) -> dict:
    path = HERE / f"report__{name}.json.gz"
    if not path.exists():
        path = HERE / f"{name}.json"
    with (gzip.open(path, "rt") if path.suffix == ".gz" else open(path)) as handle:
        return {r["task_id"]: r for r in json.load(handle)["destinations"][0]["variants"][0]["results"]}


def passed(attempt: dict) -> bool:
    run = attempt.get("run") or {}
    return bool(run.get("exact_stdout_match")) and run.get("returncode") == run.get("expected_returncode")


def sign_test(a: int, b: int) -> float:
    n, k = a + b, min(a, b)
    return min(1.0, 2 * sum(math.comb(n, i) for i in range(k + 1)) / 2 ** n) if n else 1.0


def main() -> None:
    after = {}
    for tag in ("v3", "v2_s42"):
        first, repair = load(f"filter_{tag}"), load(f"repair_{tag}")
        failed = {k for k, r in first.items() if not passed(r["attempts"][0])}
        if set(repair) != failed:
            raise SystemExit(f"STOP: {tag}'s repair run does not cover exactly its failures")
        same = sum(repair[k]["attempts"][0].get("source_code") == first[k]["attempts"][0].get("source_code") for k in failed)
        fixed = {k for k in failed if passed(repair[k]["attempts"][-1])}
        after[tag] = {k for k in first if k not in failed or k in fixed}
        print(f"{tag:7s}: first attempt {len(first) - len(failed)} of {len(first)}, repair fixed {len(fixed)} of {len(failed)}, after repair {len(after[tag])}; "
              f"re-asked first programs identical {same} of {len(failed)}")
    a, b = len(after["v3"] - after["v2_s42"]), len(after["v2_s42"] - after["v3"])
    p = sign_test(a, b)
    print(f"\nTHE READING: after repair v3 alone passes {a}, seed 42 alone passes {b}, p={p:.2g}")
    print("  " + ("V3 IS BETTER AFTER REPAIR" if p < 0.05 and a > b else "SEED 42 IS BETTER AFTER REPAIR" if p < 0.05 else "NO DIFFERENCE AFTER REPAIR"))


if __name__ == "__main__":
    main()
