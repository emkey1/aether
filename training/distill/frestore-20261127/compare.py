#!/usr/bin/env python3
"""The reading fixed in README.md: gemma4-12b-v3 with its layers 0-15 restored to the base (r16), against v3, in
Python and in Aether on fv3's 1,026 tasks. Run from the repo root.

Reads, beside this file, the session's reports report__{python,aether}_<tag>.json.gz (or {python,aether}_<tag>.json)
and session_server_version.txt. Reported, not deciding: e16 (only layers 0-15 changed) against the base, the late-only
LoRA run, failure classes, Aether constructs in failed Python programs, and v3's Aether answers here against the same
weights' answers in gemma4-12b-late's training-host session, when that report is present. Prints no task id or program.
"""

from __future__ import annotations

import gzip
import json
import pathlib
import random
import re
import sys

sys.path.insert(0, "tools")
import aether_distill as distill  # noqa: E402

HERE = pathlib.Path(__file__).resolve().parent
FV3 = HERE.parent / "fv3-20261105"
LATE = HERE.parent.parent / "training_runs" / "gemma4-12b-late"
SESSION_VERSION = HERE / "session_server_version.txt"
TASKS = 1026
RESAMPLES = 10_000
MARGIN = 0.02  # 2.0 points, the margin of ../../training_runs/gemma4-12b-late/README.md
PYTHON_TAGS = ("base", "v3", "r16", "e16", "v3_late")
AETHER_TAGS = ("v3", "r16")
# Aether written into a Python program, fixed before any program was read (README.md).
MARKERS = {
    "in a..b": re.compile(r"(?m)^[^#\n]*\bin\s+[^#\"'\n]*?[\w)\]]\.\.(?!\.)[\w(]"),  # not in a comment
    "TOON builtin": re.compile(r"\b(?:toon_\w+|has_toon)\s*\("),
    "println": re.compile(r"\bprintln\s*\("),
    "fx block": re.compile(r"\bfx\s*\{"),
    "fn / let x: / loop": re.compile(r"(?m)^\s*(?:fn\s+\w+\s*\(|let\s+\w+\s*:|loop\s+\w+\s+in\b)"),
}


def load(kind: str, tag: str, where: pathlib.Path = HERE, name: str | None = None) -> dict | None:
    stem = name or f"{kind}_{tag}"
    for path in (where / f"report__{stem}.json.gz", where / f"{stem}.json"):
        if path.exists():
            with (gzip.open(path, "rt") if path.suffix == ".gz" else open(path)) as handle:
                report = json.load(handle)
            variant = report["destinations"][0]["variants"][0]
            results = variant["python_baseline_results"] if kind == "python" else variant["results"]
            return {r["task_id"]: r["attempts"][0] for r in results}
    return None


def python_class(run: dict) -> str | None:
    """None for a pass; otherwise error (an exit code other than the expected one) or wrong (wrong output)."""
    if run.get("exact_stdout_match") and run.get("returncode") == run.get("expected_returncode"):
        return None
    return "wrong" if run.get("returncode") in (None, run.get("expected_returncode")) else "error"


def interval(diffs: list[float]) -> tuple[float, float, float]:
    rng = random.Random(42)
    n = len(diffs)
    means = sorted(sum(rng.choices(diffs, k=n)) / n for _ in range(RESAMPLES))
    return sum(diffs) / n, means[int(0.025 * RESAMPLES)], means[int(0.975 * RESAMPLES) - 1]


def share(num: list[float], den: list[float]) -> tuple[float, float, float] | None:
    """The share of the gap den that num recovers, with a paired bootstrap over the same resampled tasks."""
    n = len(num)
    if sum(den) <= 0:
        return None
    rng = random.Random(42)
    ratios = []
    for _ in range(RESAMPLES):
        idx = rng.choices(range(n), k=n)
        d = sum(den[i] for i in idx)
        ratios.append(sum(num[i] for i in idx) / d if d > 0 else float("nan"))
    ratios = sorted(r for r in ratios if r == r)
    m = len(ratios)
    return sum(num) / sum(den), ratios[int(0.025 * m)], ratios[int(0.975 * m) - 1]


def pts(t: tuple[float, float, float]) -> str:
    return f"{100 * t[0]:+.2f} points [{100 * t[1]:+.2f}, {100 * t[2]:+.2f}]"


def python_reading(low: float, high: float) -> str:
    if low > 0:
        return "restoring layers 0-15 recovers part of v3's Python"
    if high < 0:
        return "restoring layers 0-15 makes v3's Python worse"
    return "no Python recovery shown"


def aether_reading(low: float, high: float) -> str:
    if low > 0:
        return "better at Aether than v3"
    if low >= -MARGIN:
        return f"keeps Aether: no loss beyond {100 * MARGIN:.1f} points"
    if high < 0:
        return "costs Aether"
    return f"Aether not resolved (the interval reaches below -{100 * MARGIN:.1f} points)"


