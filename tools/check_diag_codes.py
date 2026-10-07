#!/usr/bin/env python3
"""Check the diagnostic-code map between the compiler and the three guides.

Run by CTest as `aether_diag_codes`, in REPORT mode until the first guide pass:
it prints every finding and exits 0; `--strict` exits 1 on any finding, which is
what the guide pass switches the CTest to.

The source of truth is what the compiler actually prints, not what its sources
mention: dead branches (MUT-001 in diagnostics.c) and codes sitting in comments
or hint strings would otherwise count as emitted.

 1. Emitted set: the `[CODE]` tags printed (stdout and stderr, default flags)
    by every tests/*_fail*.aether and tests/*_warn*_pass.aether fixture, run in
    a scratch copy of tests/ so sibling modules resolve. A fixture that
    compiles is run (stdin from /dev/null, 20 s limit), so runtime codes count;
    one that calls http*/socket*/ai_* is only compiled. PROBES below stand in
    for codes no fixture covers yet; a code seen only in a probe is a finding
    (check 5).
 2. Reverse check: a code written as a C string literal -- `"[XYZ-001] ..."`
    or exactly `"XYZ-001"` -- in src/aether, rea's src/rea and pscal-core's
    src/{vm,ext_builtins,backend_ast} (literals only; comments are stripped)
    must be emitted, or listed in SOURCE_ONLY with a reason.
 3. Per guide: every emitted code appears -- for medium and full in Repair
    rules as `**[CODE]**` or in a `(CODE)` heading, for small anywhere -- and
    every `**[CODE]**` repair bullet is emitted or an authoring-rule name
    (AUTHORING_RULES).
 4. Full's "The codes the compiler actually emits are ..." list equals the
    emitted compile-time set (codes seen only when a program runs, such as
    ARR-003 and AETH-RUNTIME-*, carry their own message and are reported
    separately).
 5. Every emitted code has at least one fixture.

Usage:
    python3 tools/check_diag_codes.py [--strict] [--verbose] [--docs DIR]
Environment: AETHER_BIN, AETHER_REA_DIR, AETHER_PSCAL_CORE_DIR.
Exit status: 0 (report mode, or strict and clean), 1 strict with findings,
2 setup error.
"""
import argparse
import glob
import os
import re
import shutil
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
AETHER = os.environ.get("AETHER_BIN", os.path.join(REPO, "build", "aether"))
REA = os.environ.get("AETHER_REA_DIR") or os.path.join(REPO, "external", "rea")
CORE = os.environ.get("AETHER_PSCAL_CORE_DIR") or os.path.join(REPO, "external", "pscal-core")
GUIDES = (("full", "aether_for_llms_and_others.md"),
          ("medium", "aether_for_llms_medium_contexts.md"),
          ("small", "aether_for_llms_with_small_contexts.md"))

CODE = r"[A-Z]{2,10}-\d{3}"
TAG = re.compile(r"\[(" + CODE + r")\]")
RUNTIME_TAG = re.compile(r"\b(AETH-RUNTIME-[A-Z0-9-]+)\b")
NETWORK = re.compile(r"\b(?:http\w*|socket\w*|ai_\w+|dnslookup)\s*\(", re.I)

# Rule names the guides teach that are not compiler codes.
AUTHORING_RULES = {
    "BUILT-001": "helper-does-not-exist rule; an unknown helper is reported as SCOPE-001",
    "LEN-001": "length/toon_len crossing; no code of its own",
    "NEST-001": "TOON nested-path rule",
    "KEY-001": "TOON key rule",
    "METH-001": "method rule; appears only inside a hint string",
    "MUT-001": "`let mut` rule; its emission was removed with the text rewriter",
}
# A repair bullet may name an authoring rule, except MUT-001: its bullets tell a
# model how to repair a diagnostic it can never receive (retired per D37d; the
# guide pass deletes them, the "never generate `let mut`" rule stays).
REPAIR_BULLET_OK = set(AUTHORING_RULES) - {"MUT-001"}
# Code literals in the sources that no default-flag run prints, with the reason.
SOURCE_ONLY = {}

