#!/usr/bin/env python3
"""Compile-check every ```aether block in the LLM-facing guides.

Usage:
    python3 tools/verify_guide_snippets.py docs/aether_for_llms_*.md     # all three
    python3 tools/verify_guide_snippets.py docs/aether_for_llms_medium_contexts.md
    python3 tools/verify_guide_snippets.py --keys-only docs/aether_for_llms_*.md
    python3 tools/verify_guide_snippets.py --run-outputs docs/aether_card.md
    python3 tools/verify_guide_snippets.py --spec docs/aether_spec.md

Every block is compiled with `aether --no-cache --no-run --diagnostics-json`
($AETHER_BIN, default build/aether). Fragments are wrapped in a function over
the CONTEXT prelude below, and declaration blocks get a trivial `main` and a
top-level `main();` call.
The stub modules in tests/guide_modules/ sit next to every compiled block, so
`use "score_utils";` or `use "geometry";` resolves and its calls are checked.
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
    --keys-only, as the CTest aether_guide_snippet_keys does);
  * with --run-outputs (the card's CTest, aether_card_snippets): a complete
    program that is not followed by a ```text block holding its output, or
    whose run exits non-zero or prints anything else. The card states what each
    program prints, so the gate runs it.

With --run (CTest aether_guide_run) it instead RUNS every complete program
(a block with its own `fn main`) and the recipe drivers, because a block that
compiles can still abort or print the wrong thing (the par/Tally incident went
16 days unnoticed under the compile-only gate):

  * each complete program must exit 0 within 20 s, and print exactly the
    stdout docs/guide_goldens.json records for it, keyed by the sha256 of the
    block text. An `rc_only` entry (time- or environment-dependent output)
    checks the exit status alone; a `skip` entry (live network) is not run;
    an EXPECT_FAIL block is counted as expected and not run. A socket
    program's port literal is rewritten to a free port in the temp copy, so
    concurrent runs do not collide. A program with no entry fails: bless it
    with --update and review the diff.
  * each tests/guide_recipes/<name>.driver.aether names a guide set and the
    recipe functions it exercises in its header (`// guides: full medium`,
    `// recipes: total mean ...`). The functions are extracted from each named
    guide by name (with the annotations directly above them), the driver's
    main is appended, and the run's stdout must equal <name>.out. The .out is
    written by hand from what the recipes SHOULD print, never blessed.
  * --update rewrites the goldens' stdout entries from this build and prints
    what changed; rc_only and skip entries are kept, stale keys dropped.

--spec runs the tagged examples of the normative spec instead (see
check_spec below and the header of docs/aether_spec.md).

See docs/aether_doc_maintenance.md for why this exists.
"""
import argparse
import difflib
import hashlib
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
AETHER = os.environ.get("AETHER_BIN", os.path.join(REPO, "build", "aether"))
# Stub modules copied next to every compiled block, so an import example whose
# module is one of these compiles for real instead of sitting in EXPECT_FAIL.
GUIDE_MODULES = os.path.join(REPO, "tests", "guide_modules")
GUIDE_NAMES = ("aether_for_llms_and_others.md", "aether_for_llms_medium_contexts.md",
               "aether_for_llms_with_small_contexts.md")
GUIDE_SHORT = {"full": GUIDE_NAMES[0], "medium": GUIDE_NAMES[1], "small": GUIDE_NAMES[2]}
GOLDENS = os.path.join(REPO, "docs", "guide_goldens.json")
RECIPES = os.path.join(REPO, "tests", "guide_recipes")
RUN_TIMEOUT = 20

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
# A declaration block may also hold top-level statements (script mode); the
# trivial main is then called explicitly, since a `fn main` beside top-level
# statements that never call it is ENTRY-001 (decision D16).
MAIN_CALLED = MAIN + "main();\n"


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


