#!/usr/bin/env python3
"""Reformat-invariance gate: whitespace and comments must not change a program.

Run by CTest as `aether_reformat_invariance` (label `invariance`). Each
tests/*_pass.aether fixture and each program under examples/base and
examples/showcase is rewritten by every layout-only transform below, then the
original and the rewrite are run and their exit status and stdout compared.
Whitespace-only changes have been shown to change whether a program compiles
and what it prints -- wrapped call arguments, a `let` broken before its
initializer, compile-clean runtime failures that leak `__aether_*` names -- and
without this gate every text-pass removal has no oracle.

Transforms (all leave `toon:` blocks alone, apart from shifting them with
their `let` line, and skip `@` annotation and `//` comment lines where noted):
  args        newline after each top-level comma inside (...)
  argsnofn    the same, leaving `fn` / `export fn` signature lines alone
  leteq       break `let x: T = rhs;` after the `=`
  indent0     strip all indentation
  indent2     re-indent to 2 spaces per level (from 4)
  indent8     re-indent to 8 spaces per level
  indenttab   re-indent with one tab per level
  comment     append `// note, with comma` to every code line, which pins the
              shared lexer's heuristic for `//` after an expression tail
  join        join a statement onto the previous line when both end in `;`

The original runs twice; a program whose two runs differ (a `par` race, a
clock, random) is reported and skipped. A transform that diverges is confirmed
before it counts: the original runs three more times and the rewrite twice
more, and if the original disagrees with itself or any rewrite run matches it,
the pair is reported as flaky, not as a divergence (and a known pair that comes
out flaky is not treated as fixed). Programs that call http*/socket*/ai_* are
compared by compile result only (`--no-run`), so the gate needs no network and
binds no port; so are programs with a `par` block, whose output can tear
mid-line under load, until par output is made deterministic.

tests/reformat_invariance.known lists today's divergences as `program
transform` pairs. The gate fails on a divergence that is not listed, and on a
listed pair that now matches -- delete its line: the list only shrinks.

Usage:
    python3 tests/tools/reformat_invariance.py [--jobs N] [--only NAME] [-v]
    python3 tests/tools/reformat_invariance.py --write-known   # re-baseline (owner sign-off)
Exit status: 0 matches the list, 1 new divergence or stale entry, 2 setup error.
"""
import argparse
import concurrent.futures
import glob
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
TESTS = os.path.dirname(HERE)
REPO = os.path.dirname(TESTS)
KNOWN = os.path.join(TESTS, "reformat_invariance.known")
AETHER = os.environ.get("AETHER_BIN", os.path.join(REPO, "build", "aether"))
TIMEOUT = 20
NOTE = "// note, with comma"
NETWORK = re.compile(r"\b(?:http\w*|socket\w*|ai_\w+|dnslookup)\s*\(", re.I)
# `par` output can tear mid-line under load (par_forward_target_args_pass
# differs from itself in 2-20% of runs), so par programs are compared by
# compile result until par output is deterministic.
PAR = re.compile(r"^\s*par\s*\{", re.M)
TOON_MARKER = re.compile(r"=\s*toon:\s*(?://.*)?$")
LET = re.compile(r"^(\s*)(let\s+[A-Za-z_][A-Za-z0-9_]*(\s*:\s*[^=]+?)?\s*)=\s*(?!=)(.+;)\s*$")
FN_LINE = re.compile(r"(export\s+)?fn\b")


# ---- line classification ----------------------------------------------------

def classify(lines):
    """Per line: 'toon' (inside a toon: block), 'block' (inside /* */), or 'code'.
    Also returns {toon block line: index of its marker line}."""
    kinds, owner = [], {}
    in_block, marker, marker_indent = False, None, 0
    for i, line in enumerate(lines):
        indent = len(line) - len(line.lstrip(" \t"))
        if marker is not None:
            if not line.strip() or indent > marker_indent:
                kinds.append("toon")
                owner[i] = marker
                continue
            marker = None
        if in_block:
            kinds.append("block")
            if "*/" in line:
                in_block = False
            continue
        kinds.append("code")
        stripped = strip_strings(line)
        if "/*" in stripped.split("//", 1)[0] and "*/" not in stripped[stripped.index("/*"):]:
            in_block = True
        if TOON_MARKER.search(stripped):
            marker, marker_indent = i, indent
    return kinds, owner


