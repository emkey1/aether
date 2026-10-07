#!/usr/bin/env python3
"""Audit the LLM-facing guides for content a model should never read.

Run by CTest as `aether_guide_audit`, in REPORT mode until the first guide pass
(it prints every hit and exits 0; `--strict` exits 1 on any hit, which is what
the guide pass switches the CTest to).

The constrained guides (medium and small) are held to the four zero-hit
categories in docs/aether_doc_maintenance.md ("LLM-applicable content only"):

  provenance  PSCAL, Rea, the VM, yyjson, curl, opcodes, bytecode, "lowers to"
              wording, Pascal ancestry
  build       CMake and its -D / AETHER_ENABLE_* flags, ./build, ctest,
              "run the compiler"
  history     historically, used to, we chose, heritage, on newer/older builds
  meta        docs/, README, examples/, benchmark, the other guides by name

All three guides, the full one included, are checked for:

  literal     the copied template string "yyjson unavailable"
  bench       benchmark identifiers (task ids, module and data file names, mod
              and export names) listed in tools/bench_identifiers.txt

ALLOWLIST keeps lines that hit a term but are deliberate, per guide: the Pascal
naming mnemonics (`arctan` not `atan`), the Pascal-style File handle model, and
the runtime-discovery sentences that must say a one-shot generator cannot run
the compiler.

The identifier list is regenerated from the umbrella's task suites, never read
from it at test time:

    PSCAL_UMBRELLA=~/git/pscal python3 tools/audit_guides.py --refresh

Usage:
    python3 tools/audit_guides.py [--strict] [GUIDE ...]
Exit status: 0 report mode (or strict with no hits), 1 strict with hits, 2 error.
"""
import argparse
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
IDENTIFIERS = os.path.join(HERE, "bench_identifiers.txt")
GUIDES = {
    "aether_for_llms_and_others.md": "full",
    "aether_for_llms_medium_contexts.md": "medium",
    "aether_for_llms_with_small_contexts.md": "small",
    "aether_card.md": "card",
}
CONSTRAINED = {"medium", "small", "card"}
SUITES = ("tasks_v2_pos", "tasks_hard_v2", "tasks_cs", "tasks_hard_nontoon", "tasks")

TERMS = {
    "provenance": [
        (r"\bpscal(?:-core)?\b", re.I), (r"\b[Rr]ea\b", 0), (r"\bVM\b", 0),
        (r"yyjson", re.I), (r"\b(?:lib)?curl\b", re.I), (r"\bopcodes?\b", re.I),
        (r"\bbytecode\b", re.I), (r"\blower(?:s|ed|ing)?\s+(?:to|into)\b", re.I),
        (r"\blowering\b", re.I), (r"\bPascal\b", 0),
    ],
    "build": [
        (r"\bcmake\b", re.I), (r"(?<![\w-])-D[A-Z][A-Z0-9_]+", 0), (r"\bAETHER_ENABLE_\w+", 0),
        (r"\./build\b", 0), (r"\bctest\b", re.I), (r"\brun the compiler\b", re.I),
    ],
    "history": [
        (r"\bhistorically\b", re.I), (r"\bused to\b", re.I), (r"\bwe chose\b", re.I),
        (r"\bheritage\b", re.I), (r"\b(?:newer|older) builds?\b", re.I),
    ],
    "meta": [
        (r"\bdocs/", 0), (r"\bREADME\b", 0), (r"\b[Ee]xamples/", 0), (r"\bbenchmark", re.I),
        (r"\b(?:full|medium|small|concise|other) guides?\b", re.I),
        (r"aether_for_llms_", 0),
    ],
}
LITERAL = re.compile(r"yyjson unavailable")

# (guide, distinctive substring of the line) -> why the hit is kept.
ALLOWLIST = {
    ("medium", "Names follow Pascal conventions"): "naming mnemonic (`arctan` not `atan`)",
    ("small", "Pascal naming: **`arctan`** not `atan`"): "naming mnemonic",
    ("small", "(Pascal-style handle;"): "the assign/reset File model",
    ("small", "Agentic only: you must run the compiler"):
        "runtime-discovery advice must say a one-shot generator cannot run it",
    ("small", "Cannot run the compiler? Stay within this guide"):
        "runtime-discovery advice must say a one-shot generator cannot run it",
}


def load_identifiers(path):
    out = []
    try:
        with open(path, encoding="utf-8") as fh:
            for line in fh:
                tok = line.split("#", 1)[0].strip()
                if tok:
                    out.append(tok)
    except FileNotFoundError:
        pass
    return out


def distinctive(name):
    """Drop plain words (cube, Base, rows.json) that would match ordinary prose."""
    stem = name.rsplit(".", 1)[0] if "." in name else name
    return bool(re.search(r"[_\d]", stem) or re.search(r"[a-z][A-Z]", stem))


