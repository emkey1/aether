#!/usr/bin/env python3
"""Backlog probe lap: each probe pins TODAY's wrong behaviour of one open item.

Nothing re-checked the backlog against the compiler, so ledger entries said
"open" long after a fix, and a fix could ship without its ledger or guide
follow-through. Each probe in tests/backlog/ is a minimal program for one open
plan item, and tests/backlog/manifest.tsv records the outcome class it has on
this compiler. The lap fails when the observed class differs, so the commit
that fixes an item must also flip its probe, update the ledger entry and the
guide lines the manifest names, and move the probe into tests/ as a regression
fixture. This is the front-end counterpart of tests/backend_conformance/
(whose xfail lines flip with a pin bump).

It asserts an outcome CLASS, not VM text:
  silent:<stdout>   exit 0 and exactly this stdout (`\\n` between lines, the
                    final newline implied; `\\t`, `\\\\` escapes)
  silent~<regex>    exit 0 and stdout (final newline stripped) fullmatches regex
  passes            exit 0 and stdout equal to <slug>.out (the item's own
                    defect is elsewhere, e.g. a guide still forbids it)
  fails:<CODE>      nonzero exit; the first error record of
                    --diagnostics-json carries CODE
  uncoded-compile   --no-run fails and the first error record has no code
  uncoded-runtime   --no-run compiles; the run exits 1..127 and the first
                    error record has no code (or no JSON is written)
  crash             killed by a signal, or exit >= 128
  hang              no exit within the timeout

Manifest columns (tab separated): slug, outcome, anchor, guide lines, note.
  anchor  the plan item that fixes it (`W4-22`), or `ideas:<heading text>`
          for a docs/ideas_and_todo.md heading; checked to exist
  guide   comma-separated `full:N`, `medium:N`, `small:N` lines that state the
          behaviour (printed when the probe flips), or `-`
Sidecars: <slug>.in (stdin), <slug>.flags (extra aether flags), <slug>.cap
(needs curl, yyjson, openai or sdl: skipped on a build without it), <slug>.out
(stdout for `passes`).

Usage: python3 tools/run_backlog.py [--aether BIN] [--only SLUG ...] [--observe] [-v]
  --observe  print each probe's observed class as a manifest outcome, no gate
Exit status: 0 every probe matches, 1 a probe flipped or the manifest is
stale, 2 setup error.
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
PROBES = os.path.join(REPO, "tests", "backlog")
MANIFEST = os.path.join(PROBES, "manifest.tsv")
IDEAS = os.path.join(REPO, "docs", "ideas_and_todo.md")
GUIDES = {
    "full": os.path.join(REPO, "docs", "aether_for_llms_and_others.md"),
    "medium": os.path.join(REPO, "docs", "aether_for_llms_medium_contexts.md"),
    "small": os.path.join(REPO, "docs", "aether_for_llms_with_small_contexts.md"),
}
TIMEOUT = 10
FLIPPED = ("item flipped: update ledger/guide in this commit and move the probe "
           "into tests/ as a regression fixture")

sys.path.insert(0, HERE)
import aether_caps  # noqa: E402


def unescape(text):
    out, i = [], 0
    while i < len(text):
        c = text[i]
        if c == "\\" and i + 1 < len(text):
            nxt = text[i + 1]
            out.append({"n": "\n", "t": "\t", "\\": "\\"}.get(nxt, "\\" + nxt))
            i += 2
            continue
        out.append(c)
        i += 1
    return "".join(out)


def escape(text):
    return text.replace("\\", "\\\\").replace("\t", "\\t").replace("\n", "\\n")


def load_manifest():
    entries, problems = [], []
    with open(MANIFEST, encoding="utf-8") as fh:
        for lineno, raw in enumerate(fh, 1):
            line = raw.rstrip("\n")
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            cols = line.split("\t")
            if len(cols) < 4:
                problems.append(f"manifest.tsv:{lineno}: need slug, outcome, anchor, guide (tab separated)")
                continue
            slug, outcome, anchor, guide = (c.strip() for c in cols[:4])
            note = cols[4].strip() if len(cols) > 4 else ""
            entries.append({"slug": slug, "outcome": outcome, "anchor": anchor,
                            "guide": guide, "note": note, "line": lineno})
    return entries, problems


def ideas_headings():
    with open(IDEAS, encoding="utf-8") as fh:
        return [ln.lstrip("#").strip() for ln in fh if ln.startswith("#")]


def guide_lines():
    lines = {}
    for name, path in GUIDES.items():
        with open(path, encoding="utf-8") as fh:
            lines[name] = fh.read().splitlines()
    return lines


def check_entry(e, headings, guides):
    """Problems with one manifest row that do not need the compiler."""
    problems = []
    where = f"manifest.tsv:{e['line']} {e['slug']}"
    if not os.path.exists(os.path.join(PROBES, e["slug"] + ".aether")):
        problems.append(f"{where}: no tests/backlog/{e['slug']}.aether")
    out = e["outcome"]
    if not (out.startswith(("silent:", "silent~", "fails:")) or
            out in ("passes", "uncoded-compile", "uncoded-runtime", "crash", "hang")):
        problems.append(f"{where}: unknown outcome {out!r}")
    if out.startswith("silent~"):
        try:
            re.compile(out[len("silent~"):])
        except re.error as err:
            problems.append(f"{where}: bad regex: {err}")
    if out == "passes" and not os.path.exists(os.path.join(PROBES, e["slug"] + ".out")):
        problems.append(f"{where}: `passes` needs tests/backlog/{e['slug']}.out")
    anchor = e["anchor"]
    if anchor.startswith("ideas:"):
        text = anchor[len("ideas:"):].strip()
        if not any(text in h for h in headings):
            problems.append(f"{where}: no docs/ideas_and_todo.md heading contains {text!r}")
    elif not re.fullmatch(r"W\d+-\d+[a-z]?(\+W\d+-\d+[a-z]?)*", anchor):
        problems.append(f"{where}: anchor must be a plan W-key or ideas:<heading text>")
    if e["guide"] != "-":
        for ref in e["guide"].split(","):
            m = re.fullmatch(r"(full|medium|small):(\d+)", ref.strip())
            if not m:
                problems.append(f"{where}: guide ref {ref!r} is not full:N, medium:N or small:N")
            elif not 1 <= int(m.group(2)) <= len(guides[m.group(1)]):
                problems.append(f"{where}: {ref} is past the end of the guide")
    return problems


def run(cmd, stdin, cwd):
    try:
        p = subprocess.run(cmd, input=stdin, capture_output=True, cwd=cwd, timeout=TIMEOUT)
        return p.returncode, p.stdout.decode("utf-8", "replace"), p.stderr.decode("utf-8", "replace")
    except subprocess.TimeoutExpired:
        return None, "", ""


def first_error_code(aether, flags, src, stdin, cwd):
    """(saw JSON, code of the first error-severity record or None)."""
    _, _, err = run([aether, "--no-cache", "--diagnostics-json"] + flags + [src], stdin, cwd)
    try:
        records = json.loads(err.strip())
    except json.JSONDecodeError:
        return False, None
    if isinstance(records, dict):
        records = records.get("diagnostics", [records])
    for r in records if isinstance(records, list) else []:
        if isinstance(r, dict) and r.get("severity", "error") == "error":
            return True, r.get("code")
    return True, None


def observe(e, aether, work):
    """(observed class, stdout, stderr head) for one probe."""
    base = os.path.join(PROBES, e["slug"])
    src = base + ".aether"
    stdin = b""
    if os.path.exists(base + ".in"):
        with open(base + ".in", "rb") as fh:
            stdin = fh.read()
    flags = []
    if os.path.exists(base + ".flags"):
        with open(base + ".flags", encoding="utf-8") as fh:
            flags = fh.read().split()
    cwd = tempfile.mkdtemp(prefix=e["slug"] + "_", dir=work)
    rc, out, err = run([aether, "--no-cache"] + flags + [src], stdin, cwd)
    head = err.strip().splitlines()[0] if err.strip() else ""
    if rc is None:
        return "hang", out, head
    if rc < 0 or rc >= 128:
        return "crash", out, head
    if rc == 0:
        return "rc0", out, head
    _, code = first_error_code(aether, flags, src, stdin, cwd)
    if code:
        return f"fails:{code}", out, head
    crc, _, _ = run([aether, "--no-cache", "--no-run"] + flags + [src], stdin, cwd)
    return ("uncoded-compile" if crc != 0 else "uncoded-runtime"), out, head


def matches(e, observed, out):
    want = e["outcome"]
    if observed != "rc0":
        return want == observed
    if want.startswith("silent:"):
        return out == unescape(want[len("silent:"):]) + "\n"
    if want.startswith("silent~"):
        return re.fullmatch(want[len("silent~"):], out[:-1] if out.endswith("\n") else out,
                            re.S) is not None
    if want == "passes":
        with open(os.path.join(PROBES, e["slug"] + ".out"), encoding="utf-8") as fh:
            return out == fh.read()
    return False


def describe(observed, out):
    if observed != "rc0":
        return observed
    return "silent:" + escape(out[:-1] if out.endswith("\n") else out)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--aether", default=os.environ.get("AETHER_BIN", os.path.join(REPO, "build", "aether")))
    ap.add_argument("--only", nargs="*", help="run only these probe slugs")
    ap.add_argument("--observe", action="store_true",
                    help="print each probe's observed outcome as a manifest value; never fails")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()
    aether = os.path.abspath(args.aether)
    if not (os.path.isfile(aether) and os.access(aether, os.X_OK)):
        print(f"aether binary not found: {aether}", file=sys.stderr)
        return 2

    entries, problems = load_manifest()
    headings, guides = ideas_headings(), guide_lines()
    for e in entries:
        problems += check_entry(e, headings, guides)
    listed = {e["slug"] for e in entries}
    for name in sorted(os.listdir(PROBES)):
        if name.endswith(".aether") and name[:-len(".aether")] not in listed:
            problems.append(f"tests/backlog/{name}: not in manifest.tsv")
    if args.only:
        entries = [e for e in entries if e["slug"] in args.only]

    caps = aether_caps.detect(aether)
    work = tempfile.mkdtemp(prefix="aether_backlog_")
    counts = {"match": 0, "flipped": 0, "skip": 0}
    try:
        for e in entries:
            need = aether_caps.read_caps_file(os.path.join(PROBES, e["slug"] + ".cap"))
            if need - caps:
                print(f"SKIP     {e['slug']}: needs {', '.join(sorted(need - caps))}")
                counts["skip"] += 1
                continue
            observed, out, head = observe(e, aether, work)
            got = describe(observed, out)
            if args.observe:
                print(f"{e['slug']}\t{got if not matches(e, observed, out) else e['outcome']}")
                continue
            if matches(e, observed, out):
                counts["match"] += 1
                print(f"ok       {e['slug']} [{e['anchor']}] {e['outcome']}")
                if args.verbose and head:
                    print(f"           stderr: {head}")
                continue
            counts["flipped"] += 1
            print(f"FLIPPED  {e['slug']} [{e['anchor']}]: manifest says {e['outcome']!r}, "
                  f"observed {got!r}")
            if head:
                print(f"           stderr: {head}")
            print(f"           {FLIPPED}")
            print(f"           ledger: {e['anchor']}" + (f"  ({e['note']})" if e["note"] else ""))
            if e["guide"] != "-":
                for ref in e["guide"].split(","):
                    g, n = ref.strip().split(":")
                    print(f"           guide {g}:{n}: {guides[g][int(n) - 1].strip()}")
    finally:
        shutil.rmtree(work, ignore_errors=True)

    for p in problems:
        print(f"STALE    {p}")
    if args.observe:
        return 0
    print(f"backlog: {counts['match']} unchanged, {counts['flipped']} flipped, "
          f"{counts['skip']} skipped, {len(problems)} manifest problem(s)")
    return 1 if counts["flipped"] or problems else 0


if __name__ == "__main__":
    sys.exit(main())
