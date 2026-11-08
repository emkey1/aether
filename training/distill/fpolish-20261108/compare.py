#!/usr/bin/env python3
"""The reading fixed in README.md: the polished TIES merge against gemma4-12b-v3 on the 1,026 fresh tasks of fv3-20261105. Run from the repo root."""

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


def main() -> None:
    runs = {"polish": load(HERE / "report__filter.json.gz"), "v3": load(FV3 / "report__filter_v3.json.gz"),
            "v3 seed 42": load(FV3 / "report__filter_v3_s42.json.gz"), "v2": load(FV3 / "report__filter_v2.json.gz"),
            "v2 seed 42": load(FV3 / "report__filter_v2_s42.json.gz")}
    keys = set(runs["v3"])
    if any(set(r) != keys for r in runs.values()):
        raise SystemExit("STOP: the runs do not cover the same tasks")
    for name, run in runs.items():
        c = collections.Counter(kind(a) for a in run.values() if not passed(a))
        print(f"{name:11s} failed {sum(c.values()):4d} of {len(keys)}: coded {c['coded']}, return code {c['rc']}, wrong output {c['wrong']}, "
              f"length caps {sum(a.get('finish_reason') == 'length' for a in run.values())}")
    new = runs["polish"]
    for name in ("v3", "v3 seed 42", "v2", "v2 seed 42"):
        a = sum(not passed(new[k]) and passed(runs[name][k]) for k in keys)
        b = sum(not passed(runs[name][k]) and passed(new[k]) for k in keys)
        p = sign_test(a, b)
        print(f"\nagainst {name}: the polish fails alone {a}, {name} fails alone {b}, p={p:.2g}")
        if name == "v3":
            print("THE READING: " + ("BETTER THAN A PLAIN RUN" if p < 0.05 and a < b else "WORSE THAN A PLAIN RUN" if p < 0.05 else "NO BETTER THAN A PLAIN RUN"))


if __name__ == "__main__":
    main()
