#!/usr/bin/env python3
"""The reading fixed in README.md: late minus all on fv3's 1,026 tasks, two seeds each. Run from the repo root.

Reads, beside this file, the one-session reports report__filter_{v3_late,v3,v3_late_s42,v3_s42}.json.gz (or
filter_<tag>.json) and session_server_version.txt (since README.md's amendment of 2026-11-26, the session's vLLM image,
grader and worker count on the training host). Reported, not deciding: the same all-layer weights' int8 answers in
the c1 session on the scoring Mac (../../distill/c1-20261118/, report__filter_{v3_c1s,v3_s42_c1s}.json.gz) and each
run's trainable parameters (train/run_metadata.json of each run). Prints no task id or program.
"""

from __future__ import annotations

import gzip
import json
import math
import pathlib
import random
import sys

sys.path.insert(0, "tools")
import aether_distill as distill  # noqa: E402

HERE = pathlib.Path(__file__).resolve().parent
RUNS = HERE.parent
FV3 = RUNS.parent / "distill" / "fv3-20261105"
C1 = RUNS.parent / "distill" / "c1-20261118"
# Arm members in seed order (3407, 42); every report is from this study's one session.
ARMS = {"late": ((HERE, "v3_late"), (HERE, "v3_late_s42")), "all": ((HERE, "v3"), (HERE, "v3_s42"))}
RUN_DIRS = {"v3_late": "gemma4-12b-v3-late", "v3_late_s42": "gemma4-12b-v3-late-s42", "v3": "gemma4-12b-v3",
            "v3_s42": "gemma4-12b-v3-s42"}
# The same all-layer weights answered in the c1 session: reproduction and seed noise only.
C1_ANSWERS = {"v3": (C1, "v3_c1s"), "v3_s42": (C1, "v3_s42_c1s")}
SESSION_VERSION = HERE / "session_server_version.txt"
TASKS = 1026
RESAMPLES = 10_000
MARGIN = 0.02  # 2.0 points


def load(where: pathlib.Path, tag: str) -> dict:
    path = where / f"report__filter_{tag}.json.gz"
    if not path.exists():
        path = where / f"filter_{tag}.json"
    with (gzip.open(path, "rt") if path.suffix == ".gz" else open(path)) as handle:
        report = json.load(handle)
    return {r["task_id"]: r["attempts"][0] for r in report["destinations"][0]["variants"][0]["results"]}


def sign_test(a: int, b: int) -> float:
    n, k = a + b, min(a, b)
    return min(1.0, 2 * sum(math.comb(n, i) for i in range(k + 1)) / 2 ** n) if n else 1.0


def interval(diffs: list[float]) -> tuple[float, float, float]:
    rng = random.Random(42)
    n = len(diffs)
    means = sorted(sum(rng.choices(diffs, k=n)) / n for _ in range(RESAMPLES))
    return sum(diffs) / n, means[int(0.025 * RESAMPLES)], means[int(0.975 * RESAMPLES) - 1]


def seed_check(mean: float, pair_totals: list[int], seed_only: tuple[float, float, float]) -> tuple[bool, str]:
    """The seed check of ../../distill/c1-20261118/README.md, unchanged: a difference in one direction counts only
    if every run of one arm beats every run of the other in that direction, and the arm difference is larger in
    size than either end of the seed-only contrast's interval."""
    direction = 1 if mean > 0 else -1
    ordered = all(direction * total > 0 for total in pair_totals)
    reach = max(abs(seed_only[1]), abs(seed_only[2]))
    larger = abs(mean) > reach
    why = []
    if not ordered:
        why.append("the four run-against-run totals do not all point this way")
    if not larger:
        why.append(f"the difference is within the seed-only contrast's reach ({100 * reach:.2f} points)")
    return ordered and larger, "; ".join(why)


def reading(low: float, high: float, clear: bool, why: str) -> str:
    """README.md's table, row by row."""
    if low > 0:
        if clear:
            return "late-only is better: it is the candidate recipe for the next 12B run; the owner decides"
        return (f"no loss shown (a gain not separable from seed noise: {why}): late-only may replace the all-layer "
                "recipe for its lower cost; the owner decides")
    if high < 0:
        if clear:
            return "late-only is worse: keep every layer"
        return f"a loss not separable from seed noise ({why}): no change; the owner decides on more seeds"
    if low >= -MARGIN:
        return (f"no loss shown at a {100 * MARGIN:.1f}-point margin: late-only may replace the all-layer recipe for "
                "its lower cost; the owner decides")
    return f"not resolved (the interval reaches below -{100 * MARGIN:.1f} points): no change"


def trainable(tag: str) -> str:
    path = RUNS / RUN_DIRS[tag] / "train" / "run_metadata.json"
    if not path.is_file():
        return "no run_metadata.json"
    coverage = json.loads(path.read_text(encoding="utf-8")).get("lora_coverage") or {}
    modules = sum((coverage.get("lora_modules") or {}).values())
    return f"{coverage.get('trainable_params', 0):,} trainable parameters, {modules} LoRA modules"