def output_fences(path):
    """{start line of an ```aether block: the ```text block right after it}."""
    with open(path, encoding="utf-8") as fh:
        lines = fh.read().split("\n")
    out, last_aether, i = {}, None, 0
    while i < len(lines):
        s = lines[i].strip()
        if s == "```aether":
            last_aether = i + 1
            while i + 1 < len(lines) and lines[i + 1].strip() != "```":
                i += 1
            i += 2
            continue
        if s == "```text" and last_aether is not None:
            body = []
            i += 1
            while i < len(lines) and lines[i].strip() != "```":
                body.append(lines[i])
                i += 1
            out[last_aether] = "\n".join(body) + "\n"
        elif s:
            last_aether = None
        i += 1
    return out


def run_src(workdir, src, tag):
    path = os.path.join(workdir, f"{tag}.aether")
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(src)
    r = subprocess.run([AETHER, "--no-cache", path], capture_output=True, text=True,
                       timeout=60, cwd=workdir)
    return r.returncode, r.stdout, r.stderr


def wrap(src):
    if re.search(r"\bfn\s+main\s*\(", src):
        return "whole", src
    if re.search(r"^\s*(fn|type|mod|use|const|@)", src, re.M):
        return "decl", src + "\n" + MAIN_CALLED
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


def check_guide(path, workdir, tag_prefix, run_outputs=False):
    """Return (summary dict, list of problem strings, list of report-only notes)."""
    name = os.path.basename(path)
    problems, notes = [], []
    stats = {"blocks": 0, "compiled": 0, "expected_fail": 0, "fx_rescued": 0, "ran": 0}
    outputs = output_fences(path) if run_outputs else {}
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
            if run_outputs and kind == "whole":
                want = outputs.get(ln)
                if want is None:
                    problems.append(f"{where}: a complete program with no ```text block "
                                    f"after it stating its output\n{excerpt}")
                    continue
                rrc, got, err = run_src(workdir, full, f"{tag_prefix}_b{k:02d}_L{ln}_run")
                if rrc != 0 or got != want:
                    problems.append(f"{where}: ran with exit {rrc}; the card says it prints\n"
                                    f"{want!r}\nbut it printed\n{got!r}\n{err.strip()[:300]}")
                else:
                    stats["ran"] += 1
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


def block_sha(src):
    return hashlib.sha256(src.encode("utf-8")).hexdigest()


def load_goldens():
    if not os.path.exists(GOLDENS):
        return {"_note": [], "blocks": {}}
    with open(GOLDENS, encoding="utf-8") as fh:
        return json.load(fh)


def free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sk:
        sk.bind(("127.0.0.1", 0))
        return sk.getsockname()[1]


PORT_LITERAL = re.compile(r"(let\s+port\s*:\s*Int\s*=\s*)(\d{4,5})(\s*;)")


def run_program(workdir, src, tag, timeout=RUN_TIMEOUT):
    """(exit status or 'timeout', stdout, stderr) of one complete program."""
    if "socketbind" in src:
        src = PORT_LITERAL.sub(lambda m: f"{m.group(1)}{free_port()}{m.group(3)}", src)
    path = os.path.join(workdir, f"{tag}.aether")
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(src)
    try:
        r = subprocess.run([AETHER, "--no-cache", path], capture_output=True, text=True,
                           timeout=timeout, cwd=workdir, stdin=subprocess.DEVNULL)
        return r.returncode, r.stdout, r.stderr
    except subprocess.TimeoutExpired:
        return "timeout", "", ""


def run_guide(path, workdir, tag_prefix, goldens, update):
    """Run every complete program in one guide. Returns (stats, problems, seen shas)."""
    name = os.path.basename(path)
    stats = {"whole": 0, "stdout": 0, "rc_only": 0, "skip": 0, "expected_fail": 0}
    problems, seen = [], set()
    entries = goldens.setdefault("blocks", {})
    for k, (ln, src) in enumerate(parse_blocks(path)):
        kind, full = wrap(src)
        if kind != "whole":
            continue
        stats["whole"] += 1
        sha = block_sha(src)
        seen.add(sha)
        where = f"{name}:{ln}"
        if next((key for key in EXPECT_FAIL if key in src), None) is not None:
            stats["expected_fail"] += 1
            continue
        entry = entries.get(sha)
        if entry and "skip" in entry:
            stats["skip"] += 1
            continue
        rc, out, err = run_program(workdir, full, f"{tag_prefix}_run{k:02d}_L{ln}")
        if rc != 0:
            problems.append(f"{where}: exit {rc}\n      {err.strip()[:300]}")
            continue
        if entry and "rc_only" in entry:
            stats["rc_only"] += 1
            continue
        if update:
            if entry is None or entry.get("stdout") != out:
                print(f"  update {where}: stdout {out!r}"
                      + (f" (was {entry.get('stdout')!r})" if entry else " (new)"))
            entries[sha] = {"where": f"{name}:{ln}", "stdout": out}
            stats["stdout"] += 1
            continue
        if entry is None:
            problems.append(f"{where}: no golden in docs/guide_goldens.json for this block "
                            f"(sha256 {sha[:12]}); run --run --update and review the diff")
            continue
        if out != entry["stdout"]:
            problems.append(f"{where}: printed {out!r}, golden says {entry['stdout']!r}")
            continue
        stats["stdout"] += 1
    return stats, problems, seen