def refresh(umbrella):
    bench = os.path.join(umbrella, "Tests", "aether_doc_bench")
    names = set()
    for suite in SUITES:
        with open(os.path.join(bench, suite + ".json"), encoding="utf-8") as fh:
            tasks = json.load(fh)["tasks"]
        for t in tasks:
            names.add(t["id"])
            files = t.get("files") or {}
            if not isinstance(files, dict):
                continue
            for fname, body in files.items():
                names.add(fname)
                names.update(re.findall(r"\bmod\s+([A-Za-z_]\w*)", body))
                names.update(re.findall(r"\bexport\s+(?:fn|const|type)\s+([A-Za-z_]\w*)", body))
    kept = sorted(n for n in names if distinctive(n))
    sha = subprocess.run(["git", "-C", umbrella, "rev-parse", "--short", "HEAD"],
                         capture_output=True, text=True).stdout.strip() or "unknown"
    with open(IDENTIFIERS, "w", encoding="utf-8") as fh:
        fh.write("# Benchmark identifiers the guides must not carry (tools/audit_guides.py).\n")
        fh.write("# Generated by `PSCAL_UMBRELLA=... python3 tools/audit_guides.py --refresh`\n")
        fh.write(f"# from Tests/aether_doc_bench/{{{','.join(SUITES)}}}.json at umbrella "
                 f"{sha}:\n")
        fh.write("# task ids, task file names, and the mod / export names in task modules,\n")
        fh.write("# minus plain words (cube, Base, rows.json). Do not hand-edit.\n")
        for n in kept:
            fh.write(n + "\n")
    print(f"wrote {len(kept)} identifiers ({len(names) - len(kept)} plain words dropped) "
          f"to {os.path.relpath(IDENTIFIERS, REPO)}")


def audit(path, identifiers):
    role = GUIDES.get(os.path.basename(path), "full")
    with open(path, encoding="utf-8") as fh:
        lines = fh.read().split("\n")
    bench_rx = [re.compile(r"(?<![\w.])" + re.escape(i) + r"(?![\w])") for i in identifiers]
    hits, kept = [], []
    for no, line in enumerate(lines, 1):
        found = []
        if LITERAL.search(line):
            found.append(("literal", "yyjson unavailable"))
        for rx, ident in zip(bench_rx, identifiers):
            n = len(rx.findall(line))
            found.extend([("bench", ident)] * n)
        if role in CONSTRAINED and not line.startswith("# "):  # the guide's own title
            for cat, terms in TERMS.items():
                for pat, flags in terms:
                    m = re.search(pat, line, flags)
                    if m and not (cat == "provenance" and pat == "yyjson" and LITERAL.search(line)):
                        found.append((cat, m.group(0)))
        if not found:
            continue
        allow = next((why for (g, s), why in ALLOWLIST.items() if g == role and s in line), None)
        if allow and all(c not in ("bench", "literal") for c, _ in found):
            kept.append((no, found, allow))
        else:
            hits.append((no, found, line.strip()))
    return role, hits, kept


def main():
    ap = argparse.ArgumentParser(description="Audit the guides for non-LLM content.")
    ap.add_argument("guides", nargs="*")
    ap.add_argument("--strict", action="store_true", help="exit 1 on any hit")
    ap.add_argument("--refresh", action="store_true",
                    help="regenerate tools/bench_identifiers.txt from $PSCAL_UMBRELLA")
    args = ap.parse_args()

    if args.refresh:
        umbrella = os.environ.get("PSCAL_UMBRELLA")
        if not umbrella or not os.path.isdir(umbrella):
            print("ERROR: --refresh needs PSCAL_UMBRELLA pointing at an umbrella checkout")
            return 2
        refresh(umbrella)
        return 0

    identifiers = load_identifiers(IDENTIFIERS)
    if not identifiers:
        print(f"ERROR: no identifiers in {IDENTIFIERS}")
        return 2
    paths = args.guides or [os.path.join(REPO, "docs", g) for g in GUIDES]
    total = 0
    for path in paths:
        role, hits, kept = audit(path, identifiers)
        n = sum(len(f) for _, f, _ in hits)
        total += len(hits)
        by_cat = {}
        for _, found, _ in hits:
            for cat, _ in found:
                by_cat[cat] = by_cat.get(cat, 0) + 1
        cats = ", ".join(f"{c} {k}" for c, k in sorted(by_cat.items())) or "none"
        print(f"{role} ({os.path.basename(path)}): {len(hits)} lines, {n} hits ({cats}); "
              f"{len(kept)} allowlisted")
        for no, found, line in hits:
            terms = ", ".join(f"{c}: {t}" for c, t in found)
            print(f"  {role}:{no}  [{terms}]  {line[:130]}")
    mode = "STRICT" if args.strict else "REPORT MODE (exit 0; --strict fails on any hit)"
    print(f"guide audit: {total} lines with hits across {len(paths)} guides. {mode}")
    return 1 if (args.strict and total) else 0


if __name__ == "__main__":
    sys.exit(main())
