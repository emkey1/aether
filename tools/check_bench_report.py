#!/usr/bin/env python3
"""Refuse a bench report that did not measure what it claims (used by tools/score_ollama_model.sh).

    check_bench_report.py REPORT.json [--leak PREFIX ...] [--complete] [--guide NAME=SHA256 | --no-guide]

Fails when every generation is empty, when any first answer starts with a reasoning-leak prefix (case-insensitive),
with --complete when any case went unmeasured (infrastructure failure), with --guide when a variant is not that guide
(name and sha256) or carries no guide tokens, and with --no-guide when a variant carries a guide. Prints one line of
counts either way.
"""
from __future__ import annotations

import argparse
import json
import sys


# Case lists a variant can carry, by key prefix: Aether, then the Python and Rust baselines.
CASE_PREFIXES = ("", "python_baseline_", "rust_baseline_")


def first_source(result: dict) -> str:
    attempts = result.get("attempts") or []
    return (attempts[0].get("source_code") if attempts else None) or result.get("source_code") or ""


def check(report: dict, leaks: list[str], complete: bool, guide: tuple[str, str] | None, no_guide: bool) -> list[str]:
    problems = []
    variants = [v for d in report.get("destinations") or [] for v in d.get("variants") or []]
    if not variants:
        return ["no variants in the report"]
    prefixes = tuple(p.lower() for p in leaks)
    for v in variants:
        name = v.get("doc_name") or "none"
        measured = 0
        for prefix in CASE_PREFIXES:
            if f"{prefix}results" not in v:
                continue
            results = v.get(f"{prefix}results") or []
            measured += len(results)
            label = f"{name}/{prefix.rstrip('_') or 'aether'}"
            empty = sum(1 for r in results if not first_source(r).strip())
            leak = sum(1 for r in results if prefixes and first_source(r).lstrip().lower().startswith(prefixes))
            summary = v.get(f"{prefix}summary") or {}
            infra = summary.get("infra_failed", 0)
            print(f"check {label}: cases={summary.get('total_cases', len(results))} "
                  f"FA={summary.get('first_attempt_exact')} FX={summary.get('final_exact')} "
                  f"empty={empty} leak={leak} infra={infra}")
            if results and empty == len(results):
                problems.append(f"{label}: every generation is empty")
            if leak:
                problems.append(f"{label}: {leak} answers start with a reasoning trace")
            if complete and infra:
                problems.append(f"{label}: {infra} cases unmeasured (infrastructure)")
        if measured == 0:
            problems.append(f"{name}: no cases in the report")
        if guide is not None:
            want_name, want_sha = guide
            if name != want_name or v.get("doc_sha256") != want_sha:
                problems.append(f"{name}: expected guide {want_name} sha256 {want_sha[:12]}, report has "
                                f"{name} sha256 {str(v.get('doc_sha256'))[:12]}")
            if not (v.get("doc_tokens_o200k") or v.get("doc_approx_tokens")):
                problems.append(f"{name}: the guide carries no tokens")
        if no_guide and name not in ("none", ""):
            problems.append(f"{name}: a guide was sent where none was asked for")
    if complete and report.get("infra_failed"):
        problems.append("the report records infrastructure failures")
    return problems


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("report")
    ap.add_argument("--leak", action="append", default=[])
    ap.add_argument("--complete", action="store_true")
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--guide", help="NAME=SHA256 every variant must carry")
    g.add_argument("--no-guide", action="store_true")
    args = ap.parse_args(argv)
    try:
        report = json.load(open(args.report, encoding="utf-8"))
    except (OSError, ValueError) as exc:
        print(f"check: cannot read {args.report}: {exc}")
        return 1
    guide = tuple(args.guide.split("=", 1)) if args.guide else None
    if guide is not None and len(guide) != 2:
        ap.error("--guide takes NAME=SHA256")
    problems = check(report, args.leak, args.complete, guide, args.no_guide)
    for p in problems:
        print(f"check: {p}")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
