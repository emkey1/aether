#!/usr/bin/env python3
"""Compile-check every ```aether block in the LLM-facing guides.

Usage:
    python3 tools/verify_guide_snippets.py docs/aether_for_llms_*.md     # all three
    python3 tools/verify_guide_snippets.py docs/aether_for_llms_medium_contexts.md
    python3 tools/verify_guide_snippets.py --keys-only docs/aether_for_llms_*.md

Every block is compiled with `aether --no-cache --no-run --diagnostics-json`
($AETHER_BIN, default build/aether). Fragments are wrapped in a function over
the CONTEXT prelude below, and declaration-only blocks get a trivial `main`.
The run fails (exit 1) on any of:

  * a block that does not compile and matches no EXPECT_FAIL key;
  * an EXPECT_FAIL block that compiles, or fails with a set of diagnostic codes
    other than the one its entry names, or emits any record whose code is null
    (CODE_CHECK_REPORT_ONLY lists the keys whose code check is printed but not
    yet enforced);
  * a fragment that compiles only after the gate wraps it in `fx { }` -- the
    model copies the block as written -- unless FX_RESCUED allowlists it;
  * an EXPECT_FAIL key that matches no block in any guide. The keys are shared
    by all three guides, so this runs when all three are given (or alone, with
    --keys-only, as the CTest aether_guide_snippet_keys does).

See docs/aether_doc_maintenance.md for why this exists.
"""
import argparse
import json
import os
import re
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
AETHER = os.environ.get("AETHER_BIN", os.path.join(REPO, "build", "aether"))
GUIDE_NAMES = ("aether_for_llms_and_others.md", "aether_for_llms_medium_contexts.md",
               "aether_for_llms_with_small_contexts.md")

# Blocks that MUST fail, keyed by a distinctive substring of the block. The
# value is (why, the exact set of codes the block must fail with). One dict
# serves all three guides, so keep keys specific: a key broad enough to match a
# good block in another guide would hide a real regression there.
EXPECT_FAIL = {
    'use "bench_support";': (
        "imports a module that is not on disk here", {"SCOPE-001"}),
    'WRONG — free-standing method': (
        "deliberate WRONG/RIGHT contrast; the WRONG half must not compile", {"SCOPE-001"}),
    'if status == "ready" { ... }': (
        "prose block using ... elision, not real source", {"SYN-001"}),
    'let name: Text = toon_get_text(doc, "name");': (
        "negative example: getter on a ToonDoc", {"TOON-001"}),
    'xs = doubleAll(xs);': (
        "prose block mixing a declaration with a bare call statement", {"SCOPE-001"}),
    'let user: ToonNode = toon_at(root, i);': (
        "shows both root shapes side by side, so `root` is declared twice", {"NAME-001"}),
    'export fn clampSupport(score: Int) -> Int { ... }': (
        "module sketch using ... elision, not real source", {"SYN-001"}),
    'let value: Int = answer();': (
        "calls an export from a module that is not on disk here", {"SCOPE-001"}),
    'if name == "Aether" { ... }': (
        "prose block using ... elision, not real source", {"SYN-001"}),
}

# EXPECT_FAIL keys whose code check is reported, not enforced, with the reason.
CODE_CHECK_REPORT_ONLY = {
    'WRONG — free-standing method': (
        "the WRONG and RIGHT halves share one fence, so the RIGHT half's in-type `get` "
        "collides with the WRONG half's extension method and adds an uncoded "
        "'Duplicate method' record (code null, line 0); enforced once the guide pass "
        "splits the fence"),
}

# Fragments that compile only inside an fx block the guide does not show, keyed
# by (guide, distinctive substring). They break rule 1 in exactly the shape a
# model copies; the guide pass wraps them and deletes the fx retry with this list.
FX_RESCUED = {
    ("aether_for_llms_with_small_contexts.md",
     'println("status: ", if ready { "ready" } else { "blocked" });'):
        "bare println in the inline-if example",
    ("aether_for_llms_with_small_contexts.md",
     'loop i in 0..length(s) { print(s[i]); }   // every character, exactly once'):
        "bare print in the per-character loop",
    ("aether_for_llms_with_small_contexts.md", 'print(xs[i], " ");'):
        "bare print/println in the array-printing loop",
}