# Programs for codes no tests/ fixture covers yet under default flags. W6-02
# asks for tests/name_redeclare_fail.aether (NAME-001) and
# tests/arr_bounds_runtime_fail.aether (ARR-003); IMP-001 is pinned only as a
# --verbose-compat warning (import_missing_fail) although a malformed `use`
# raises it by default. Until fixtures land, these stand in.
PROBES = {
    "probe_import_malformed": "use 123;\nfn main() -> Void {\n    ret;\n}\n",
    "probe_name_redeclare": (
        "fn main() -> Void {\n    let x: Int = 1;\n    let x: Int = 2;\n"
        "    fx { println(x); }\n    ret;\n}\n"),
    "probe_arr_bounds_runtime": (
        "fn main() -> Void {\n    let xs: Int[] = [1, 2];\n    let i: Int = 5;\n"
        "    fx { println(xs[i]); }\n    ret;\n}\n"),
}


def run(args, cwd, timeout=20):
    try:
        r = subprocess.run(args, cwd=cwd, capture_output=True, text=True,
                           timeout=timeout, stdin=subprocess.DEVNULL)
        return r.returncode, r.stdout + r.stderr
    except subprocess.TimeoutExpired as exc:
        out = (exc.stdout or b"") + (exc.stderr or b"")
        return "timeout", out.decode("utf-8", "replace") if isinstance(out, bytes) else out


def codes_of(path, cwd):
    """(compile-time codes, runtime-only codes) printed for one program."""
    base = os.path.basename(path)
    rc, out = run([AETHER, "--no-cache", "--no-run", base], cwd)
    compile_codes = set(TAG.findall(out)) | set(RUNTIME_TAG.findall(out))
    runtime_codes = set()
    with open(path, encoding="utf-8") as fh:
        src = fh.read()
    if rc == 0 and not NETWORK.search(src):
        _, out = run([AETHER, "--no-cache", base], cwd)
        runtime_codes = (set(TAG.findall(out)) | set(RUNTIME_TAG.findall(out))) - compile_codes
    return compile_codes, runtime_codes


def emitted_codes(verbose):
    """{code: {"fixtures": set, "probes": set, "runtime": bool}}."""
    found = {}
    with tempfile.TemporaryDirectory(prefix="aether_diag_codes_") as tmp:
        work = os.path.join(tmp, "tests")
        shutil.copytree(os.path.join(REPO, "tests"), work)
        fixtures = sorted(set(glob.glob(os.path.join(work, "*_fail*.aether")))
                          | set(glob.glob(os.path.join(work, "*_warn*_pass.aether"))))
        for name, src in PROBES.items():
            with open(os.path.join(work, name + ".aether"), "w", encoding="utf-8") as fh:
                fh.write(src)
        for path in fixtures + [os.path.join(work, n + ".aether") for n in PROBES]:
            base = os.path.basename(path)
            is_probe = base[:-len(".aether")] in PROBES
            ct, rt = codes_of(path, work)
            if verbose:
                print(f"  {base}: compile {sorted(ct)} runtime {sorted(rt)}")
            for code in ct | rt:
                e = found.setdefault(code, {"fixtures": set(), "probes": set(), "runtime": True})
                (e["probes"] if is_probe else e["fixtures"]).add(base)
                if code in ct:
                    e["runtime"] = False
    return found


def strip_c_comments(text):
    """Return the C source with comments blanked, string literals intact."""
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if c in "\"'":
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == "\\" else 1
            out.append(text[i:j + 1])
            i = j + 1
        elif text.startswith("//", i):
            j = text.find("\n", i)
            i = n if j == -1 else j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j == -1 else j + 2
            out.append("\n" * text.count("\n", i, j))  # keep line numbers
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


STRING = re.compile(r'"((?:\\.|[^"\\\n])*)"')
BARE = re.compile(r"^" + CODE + r"$")


def source_literals():
    """{code: [file:line, ...]} for codes written as string literals."""
    roots = [os.path.join(REPO, "src", "aether"), os.path.join(REA, "src", "rea")]
    roots += [os.path.join(CORE, "src", d) for d in ("vm", "ext_builtins", "backend_ast")]
    lits, missing = {}, [r for r in roots if not os.path.isdir(r)]
    labels = {REPO: "aether", REA: "rea", CORE: "pscal-core"}
    for root in roots:
        top = next(t for t in labels if os.path.commonpath([t, root]) == t)
        for path in sorted(glob.glob(os.path.join(root, "**", "*.[ch]"), recursive=True)):
            with open(path, encoding="utf-8", errors="replace") as fh:
                code_text = strip_c_comments(fh.read())
            for m in STRING.finditer(code_text):
                body = m.group(1)
                hits = set(TAG.findall(body))
                if BARE.match(body):
                    hits.add(body)
                line = code_text.count("\n", 0, m.start()) + 1
                rel = labels[top] + ":" + os.path.relpath(path, top)
                for code in hits:
                    lits.setdefault(code, []).append(f"{rel}:{line}")
    return lits, missing