def main() -> None:
    if not SESSION_VERSION.is_file() or not SESSION_VERSION.read_text(encoding="utf-8").strip():
        raise SystemExit(f"STOP: no {SESSION_VERSION.name}: the four session passes must record their one server version")
    print(f"session: {'; '.join(SESSION_VERSION.read_text(encoding='utf-8').split(chr(10))).strip('; ')}")
    keys = [t["id"] for t in json.loads((FV3 / "tasks_frozen.json").read_text(encoding="utf-8"))["tasks"]]
    if len(keys) != TASKS or len(set(keys)) != TASKS:
        raise SystemExit(f"STOP: expected {TASKS} unique fv3 tasks, found {len(keys)}")
    runs = {tag: load(where, tag) for members in ARMS.values() for where, tag in members}
    for tag, results in runs.items():
        if set(results) != set(keys):
            raise SystemExit(f"STOP: {tag} does not cover the {TASKS} tasks ({len(results)} results)")
    failure = {t: {k: distill.filter_failure_class(r[k].get("run") or {}) for k in keys} for t, r in runs.items()}
    passed = {t: {k: failure[t][k] is None for k in keys} for t in runs}

    print(f"first attempt, {TASKS} tasks: passed, failed, coded / return code / wrong output")
    for tag in runs:
        classes = [c for c in failure[tag].values() if c]
        print(f"  {tag:12s} {TASKS - len(classes):5d} {len(classes):5d}  "
              f"{classes.count('coded')} / {classes.count('rc')} / {classes.count('wrong')}")

    arm = {name: {k: sum(passed[t][k] for _, t in members) / len(members) for k in keys} for name, members in ARMS.items()}

    def runs_diff(a: str, b: str) -> str:
        mean, low, high = interval([passed[a][k] - passed[b][k] for k in keys])
        gained = sum(1 for k in keys if passed[a][k] and not passed[b][k])
        lost = sum(1 for k in keys if passed[b][k] and not passed[a][k])
        return (f"{100 * mean:+.2f} points [{100 * low:+.2f}, {100 * high:+.2f}], gained {gained}, lost {lost}, "
                f"sign test p={sign_test(gained, lost):.3g}")

    (_, l1), (_, l2) = ARMS["late"]
    (_, a1), (_, a2) = ARMS["all"]
    # A contrast with one run of each arm on each side, {v3_late, v3_s42} minus {v3_late_s42, v3}: no arm effect.
    seed_only = interval([(passed[l1][k] + passed[a2][k]) / 2 - (passed[l2][k] + passed[a1][k]) / 2 for k in keys])
    pair_totals = [sum(passed[l].values()) - sum(passed[a].values()) for l in (l1, l2) for a in (a1, a2)]
    mean, low, high = interval([arm["late"][k] - arm["all"][k] for k in keys])
    clear, why = seed_check(mean, pair_totals, seed_only)
    print(f"\nTHE READING: late minus all, two seeds each, {TASKS} tasks\n"
          f"  {100 * mean:+.2f} points [{100 * low:+.2f}, {100 * high:+.2f}]")
    print(f"  seed check: seed-only contrast, {{{l1}, {a2}}} minus {{{l2}, {a1}}}: {100 * seed_only[0]:+.2f} points "
          f"[{100 * seed_only[1]:+.2f}, {100 * seed_only[2]:+.2f}]; late-run minus all-run totals "
          f"{', '.join(f'{total:+d}' for total in pair_totals)}")
    print(f"  -> {reading(low, high, clear, why)}")

    print("\nreported, not deciding")
    print("  seed noise, tasks on which an arm's two seeds differ:")
    for name, ((_, a), (_, b)) in ARMS.items():
        print(f"    {name}: {sum(1 for k in keys if passed[a][k] != passed[b][k])}")
    c1_pass = {tag: {k: distill.filter_failure_class(r[k].get("run") or {}) is None for k in keys}
               for tag, r in ((t, load(w, f)) for t, (w, f) in C1_ANSWERS.items())}
    print(f"    all, the same weights in the c1 session: {sum(1 for k in keys if c1_pass['v3'][k] != c1_pass['v3_s42'][k])}")
    for name, ((_, a), (_, b)) in ARMS.items():
        print(f"  seed gap, {name}: {a} minus {b}: {runs_diff(a, b)}")
    for l in (l1, l2):
        for a in (a1, a2):
            print(f"  {l} minus {a}: {runs_diff(l, a)}")
    c1_server = (C1 / "session_server_version.txt").read_text(encoding="utf-8").strip()
    print("  all-layer runs here against their int8 answers in the c1 session on the scoring Mac "
          f"(Ollama {c1_server}; across server, precision and batching):")
    for tag, (where, first) in C1_ANSWERS.items():
        before = load(where, first)
        same_program = sum(1 for k in keys if (runs[tag][k].get("source_code") or "") == (before[k].get("source_code") or ""))
        same_class = sum(1 for k in keys if passed[tag][k] == c1_pass[tag][k])
        print(f"    {tag}: same first program on {same_program} of {TASKS}, same pass or fail on {same_class}; "
              f"passed {sum(passed[tag].values())} now, {sum(c1_pass[tag].values())} then")
    print("  cost:")
    for tag in runs:
        print(f"    {tag}: {trainable(tag)}")


if __name__ == "__main__":
    main()