def extract_function(guide_path, fname):
    """The source of `fn fname(` in a guide's aether blocks, with the annotation
    lines directly above it. Raises ValueError unless exactly one is found."""
    found = []
    for _, src in parse_blocks(guide_path):
        lines = src.split("\n")
        for i, line in enumerate(lines):
            if not re.match(r"\s*(export\s+)?fn\s+" + re.escape(fname) + r"\s*\(", line):
                continue
            start = i
            while start > 0 and lines[start - 1].strip().startswith("@"):
                start -= 1
            depth, end, opened = 0, None, False
            for j in range(i, len(lines)):
                code = lines[j].split("//", 1)[0]
                depth += code.count("{") - code.count("}")
                opened = opened or "{" in code
                if opened and depth <= 0:
                    end = j
                    break
            if end is None:
                raise ValueError(f"fn {fname}: no closing brace")
            found.append("\n".join(lines[start:end + 1]))
    if len(found) != 1:
        raise ValueError(f"fn {fname}: {len(found)} definitions in {os.path.basename(guide_path)}")
    return found[0]


def run_recipes(guide_paths, workdir):
    """Run each recipe driver against the guides it names. Returns (runs, problems)."""
    by_short = {short: p for short, n in GUIDE_SHORT.items()
                for p in guide_paths if os.path.basename(p) == n}
    runs, problems = 0, []
    if not os.path.isdir(RECIPES):
        return runs, problems
    for drv in sorted(f for f in os.listdir(RECIPES) if f.endswith(".driver.aether")):
        name = drv[:-len(".driver.aether")]
        with open(os.path.join(RECIPES, drv), encoding="utf-8") as fh:
            driver = fh.read()
        want_path = os.path.join(RECIPES, name + ".out")
        if not os.path.exists(want_path):
            problems.append(f"guide_recipes/{drv}: no {name}.out")
            continue
        with open(want_path, encoding="utf-8") as fh:
            want = fh.read()
        mg = re.search(r"^// guides:(.*)$", driver, re.M)
        mr = re.search(r"^// recipes:(.*)$", driver, re.M)
        if not mg or not mr:
            problems.append(f"guide_recipes/{drv}: needs `// guides:` and `// recipes:` header lines")
            continue
        for short in mg.group(1).split():
            if short not in GUIDE_SHORT:
                problems.append(f"guide_recipes/{drv}: unknown guide {short!r}")
                continue
            if short not in by_short:
                continue  # that guide was not given on this run
            try:
                funcs = [extract_function(by_short[short], f) for f in mr.group(1).split()]
            except ValueError as err:
                problems.append(f"guide_recipes/{drv} [{short}]: {err}")
                continue
            prog = "\n\n".join(funcs) + "\n\n" + driver
            rc, out, err = run_program(workdir, prog, f"recipe_{name}_{short}")
            runs += 1
            if rc != 0 or out != want:
                diff = "".join(difflib.unified_diff(want.splitlines(True), out.splitlines(True),
                                                    f"{name}.out", f"{short} recipes"))
                problems.append(f"guide_recipes/{drv} [{short}]: exit {rc}\n{diff}"
                                f"{err.strip()[:300]}")
    return runs, problems