def guide_codes(text):
    """(repair-bullet codes, codes in parenthesised headings, every code mentioned)."""
    repair, heading = set(), set()
    in_repair = False
    for line in text.split("\n"):
        if line.startswith("## "):
            in_repair = line.startswith("## Repair rules")
        if line.startswith("#"):
            for group in re.findall(r"\(([^)]*)\)", line):
                heading |= set(re.findall(CODE, group))
        if in_repair:
            repair |= set(re.findall(r"\*\*\[(" + CODE + r")\]\*\*", line))
    return repair, heading, set(re.findall(r"\b" + CODE + r"\b", text))


def emits_sentence(text):
    m = re.search(r"The codes the compiler actually emits are (.*?);", text, re.S)
    return set(re.findall(CODE, m.group(1))) if m else None


def main():
    ap = argparse.ArgumentParser(description="Check the code-to-guide map.")
    ap.add_argument("--strict", action="store_true", help="exit 1 on any finding")
    ap.add_argument("--verbose", action="store_true", help="print each fixture's codes")
    ap.add_argument("--docs", default=os.path.join(REPO, "docs"),
                    help="directory holding the three guides (default: the repo's docs/)")
    args = ap.parse_args()
    if not os.access(AETHER, os.X_OK):
        print(f"ERROR: AETHER_BIN not executable: {AETHER}")
        return 2

    found = emitted_codes(args.verbose)
    emitted = {c for c in found if not c.startswith("AETH-RUNTIME-")}
    compile_time = {c for c in emitted if not found[c]["runtime"]}
    runtime_only = sorted(c for c in found if found[c]["runtime"])
    findings = []

    print(f"emitted codes ({len(emitted)}): {', '.join(sorted(emitted))}")
    print(f"  seen only at run time: {', '.join(runtime_only) or 'none'}")

    # 5. fixtures
    for c in sorted(found):
        if not found[c]["fixtures"]:
            findings.append(f"[fixtures] {c} is emitted (by {', '.join(sorted(found[c]['probes']))}) "
                            "but no tests/ fixture produces it")

    # 2. reverse check
    lits, missing = source_literals()
    for r in missing:
        findings.append(f"[source] cannot scan {r} (set AETHER_REA_DIR / AETHER_PSCAL_CORE_DIR)")
    for c in sorted(lits):
        if c not in found and c not in SOURCE_ONLY:
            where = ", ".join(lits[c][:3]) + (" ..." if len(lits[c]) > 3 else "")
            findings.append(f"[source] {c} is a code literal ({where}) that no default-flag "
                            "fixture or probe prints" +
                            (f"; authoring rule: {AUTHORING_RULES[c]}" if c in AUTHORING_RULES else ""))

    # 3 and 4. guides
    for role, name in GUIDES:
        with open(os.path.join(args.docs, name), encoding="utf-8") as fh:
            text = fh.read()
        repair, heading, anywhere = guide_codes(text)
        documented = anywhere if role == "small" else (repair | heading)
        where = "anywhere" if role == "small" else "in Repair rules or a (CODE) heading"
        for c in sorted(emitted - documented):
            findings.append(f"[{role}] {c} is emitted but not documented {where}")
        for c in sorted(repair - emitted - REPAIR_BULLET_OK):
            why = f" ({AUTHORING_RULES[c]})" if c in AUTHORING_RULES else ""
            findings.append(f"[{role}] repair bullet **[{c}]** names a code nothing emits{why}")
        if role == "full":
            listed = emits_sentence(text)
            if listed is None:
                findings.append("[full] the 'codes the compiler actually emits' list is missing")
            elif listed != compile_time:
                extra, absent = sorted(listed - compile_time), sorted(compile_time - listed)
                findings.append("[full] 'The codes the compiler actually emits' list differs from "
                                f"the emitted compile-time set: listed but not emitted {extra}, "
                                f"emitted but not listed {absent}"
                                + (f"; run-time codes {runtime_only} carry their own message"
                                   if runtime_only else ""))

    for f in findings:
        print(f"  FINDING {f}")
    mode = "STRICT" if args.strict else "REPORT MODE (exit 0; --strict fails on any finding)"
    print(f"diagnostic-code map: {len(findings)} findings. {mode}")
    return 1 if (args.strict and findings) else 0


if __name__ == "__main__":
    sys.exit(main())