# Names the guides' prose fragments reference without declaring. Kept in one
# place so a fragment that legitimately uses a new name is a one-line fix here
# rather than a reason to stop checking that block.
CONTEXT = """
type Ctx { label: Text = ""; }
type Point { x: Int = 0; y: Int = 0; }
type Counter { value: Int = 0; }
type Tx { id: Text = ""; amount: Int = 0; }

fn workerA() -> Void { ret; }
fn workerB() -> Void { ret; }
// The record type is deliberately NOT named after the function that takes it.
// Aether identifiers are case-insensitive, so a `type Tally` beside a `fn tally`
// is the exact name collision that used to make `new Tally()` dispatch to the
// function; this prelude should not model a shape the guides steer writers away
// from, even though the compiler now resolves it correctly.
type TallyRec { count: Int = 0; }
fn tally(t: TallyRec, upTo: Int) -> Void { ret; }
fn doubleAll(arr: Int[]) -> Int[] { ret arr; }

fn __frag(ready: Bool, score: Int, index: Int, total: Int, count: Int,
          j: Int, id: Text, pct: Real, s: Text, url: Text, n: Int,
          rows: Int, cols: Int, path: Text, value: Int, answer: Int,
          successful: Int, amount: Int, name: Text, status: Text, ok: Int,
          raw: Text, code: Int, table: Int[][],
          a: TallyRec, b: TallyRec,
          xs: Int[], items: Int[], ys: Int[], root: ToonNode, row: ToonNode, doc: ToonDoc,
          tx: Tx) -> Void {
"""
MAIN = "\nfn main() -> Void {\n    ret;\n}\n"


def parse_blocks(path):
    with open(path, encoding="utf-8") as fh:
        lines = fh.read().split("\n")
    blocks, cur, start = [], None, 0
    for i, l in enumerate(lines, 1):
        if l.strip() == "```aether" and cur is None:
            cur, start = [], i
        elif l.strip() == "```" and cur is not None:
            blocks.append((start, "\n".join(cur)))
            cur = None
        elif cur is not None:
            cur.append(l)
    return blocks


def wrap(src):
    if re.search(r"\bfn\s+main\s*\(", src):
        return "whole", src
    if re.search(r"^\s*(fn|type|mod|use|const|@)", src, re.M):
        return "decl", src + "\n" + MAIN
    body = "\n".join("    " + x for x in src.split("\n"))
    return "frag", CONTEXT + body + "\n    ret;\n}\n" + MAIN


def wrap_fx(src):
    body = "\n".join("        " + x for x in src.split("\n"))
    return CONTEXT + "    fx {\n" + body + "\n    }\n    ret;\n}\n" + MAIN


def parse_records(stderr):
    """The --diagnostics-json array, or one uncoded record holding the raw text."""
    start = stderr.find("[\n")
    end = stderr.rfind("]")
    if start != -1 and end > start:
        try:
            recs = json.loads(stderr[start:end + 1])
            if isinstance(recs, list):
                return recs
        except ValueError:
            pass
    text = stderr.strip()
    return [{"code": None, "line": None, "raw": text, "message": text}] if text else []


def compile_src(workdir, src, tag):
    path = os.path.join(workdir, f"{tag}.aether")
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(src)
    r = subprocess.run([AETHER, "--no-cache", "--no-run", "--diagnostics-json", path],
                       capture_output=True, text=True, timeout=60, cwd=workdir)
    return r.returncode, (parse_records(r.stderr) if r.returncode != 0 else [])


def show(records, workdir, limit=4):
    out = []
    for rec in records[:limit]:
        text = (rec.get("raw") or rec.get("message") or "").replace(workdir + os.sep, "")
        out.append(f"      {rec.get('code') or 'code:null'} line {rec.get('line')}: {text[:200]}")
    return "\n".join(out)