def strip_strings(line):
    return re.sub(r'"(\\.|[^"\\])*"', '""', line)


# ---- transforms -------------------------------------------------------------

def wrap_args(line, ind):
    out, j, instr, pd, bd, cd = [], 0, None, 0, 0, 0
    while j < len(line):
        c = line[j]
        if instr:
            out.append(c)
            if c == "\\" and j + 1 < len(line):
                out.append(line[j + 1])
                j += 2
                continue
            if c == instr:
                instr = None
            j += 1
            continue
        if c == '"':
            instr = c
            out.append(c)
            j += 1
            continue
        if line.startswith("//", j) or line.startswith("/*", j):
            out.append(line[j:])
            break
        if c == "(":
            pd += 1
        elif c == ")":
            pd -= 1
        elif c == "[":
            bd += 1
        elif c == "]":
            bd -= 1
        elif c == "{":
            cd += 1
        elif c == "}":
            cd -= 1
        if c == "," and pd > 0 and bd == 0 and cd == 0:
            out.append(",\n" + ind + "    ")
            j += 1
            while j < len(line) and line[j] == " ":
                j += 1
            continue
        out.append(c)
        j += 1
    return "".join(out)


def per_code_line(src, fn):
    lines = src.split("\n")
    kinds, _ = classify(lines)
    out = []
    for line, kind in zip(lines, kinds):
        s = line.lstrip()
        if kind != "code" or not s or s.startswith("@") or s.startswith("//"):
            out.append(line)
        else:
            out.append(fn(line, line[:len(line) - len(s)], s))
    return "\n".join(out)


def t_args(src):
    return per_code_line(src, lambda line, ind, s: wrap_args(line, ind))


def t_argsnofn(src):
    return per_code_line(src, lambda line, ind, s:
                         line if FN_LINE.match(s) else wrap_args(line, ind))


def t_leteq(src):
    def one(line, ind, s):
        m = LET.match(line)
        if m and '"' not in m.group(2) and "//" not in strip_strings(m.group(4)):
            return m.group(1) + m.group(2).rstrip() + " =\n" + m.group(1) + "    " + m.group(4)
        return line
    return per_code_line(src, one)