# ---- --spec: the executable examples of docs/aether_spec.md -----------------
# A spec example is a complete program in a fence whose info string carries a
# tag and an id, e.g. ```aether @ok id=G.LetDecl.1 . The tags:
#   @ok             compiles, runs, exits with rc=N (default 0), and prints exactly
#                   the ```text block that follows (an empty block is no output);
#                   warn=CODE also requires that warning on stderr.
#   @reject=A[,B]   fails to compile (--no-run --diagnostics-json) with exactly
#                   the codes listed and no uncoded record.
#   @trap=CODE      compiles, then fails at run time with [CODE] on stderr.
#   @bug=<ref>      pins today's outcome of a behaviour a decision row has decided
#                   to change (or is still deciding): now=ok (with a ```text
#                   block), now=<CODE> (rejected with that code), now=uncoded
#                   (rejected with no code) or now=trap. A changed outcome is
#                   reported as FLIPPED and does not fail the run (--strict-bugs
#                   makes it fail): update the spec, and VERSION, when it flips.
# Ids are unique. Every production defined in a ```ebnf fence of the section
# whose heading starts "## 2." needs at least one @ok and one @reject or @trap
# example whose id starts G.<Production>.
SPEC_FENCE = re.compile(r"^```aether\s+@(ok|reject|trap|bug)(=\S+)?(.*)$")


def spec_examples(path):
    with open(path, encoding="utf-8") as fh:
        lines = fh.read().split("\n")
    out, i = [], 0
    while i < len(lines):
        m = SPEC_FENCE.match(lines[i].strip())
        if not m:
            i += 1
            continue
        start, body = i + 1, []
        i += 1
        while i < len(lines) and lines[i].strip() != "```":
            body.append(lines[i])
            i += 1
        i += 1
        attrs = dict(a.split("=", 1) for a in m.group(3).split() if "=" in a)
        stdout = None
        j = i
        while j < len(lines) and not lines[j].strip():
            j += 1
        if j < len(lines) and lines[j].strip() == "```text":
            text, j = [], j + 1
            while j < len(lines) and lines[j].strip() != "```":
                text.append(lines[j])
                j += 1
            stdout = "".join(t + "\n" for t in text)
            i = j + 1
        out.append({"line": start, "tag": m.group(1), "arg": (m.group(2) or "=")[1:],
                    "attrs": attrs, "src": "\n".join(body) + "\n", "stdout": stdout})
    return out


def spec_productions(path):
    with open(path, encoding="utf-8") as fh:
        text = fh.read()
    sec = re.search(r"^## 2\..*?(?=^## \d+\.|\Z)", text, re.M | re.S)
    names = []
    for fence in re.findall(r"^```ebnf\n(.*?)^```", sec.group(0) if sec else "", re.M | re.S):
        names += re.findall(r"^([A-Z][A-Za-z]*)\s*=", fence, re.M)
    return names


def spec_outcome(workdir, ex, tag):
    """('ok', stdout, rc, stderr) | ('reject', codes, uncoded) | ('trap', stdout, stderr)."""
    rc, records = compile_src(workdir, ex["src"], tag)
    if rc != 0:
        codes = {r.get("code") for r in records if r.get("code")}
        return ("reject", codes, [r for r in records if not r.get("code")])
    rrc, out, err = run_src(workdir, ex["src"], tag + "_run")
    if rrc != 0 and re.search(r"\[[A-Z]+-\d{3}\]|Runtime Error|VM Error", err):
        return ("trap", out, err, rrc)
    return ("ok", out, rrc, err)


