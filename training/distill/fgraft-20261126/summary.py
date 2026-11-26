#!/usr/bin/env python3
"""Summarises graft_eval.py's losses.json: each candidate's mean loss, the screen rule of README.md, and paired
differences against each parent over the same records (bootstrap 10,000, seed 42). Prints no record id or text.

    summary.py losses.json
"""
import json
import random
import sys

d = json.load(open(sys.argv[1]))
c = d["candidates"]
n = len(d["records"])


def interval(diffs):
    rng = random.Random(42)
    means = sorted(sum(rng.choices(diffs, k=len(diffs))) / len(diffs) for _ in range(10_000))
    return sum(diffs) / len(diffs), means[250], means[9749]


parents = ["v3", "v3_s42"]
ceiling = max(c[p]["mean"] for p in parents if p in c) if all(p in c for p in parents) else None
print(f"{n} held-out records; loss = mean over records of each record's mean response-token loss")
print(f"{'candidate':10s} {'mean':>7s} {'token':>7s}  screen")
for name, r in c.items():
    if name in parents:
        verdict = "parent"
    elif name in ("late_v3", "early_v3", "base"):
        verdict = "reported"
    else:
        verdict = "passes" if ceiling is not None and r["mean"] <= ceiling else "fails"
    print(f"{name:10s} {r['mean']:7.4f} {r['token_mean']:7.4f}  {verdict}")
if ceiling is not None:
    print(f"screen ceiling (the higher parent): {ceiling:.4f}")
for name, r in c.items():
    if name in parents:
        continue
    parts = []
    for p in parents:
        if p not in c:
            continue
        diffs = [x - y for x, y in zip(r["per_record"], c[p]["per_record"])]
        mean, low, high = interval(diffs)
        lower = sum(1 for x in diffs if x < 0)
        parts.append(f"vs {p}: {mean:+.4f} [{low:+.4f}, {high:+.4f}], lower on {lower}/{n}")
    print(f"  {name:9s} " + "; ".join(parts))
if all(p in c for p in parents):
    diffs = [x - y for x, y in zip(c["v3_s42"]["per_record"], c["v3"]["per_record"])]
    mean, low, high = interval(diffs)
    print(f"  parents: v3_s42 - v3 {mean:+.4f} [{low:+.4f}, {high:+.4f}], v3_s42 lower on {sum(1 for x in diffs if x < 0)}/{n}")