def reindent(unit):
    def t(src):
        lines = src.split("\n")
        kinds, owner = classify(lines)
        new_indent, out = {}, []
        for i, (line, kind) in enumerate(zip(lines, kinds)):
            if kind == "toon":
                m = owner[i]
                delta = new_indent[m] - (len(lines[m]) - len(lines[m].lstrip(" \t")))
                n = len(line) - len(line.lstrip(" "))
                out.append(" " * max(n + delta, 0) + line.lstrip(" ") if line.strip() else line)
                continue
            body = line.lstrip(" \t")
            n = len(line.replace("\t", "    ")) - len(body)
            ind = "" if unit is None else unit * (n // 4) + " " * (n % 4)
            new_indent[i] = len(ind)
            out.append(ind + body if body else "")
        return "\n".join(out)
    return t


def t_comment(src):
    def one(line, ind, s):
        if "/*" in line or "*/" in line:
            return line
        return line.rstrip() + "  " + NOTE
    return per_code_line(src, one)


def t_join(src):
    lines = src.split("\n")
    kinds, _ = classify(lines)
    out, joinable = [], False
    for line, kind in zip(lines, kinds):
        s = line.strip()
        code = strip_strings(line)
        ok = (kind == "code" and s.endswith(";") and not s.startswith("@")
              and "//" not in code and "/*" not in code and "*/" not in code)
        if ok and joinable and out:
            out[-1] = out[-1].rstrip() + " " + s
        else:
            out.append(line)
        joinable = ok
    return "\n".join(out)


TRANSFORMS = {
    "args": t_args,
    "argsnofn": t_argsnofn,
    "leteq": t_leteq,
    "indent0": reindent(None),
    "indent2": reindent("  "),
    "indent8": reindent("        "),
    "indenttab": reindent("\t"),
    "comment": t_comment,
    "join": t_join,
}


# ---- running ----------------------------------------------------------------

def run(path, cwd, compile_only):
    args = [AETHER, "--no-cache"] + (["--no-run"] if compile_only else []) + [os.path.basename(path)]
    try:
        r = subprocess.run(args, cwd=cwd, capture_output=True, text=True,
                           timeout=TIMEOUT, stdin=subprocess.DEVNULL)
    except subprocess.TimeoutExpired:
        return ("timeout", ""), ""
    first = next((l for l in r.stderr.splitlines() if l.strip()), "")
    return (r.returncode, r.stdout), first


def programs():
    """[(name, source path, directory it must run from)]."""
    out = [(f"tests/{os.path.basename(p)}", p, TESTS)
           for p in sorted(glob.glob(os.path.join(TESTS, "*_pass.aether")))]
    for sub in ("base", "showcase"):
        d = os.path.join(REPO, "examples", sub)
        for p in sorted(glob.glob(os.path.join(d, "*"))):
            if os.path.isfile(p) and not p.endswith((".md", ".json")):
                out.append((f"examples/{sub}/{os.path.basename(p)}", p, d))
    return out


def check_program(name, path, srcdir, tmp, transforms, verbose):
    """Return (diverged {transform: why}, flaky [t], skipped reason or None, tested [t])."""
    with open(path, encoding="utf-8") as fh:
        src = fh.read()
    compile_only = bool(NETWORK.search(src) or PAR.search(src))
    work = os.path.join(tmp, name.replace("/", "__"))
    # A private copy of what the program can see beside it: modules, data
    # files and (in examples/) the other programs. tests/ holds ~250 fixtures,
    # so only its non-.aether entries are copied there.
    orig = os.path.join(work, "orig")
    os.makedirs(orig)
    for entry in os.listdir(srcdir):
        if srcdir == TESTS and entry.endswith(".aether") and entry != os.path.basename(path):
            continue
        s, d = os.path.join(srcdir, entry), os.path.join(orig, entry)
        (shutil.copytree if os.path.isdir(s) else shutil.copy)(s, d)
    orig_path = os.path.join(orig, os.path.basename(path))
    base, _ = run(orig_path, os.path.join(work, "orig"), compile_only)
    if base[0] == "timeout":
        return {}, [], "original times out", []
    again, _ = run(orig_path, os.path.join(work, "orig"), compile_only)
    if again != base:
        return {}, [], "nondeterministic (two runs of the original differ)", []
    diverged, flaky, tested = {}, [], []
    for t in transforms:
        new = TRANSFORMS[t](src)
        if new == src:
            continue
        tested.append(t)
        # The rewrite gets its own directory whose other entries link to this
        # program's private copy of its source directory (not to the repo), so
        # modules and data files resolve as they do for the original.
        tdir = os.path.join(work, t)
        os.mkdir(tdir)
        for entry in os.listdir(os.path.join(work, "orig")):
            if entry != os.path.basename(path):
                os.symlink(os.path.join(work, "orig", entry), os.path.join(tdir, entry))
        tpath = os.path.join(tdir, os.path.basename(path))
        with open(tpath, "w", encoding="utf-8") as fh:
            fh.write(new)
        got, first = run(tpath, tdir, compile_only)
        if got == base:
            continue
        # Confirm before calling it a divergence: a racy program (par output
        # tearing) can differ from itself, so the original must agree with
        # itself three more times and no re-run of the rewrite may match it.
        origs = {base} | {run(orig_path, os.path.join(work, "orig"), compile_only)[0]
                          for _ in range(3)}
        rewrites = {got} | {run(tpath, tdir, compile_only)[0] for _ in range(2)}
        if len(origs) > 1 or origs & rewrites:
            flaky.append(t)
            continue
        if got[0] == 0 and not first:
            first = "same exit status, different stdout"
        elif not first:
            first = f"exit {got[0]}, no stderr"
        diverged[t] = f"rc {base[0]}->{got[0]}: {first[:150]}"
        if verbose:
            print(f"  {name} {t}: {diverged[t]}")
    return diverged, flaky, None, tested


def read_known():
    known = {}
    try:
        with open(KNOWN, encoding="utf-8") as fh:
            for no, line in enumerate(fh, 1):
                body = line.split("#", 1)[0].split()
                if body:
                    known[(body[0], body[1])] = no
    except FileNotFoundError:
        pass
    return known


def write_known(results):
    pairs = sorted((n, t, why) for n, (d, _, _, _) in results.items() for t, why in d.items())
    with open(KNOWN, "w", encoding="utf-8") as fh:
        fh.write("# Known reformat-invariance divergences: `program transform  # why`.\n")
        fh.write("# tests/tools/reformat_invariance.py (CTest aether_reformat_invariance)\n")
        fh.write("# fails on a divergence missing here and on a line here that now passes,\n")
        fh.write("# so this list only shrinks. Re-baselining (--write-known) needs the owner's\n")
        fh.write("# sign-off. Each fix commit deletes its lines and records the new count.\n")
        for n, t, why in pairs:
            fh.write(f"{n} {t}  # {why}\n")
    return len(pairs)


def main():
    ap = argparse.ArgumentParser(description="Reformat-invariance gate.")
    ap.add_argument("--jobs", type=int, default=min(8, os.cpu_count() or 2))
    ap.add_argument("--only", action="append", help="check only programs whose name contains this")
    ap.add_argument("--transform", action="append", choices=sorted(TRANSFORMS))
    ap.add_argument("--write-known", action="store_true",
                    help="rewrite tests/reformat_invariance.known from this run")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()
    if not os.access(AETHER, os.X_OK):
        print(f"ERROR: AETHER_BIN not executable: {AETHER}")
        return 2
    transforms = args.transform or list(TRANSFORMS)
    progs = [p for p in programs() if not args.only or any(o in p[0] for o in args.only)]

    results = {}
    with tempfile.TemporaryDirectory(prefix="aether_reformat_") as tmp:
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
            futs = {pool.submit(check_program, n, p, d, tmp, transforms, args.verbose): n
                    for n, p, d in progs}
            for f in concurrent.futures.as_completed(futs):
                results[futs[f]] = f.result()

    if args.write_known:
        n = write_known(results)
        print(f"wrote {n} known divergences to {os.path.relpath(KNOWN, REPO)}")
        return 0

    known = read_known()
    tested = {(n, t) for n, r in results.items() for t in r[3]}
    skipped = {n: r[2] for n, r in results.items() if r[2]}
    current = {(n, t): why for n, r in results.items() for t, why in r[0].items()}
    new = sorted(k for k in current if k not in known)
    full_run = not args.only and not args.transform
    flaky = {(n, t) for n, r in results.items() for t in r[1]}
    fixed = sorted(k for k in known if k in tested and k not in current and k not in flaky
                   and k[0] not in skipped and (full_run or k[0] in results))
    gone = sorted(k for k in known if full_run and k[0] not in skipped and k not in tested)

    pairs = sum(len(r[3]) for r in results.values())
    print(f"reformat invariance: {len(results)} programs, {pairs} (program, transform) pairs "
          f"tested, {len(current)} divergent, {len(known)} known")
    for n, why in sorted(skipped.items()):
        print(f"  skipped {n}: {why}")
    for n, r in sorted(results.items()):
        for t in r[1]:
            print(f"  flaky   {n} {t} (a re-run matched the original)")
    by_t = {}
    for (n, t) in current:
        by_t[t] = by_t.get(t, 0) + 1
    print("  divergent by transform: " + (", ".join(f"{t} {by_t[t]}" for t in TRANSFORMS if t in by_t) or "none"))
    for k in new:
        print(f"NEW DIVERGENCE {k[0]} {k[1]}: {current[k]}")
    for k in fixed:
        print(f"NOW PASSES {k[0]} {k[1]}: delete line {known[k]} of tests/reformat_invariance.known")
    for k in gone:
        print(f"STALE {k[0]} {k[1]}: no longer tested (program gone or transform a no-op); "
              f"delete line {known[k]}")
    return 1 if new or fixed or gone else 0


if __name__ == "__main__":
    sys.exit(main())