def check_guide(path, workdir, tag_prefix):
    """Return (summary dict, list of problem strings, list of report-only notes)."""
    name = os.path.basename(path)
    problems, notes = [], []
    stats = {"blocks": 0, "compiled": 0, "expected_fail": 0, "fx_rescued": 0}
    for k, (ln, src) in enumerate(parse_blocks(path)):
        stats["blocks"] += 1
        key = next((s for s in EXPECT_FAIL if s in src), None)
        kind, full = wrap(src)
        rc, records = compile_src(workdir, full, f"{tag_prefix}_b{k:02d}_L{ln}")
        where = f"{name}:{ln} ({kind})"
        excerpt = "    " + "\n    ".join(src.split("\n")[:6])

        if key is not None:
            why, codes = EXPECT_FAIL[key]
            if rc == 0:
                problems.append(f"{where}: COMPILED but expected to fail ({why})\n{excerpt}")
                continue
            got = {r.get("code") for r in records if r.get("code")}
            uncoded = [r for r in records if not r.get("code")]
            if got == codes and not uncoded:
                stats["expected_fail"] += 1
                continue
            detail = (f"expected exactly {sorted(codes)}, got {sorted(got)}"
                      + (f" plus {len(uncoded)} uncoded record(s)" if uncoded else ""))
            if key in CODE_CHECK_REPORT_ONLY:
                stats["expected_fail"] += 1
                notes.append(f"{where}: code check report-only: {detail}; "
                             f"{CODE_CHECK_REPORT_ONLY[key]}\n{show(records, workdir)}")
            else:
                problems.append(f"{where}: fails, but not as EXPECT_FAIL says: {detail}\n"
                                f"{show(records, workdir)}\n{excerpt}")
            continue

        if rc == 0:
            stats["compiled"] += 1
            continue
        if kind == "frag":
            rc_fx, _ = compile_src(workdir, wrap_fx(src), f"{tag_prefix}_b{k:02d}_L{ln}_fx")
            if rc_fx == 0:
                allow = next((why for (g, s), why in FX_RESCUED.items()
                              if g == name and s in src), None)
                if allow:
                    stats["fx_rescued"] += 1
                    notes.append(f"{where}: compiles only inside an fx block it does not show "
                                 f"(allowlisted: {allow})")
                else:
                    problems.append(f"{where}: compiles only inside an fx block it does not "
                                    f"show; a model copies the block as written\n"
                                    f"{show(records, workdir)}\n{excerpt}")
                continue
        problems.append(f"{where}: FAILED but should compile\n{show(records, workdir)}\n{excerpt}")
    return stats, problems, notes


def stale_keys(paths):
    """EXPECT_FAIL / FX_RESCUED keys that match no block in any given guide."""
    blocks = {os.path.basename(p): [src for _, src in parse_blocks(p)] for p in paths}
    stale = [f"EXPECT_FAIL key matches no block in any guide: {k!r}"
             for k in EXPECT_FAIL if not any(k in src for bs in blocks.values() for src in bs)]
    stale += [f"FX_RESCUED entry matches no block in {g}: {s!r}"
              for (g, s) in FX_RESCUED if g in blocks and not any(s in src for src in blocks[g])]
    stale += [f"CODE_CHECK_REPORT_ONLY key is not an EXPECT_FAIL key: {k!r}"
              for k in CODE_CHECK_REPORT_ONLY if k not in EXPECT_FAIL]
    return stale


def main():
    ap = argparse.ArgumentParser(description="Compile-check the guides' aether blocks.")
    ap.add_argument("guides", nargs="+")
    ap.add_argument("--keys-only", action="store_true",
                    help="only check that every EXPECT_FAIL key still matches a block")
    args = ap.parse_args()

    names = {os.path.basename(p) for p in args.guides}
    all_guides = set(GUIDE_NAMES) <= names
    failed = False

    if not args.keys_only:
        with tempfile.TemporaryDirectory(prefix="aether_guide_snip_") as workdir:
            for n, path in enumerate(args.guides):
                stats, problems, notes = check_guide(path, workdir, f"g{n}")
                print(f"{os.path.basename(path)}: blocks {stats['blocks']}  compiled "
                      f"{stats['compiled']}  expected-fail verified {stats['expected_fail']}  "
                      f"fx-rescued (allowlisted) {stats['fx_rescued']}  UNEXPECTED {len(problems)}")
                for note in notes:
                    print(f"  note: {note}")
                for p in problems:
                    print(f"--- UNEXPECTED {p}\n")
                failed = failed or bool(problems)

    if all_guides:
        stale = stale_keys(args.guides)
        for s in stale:
            print(f"--- UNEXPECTED {s}")
        print(f"EXPECT_FAIL keys: {len(EXPECT_FAIL)}, stale {len(stale)} "
              f"(checked across all three guides)")
        failed = failed or bool(stale)
    elif args.keys_only:
        print("ERROR: --keys-only needs all three guides: " + ", ".join(GUIDE_NAMES))
        return 2
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