def check_spec(path, strict_bugs=False):
    examples = spec_examples(path)
    problems, flipped = [], []
    counts = {"ok": 0, "reject": 0, "trap": 0, "bug": 0}
    seen = {}
    for ex in examples:
        eid = ex["attrs"].get("id")
        where = f"{os.path.basename(path)}:{ex['line']} ({eid or 'no id'})"
        if not eid:
            problems.append(f"{where}: a spec example needs id=")
        elif eid in seen:
            problems.append(f"{where}: duplicate id, first at line {seen[eid]}")
        else:
            seen[eid] = ex["line"]
    with tempfile.TemporaryDirectory(prefix="aether_spec_") as workdir:
        if os.path.isdir(GUIDE_MODULES):
            for mod in sorted(os.listdir(GUIDE_MODULES)):
                shutil.copy(os.path.join(GUIDE_MODULES, mod), workdir)
        for k, ex in enumerate(examples):
            tag, eid = ex["tag"], ex["attrs"].get("id", f"x{k}")
            where = f"{os.path.basename(path)}:{ex['line']} ({eid})"
            got = spec_outcome(workdir, ex, f"spec{k:03d}")
            counts[tag] += 1
            if tag == "ok":
                want_rc = int(ex["attrs"].get("rc", "0"))
                warn = ex["attrs"].get("warn")
                if ex["stdout"] is None:
                    problems.append(f"{where}: @ok needs a ```text block with its stdout")
                elif got[0] != "ok" or got[1] != ex["stdout"] or got[2] != want_rc:
                    problems.append(f"{where}: expected rc {want_rc} and stdout "
                                    f"{ex['stdout']!r}; got {got[0]} {got[1:3]!r}")
                elif warn and f"[{warn}]" not in got[3]:
                    problems.append(f"{where}: expected warning {warn}; stderr {got[3][:200]!r}")
            elif tag == "reject":
                want = set(ex["arg"].split(","))
                if got[0] != "reject" or got[1] != want or got[2]:
                    detail = (f"{got[0]} {sorted(got[1])} with {len(got[2])} uncoded"
                              if got[0] == "reject" else f"{got[0]} {got[1]!r}")
                    problems.append(f"{where}: expected rejection {sorted(want)}; got {detail}")
            elif tag == "trap":
                if got[0] != "trap" or f"[{ex['arg']}]" not in got[2]:
                    problems.append(f"{where}: expected run-time [{ex['arg']}]; got {got[0]} "
                                    f"{got[1:]!r}"[:300])
            else:
                now = ex["attrs"].get("now", "ok")
                if now == "ok":
                    same = got[0] == "ok" and got[1] == ex["stdout"] and got[2] == 0
                elif now == "uncoded":
                    same = got[0] == "reject" and not got[1] and bool(got[2])
                elif now == "trap":
                    same = got[0] == "trap"
                else:
                    same = got[0] == "reject" and got[1] == set(now.split(","))
                if not same:
                    flipped.append(f"{where}: @bug={ex['arg']} no longer behaves as pinned "
                                   f"(now={now}); today: {got[0]} {got[1:3]!r}"[:400])
    by_prod = {}
    for ex in examples:
        parts = ex["attrs"].get("id", "").split(".")
        if len(parts) >= 3 and parts[0] == "G":
            by_prod.setdefault(parts[1], set()).add(ex["tag"])
    prods = spec_productions(path)
    for name in prods:
        tags = by_prod.get(name, set())
        if "ok" not in tags or not tags & {"reject", "trap"}:
            problems.append(f"production {name}: needs at least one @ok and one @reject/@trap "
                            f"example with id G.{name}.<n> (has {sorted(tags) or 'none'})")
    for name in sorted(set(by_prod) - set(prods)):
        problems.append(f"example ids name G.{name}, which no ```ebnf fence in section 2 defines")
    print(f"{os.path.basename(path)}: examples {len(examples)}  ok {counts['ok']}  reject "
          f"{counts['reject']}  trap {counts['trap']}  bug {counts['bug']}  productions "
          f"{len(prods)}  UNEXPECTED {len(problems)}  FLIPPED {len(flipped)}")
    for f in flipped:
        print(f"  FLIPPED {f}")
    for p in problems:
        print(f"--- UNEXPECTED {p}")
    return 1 if problems or (strict_bugs and flipped) else 0


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
    ap.add_argument("--run-outputs", action="store_true",
                    help="also run every complete program and compare its stdout with "
                         "the ```text block that follows it (the card's gate)")
    ap.add_argument("--run", action="store_true",
                    help="run every complete program against docs/guide_goldens.json, and "
                         "the tests/guide_recipes drivers (CTest aether_guide_run)")
    ap.add_argument("--update", action="store_true",
                    help="with --run: rewrite the goldens' stdout entries from this build")
    ap.add_argument("--spec", action="store_true",
                    help="run the tagged @ok/@reject/@trap/@bug examples of the spec")
    ap.add_argument("--strict-bugs", action="store_true",
                    help="with --spec, fail when a pinned @bug outcome flips")
    args = ap.parse_args()
    if args.spec:
        return max(check_spec(p, args.strict_bugs) for p in args.guides)

    names = {os.path.basename(p) for p in args.guides}
    all_guides = set(GUIDE_NAMES) <= names
    failed = False

    if args.run:
        return run_mode(args.guides, all_guides, args.update)

    if not args.keys_only:
        with tempfile.TemporaryDirectory(prefix="aether_guide_snip_") as workdir:
            if os.path.isdir(GUIDE_MODULES):
                for mod in sorted(os.listdir(GUIDE_MODULES)):
                    shutil.copy(os.path.join(GUIDE_MODULES, mod), workdir)
            for n, path in enumerate(args.guides):
                stats, problems, notes = check_guide(path, workdir, f"g{n}", args.run_outputs)
                ran = f"  ran to stated output {stats['ran']}" if args.run_outputs else ""
                print(f"{os.path.basename(path)}: blocks {stats['blocks']}  compiled "
                      f"{stats['compiled']}  expected-fail verified {stats['expected_fail']}  "
                      f"fx-rescued (allowlisted) {stats['fx_rescued']}{ran}  "
                      f"UNEXPECTED {len(problems)}")
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