def main() -> None:
    if not SESSION_VERSION.is_file() or not SESSION_VERSION.read_text(encoding="utf-8").strip():
        raise SystemExit("STOP: no session_server_version.txt: the session's passes must record their one setup")
    print(f"session: {'; '.join(SESSION_VERSION.read_text(encoding='utf-8').split(chr(10))).strip('; ')}")
    keys = [t["id"] for t in json.loads((FV3 / "tasks_frozen.json").read_text(encoding="utf-8"))["tasks"]]
    if len(keys) != TASKS or len(set(keys)) != TASKS:
        raise SystemExit(f"STOP: expected {TASKS} unique fv3 tasks, found {len(keys)}")
    py = {tag: load("python", tag) for tag in PYTHON_TAGS}
    ae = {tag: load("aether", tag) for tag in AETHER_TAGS}
    for kind, runs in (("python", py), ("aether", ae)):
        for tag, results in runs.items():
            if results is None or set(results) != set(keys):
                raise SystemExit(f"STOP: {kind} {tag} does not cover the {TASKS} tasks")
    py_class = {t: {k: python_class(r[k].get("run") or {}) for k in keys} for t, r in py.items()}
    ae_class = {t: {k: distill.filter_failure_class(r[k].get("run") or {}) for k in keys} for t, r in ae.items()}
    py_pass = {t: {k: c[k] is None for k in keys} for t, c in py_class.items()}
    ae_pass = {t: {k: c[k] is None for k in keys} for t, c in ae_class.items()}

    print(f"\nfirst attempt, {TASKS} tasks")
    print("  Python: passed, failed: error / wrong output")
    for tag in PYTHON_TAGS:
        classes = [c for c in py_class[tag].values() if c]
        print(f"    {tag:8s} {TASKS - len(classes):5d}  {classes.count('error')} / {classes.count('wrong')}")
    print("  Aether: passed, failed: coded / return code / wrong output")
    for tag in AETHER_TAGS:
        classes = [c for c in ae_class[tag].values() if c]
        print(f"    {tag:8s} {TASKS - len(classes):5d}  {classes.count('coded')} / {classes.count('rc')} / {classes.count('wrong')}")

    def diff(passed: dict, a: str, b: str) -> tuple[float, float, float]:
        return interval([passed[a][k] - passed[b][k] for k in keys])

    def counts(passed: dict, a: str, b: str) -> str:
        gained = sum(1 for k in keys if passed[a][k] and not passed[b][k])
        lost = sum(1 for k in keys if passed[b][k] and not passed[a][k])
        return f"gained {gained}, lost {lost}"

    py_r = diff(py_pass, "r16", "v3")
    gap = diff(py_pass, "base", "v3")
    recovered = share([py_pass["r16"][k] - py_pass["v3"][k] for k in keys], [py_pass["base"][k] - py_pass["v3"][k] for k in keys])
    ae_r = diff(ae_pass, "r16", "v3")
    py_read, ae_read = python_reading(py_r[1], py_r[2]), aether_reading(ae_r[1], ae_r[2])
    print("\nTHE READING")
    print(f"  Python, r16 minus v3: {pts(py_r)}, {counts(py_pass, 'r16', 'v3')}  -> {py_read}")
    print(f"    the gap it is read against, base minus v3: {pts(gap)}")
    if recovered:
        print(f"    share of that gap recovered: {100 * recovered[0]:.0f}% [{100 * recovered[1]:.0f}%, {100 * recovered[2]:.0f}%]")
    print(f"  Aether, r16 minus v3: {pts(ae_r)}, {counts(ae_pass, 'r16', 'v3')}  -> {ae_read}")
    candidate = py_r[1] > 0 and ae_r[1] >= -MARGIN
    print("  -> " + ("r16 is a candidate: the same edit is answered at int8 on the scoring Mac (the card's eight Python suites "
                     "and fv3's Aether) before the owner decides anything about a release" if candidate else
                     "no change: v3 stays as it is"))

    print("\nreported, not deciding")
    print(f"  where the Python damage sits: e16 (only layers 0-15 changed) minus base {pts(diff(py_pass, 'e16', 'base'))}, "
          f"{counts(py_pass, 'e16', 'base')}")
    print(f"  r16 minus base, Python: {pts(diff(py_pass, 'r16', 'base'))}")
    print(f"  the late-only LoRA run, Python: v3_late minus v3 {pts(diff(py_pass, 'v3_late', 'v3'))}, "
          f"{counts(py_pass, 'v3_late', 'v3')}; minus base {pts(diff(py_pass, 'v3_late', 'base'))}")
    print("  failed first Python programs carrying Aether (any marker; then by marker):")
    for tag in PYTHON_TAGS:
        failed = [py[tag][k].get("source_code") or "" for k in keys if not py_pass[tag][k]]
        hits = {name: sum(1 for src in failed if rx.search(src)) for name, rx in MARKERS.items()}
        anyhit = sum(1 for src in failed if any(rx.search(src) for rx in MARKERS.values()))
        print(f"    {tag:8s} {anyhit:4d} of {len(failed):4d}  " + ", ".join(f"{n} {c}" for n, c in hits.items()))
    host_v3 = load("aether", "v3", LATE, "host_v3")
    if host_v3 is None or set(host_v3) != set(keys):
        print("  v3's Aether against gemma4-12b-late's training-host session: that report is not here")
    else:
        same_program = sum(1 for k in keys if (ae["v3"][k].get("source_code") or "") == (host_v3[k].get("source_code") or ""))
        before = {k: distill.filter_failure_class(host_v3[k].get("run") or {}) is None for k in keys}
        same_class = sum(1 for k in keys if ae_pass["v3"][k] == before[k])
        print(f"  v3's Aether against gemma4-12b-late's training-host session (same image, grader and workers): same first "
              f"program on {same_program} of {TASKS}, same pass or fail on {same_class}; passed {sum(ae_pass['v3'].values())} "
              f"now, {sum(before.values())} then")


if __name__ == "__main__":
    main()
