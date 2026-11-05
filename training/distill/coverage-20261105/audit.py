#!/usr/bin/env python3
"""What do the two Gemma seeds fail on, and how much did their training data teach it? Run from the repo root: audit.py INSTRUCTION.jsonl REPAIR.jsonl

Over the 1,776 fresh tasks of f1, f2 and f3-20261103, against v2-20261101's records. Reads pass or fail, diagnostics and which builtins a program calls. Prints no task id or program.
The tasks with a pair are those the published seed failed and a guided solver then solved, so the third table compares what the solvers reached for with what the fine-tune did.
"""

from __future__ import annotations

import collections
import gzip
import json
import pathlib
import re
import sys

ROOT = pathlib.Path("training/distill")
SLICES = ["f1-20261103", "f2-20261103", "f3-20261103"]
CODE = re.compile(r"\b[A-Z]{2,}-\d{3}\b")
GUIDE = pathlib.Path("docs/aether_for_llms_and_others.md")


def report(folder: str) -> dict:
    with gzip.open(ROOT / folder / "report__filter.json.gz", "rt") as handle:
        return {r["task_id"]: r["attempts"][0] for r in json.load(handle)["destinations"][0]["variants"][0]["results"]}


def passed(attempt: dict) -> bool:
    run = attempt.get("run") or {}
    return bool(run.get("exact_stdout_match")) and run.get("returncode") == run.get("expected_returncode")


def builtins() -> list[str]:
    names = []
    for line in GUIDE.read_text(encoding="utf-8").splitlines():
        match = re.match(r"\| `([a-z_0-9]+)\(", line)
        if match and match.group(1) not in names:
            names.append(match.group(1))
    return names


def uses(name: str, source: str) -> bool:
    return re.search(r"(?<![A-Za-z0-9_.])" + re.escape(name) + r"\(", source) is not None


def training_counts(path: str, names: list[str]) -> tuple[int, collections.Counter]:
    counts, records = collections.Counter(), 0
    for line in open(path, encoding="utf-8"):
        record = json.loads(line)
        if record.get("language") not in (None, "aether"):
            continue
        records += 1
        source = "\n".join(m["content"] for m in record["messages"] if m["role"] == "assistant")
        counts.update(n for n in names if uses(n, source))
    return records, counts


def message(attempt: dict) -> str:
    """The first diagnostic line with its code and quoted names, digits normalised."""
    text = str((attempt.get("run") or {}).get("stderr") or "")
    match = CODE.search(text)
    if not match:
        return ""
    line = next(l for l in text.splitlines() if match.group(0) in l)
    line = line[line.find(match.group(0)):]
    return re.sub(r"\d+", "N", line.replace("Aether ", ""))[:92]


def main() -> None:
    names = builtins()
    instruction_records, trained = training_counts(sys.argv[1], names)
    repair_records, repaired = training_counts(sys.argv[2], names)
    published = {}
    pairs = {}
    for name in SLICES:
        published |= report(name)
        pairs |= {p["id"]: p["solution"] for p in json.loads((ROOT / name / "pairs.json").read_text(encoding="utf-8"))["pairs"]}
    second = report("fs42-20261105")
    e3 = report("fe3-20261104")
    runs = {"published": published, "seed 42": second, "e3": e3}
    print(f"{len(names)} builtins in the guide's list; v2 has {instruction_records} Aether instruction records and {repair_records} repair records; {len(pairs)} solver pairs; {len(published)} tasks")

    print("\n1. the commonest diagnostics, with names: published / seed 42 / e3")
    counts = {n: collections.Counter(message(a) for a in r.values() if not passed(a) and message(a)) for n, r in runs.items()}
    total = collections.Counter()
    for c in counts.values():
        total.update(c)
    for text, _ in sorted(total.items(), key=lambda kv: -(counts["published"][kv[0]] + counts["seed 42"][kv[0]]))[:24]:
        print(f"  {counts['published'][text]:3d} {counts['seed 42'][text]:3d} {counts['e3'][text]:3d}  {text}")

    print("\n2. builtins that scarcely appear in training but appear in the guide: records, then solver pairs using it, then fine-tune programs per seed on all tasks")
    usage = {n: {b: sum(uses(b, a.get("source_code") or "") for a in r.values()) for b in names} for n, r in runs.items()}
    solver = {b: sum(uses(b, s) for s in pairs.values()) for b in names}
    print(f"  never in training: {sum(trained[b] == 0 for b in names)} of {len(names)}; at most 5 records: {sum(trained[b] <= 5 for b in names)}")

    print("\n3. what solvers reached for on the tasks the published seed failed, against the fine-tune on the same tasks: solver pairs, seed 3407, seed 42, training records")
    rows = []
    for b in names:
        on_a = sum(uses(b, published[k].get("source_code") or "") for k in pairs)
        on_b = sum(uses(b, second[k].get("source_code") or "") for k in pairs)
        if solver[b] >= 4 or on_a + on_b >= 8:
            rows.append((solver[b] - (on_a + on_b) / 2, b, solver[b], on_a, on_b, trained[b]))
    for gap, b, s, on_a, on_b, t in sorted(rows, reverse=True)[:14]:
        print(f"  {b:22s} {s:4d} {on_a:4d} {on_b:4d}  {t:5d}")
    print("  ...")
    for gap, b, s, on_a, on_b, t in sorted(rows)[:10]:
        print(f"  {b:22s} {s:4d} {on_a:4d} {on_b:4d}  {t:5d}")

    print("\n4. failure rate among programs calling a builtin, at least 25 programs at each seed, highest first: programs and rate at each seed, training records")
    overall = {n: 100 * sum(not passed(a) for a in r.values()) / len(r) for n, r in runs.items()}
    print(f"  overall: published {overall['published']:.0f}%, seed 42 {overall['seed 42']:.0f}%, e3 {overall['e3']:.0f}%")
    rates = []
    for b in names:
        with_a = [a for a in published.values() if uses(b, a.get("source_code") or "")]
        with_b = [a for a in second.values() if uses(b, a.get("source_code") or "")]
        if len(with_a) >= 25 and len(with_b) >= 25:
            rate_a = 100 * sum(not passed(a) for a in with_a) / len(with_a)
            rate_b = 100 * sum(not passed(a) for a in with_b) / len(with_b)
            rates.append(((rate_a + rate_b) / 2, b, len(with_a), rate_a, len(with_b), rate_b, trained[b]))
    for _, b, n_a, rate_a, n_b, rate_b, t in sorted(rates, reverse=True)[:16]:
        print(f"  {b:22s} {n_a:4d} {rate_a:3.0f}%  {n_b:4d} {rate_b:3.0f}%  {t:5d}")

    print("\n5. training coverage of the sort family")
    for b in ("sorted", "sorted_desc", "sorted_by", "sorted_by_desc"):
        print(f"  {b:16s} training {trained[b]:4d}, repair {repaired[b]:3d}, solver pairs {solver[b]:3d}, programs: published {usage['published'][b]:4d}, seed 42 {usage['seed 42'][b]:4d}, e3 {usage['e3'][b]:4d}")


if __name__ == "__main__":
    main()