def run_mode(guides, all_guides, update):
    goldens = load_goldens()
    failed, seen = False, set()
    total = {"whole": 0, "stdout": 0, "rc_only": 0, "skip": 0, "expected_fail": 0}
    with tempfile.TemporaryDirectory(prefix="aether_guide_run_") as workdir:
        if os.path.isdir(GUIDE_MODULES):
            for mod in sorted(os.listdir(GUIDE_MODULES)):
                shutil.copy(os.path.join(GUIDE_MODULES, mod), workdir)
        for n, path in enumerate(guides):
            stats, problems, shas = run_guide(path, workdir, f"g{n}", goldens, update)
            seen |= shas
            for key in total:
                total[key] += stats[key]
            print(f"{os.path.basename(path)}: complete programs {stats['whole']}  ran to golden "
                  f"stdout {stats['stdout']}  rc-only {stats['rc_only']}  skipped {stats['skip']}  "
                  f"expected-fail {stats['expected_fail']}  UNEXPECTED {len(problems)}")
            for p in problems:
                print(f"--- UNEXPECTED {p}\n")
            failed = failed or bool(problems)
        runs, problems = run_recipes(guides, workdir)
        print(f"recipe drivers: {runs} run(s)  UNEXPECTED {len(problems)}")
        for p in problems:
            print(f"--- UNEXPECTED {p}\n")
        failed = failed or bool(problems)
    entries = goldens.get("blocks", {})
    if all_guides:
        stale = sorted(k for k in entries if k not in seen)
        if update:
            for k in stale:
                print(f"  update: dropped stale golden {entries[k].get('where')} ({k[:12]})")
                del entries[k]
        else:
            for k in stale:
                print(f"--- UNEXPECTED stale golden (matches no block): {entries[k].get('where')} {k[:12]}")
            failed = failed or bool(stale)
    if update:
        def where_key(kv):
            guide, _, line = kv[1].get("where", "").rpartition(":")
            return (guide, int(line) if line.isdigit() else 0, kv[0])
        goldens["blocks"] = dict(sorted(entries.items(), key=where_key))
        with open(GOLDENS, "w", encoding="utf-8") as fh:
            json.dump(goldens, fh, indent=2, ensure_ascii=False)
            fh.write("\n")
        print(f"wrote {os.path.relpath(GOLDENS, REPO)}: review the diff before committing")
    print(f"complete programs {total['whole']}: {total['stdout']} ran to golden stdout, "
          f"{total['rc_only']} rc-only, {total['skip']} skipped, "
          f"{total['expected_fail']} expected-fail (not run)")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
