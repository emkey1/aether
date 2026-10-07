#!/usr/bin/env python3
"""Data-driven fixture runner for tests/fx/: one program plus sidecar files.

tests/run.sh is a fail-fast monolith: each new fixture costs ten or more lines
of bash, and the first failure hides how many a change broke. A fixture here is
tests/fx/[<dir>/]<name>.aether plus optional sidecars, and the runner keeps
going over all of them:

  .out        exact stdout (bytes)
  .unordered  present: compare stdout with .out as a multiset of lines (par)
  .err        one substring per line that stderr must contain; a line starting
              with `!` is a substring stderr must NOT contain
  .codes      the exact set of diagnostic codes: the --diagnostics-json
              records' codes plus the [CODE] tags in the run's stderr (the JSON
              is written only on failure, so warnings come from the tags). A
              JSON record with code null fails the fixture.
  .rc         expected exit status (default 0)
  .in         stdin (default empty)
  .flags      extra aether flags, whitespace separated
  .cap        needs curl, yyjson, openai or sdl; skipped on a build without it

Each fixture runs with --no-cache in its own directory under a private temp
dir. A fixture must assert something: .out, .err or .codes.

Usage:
  python3 tools/run_fixtures.py [--aether BIN] [--dir D] [NAME ...] [--update] [-v] [--fx-dir T]
  --dir D     only fixtures under tests/fx/D ('.' for the top level)
  NAME        only these fixtures (path under tests/fx without .aether)
  --update    rewrite .out, .rc and .codes from this build and print each
              change as a diff for review (.err stays hand-written)
Exit status: 0 all passed, 1 a fixture failed, 2 setup error.
"""
import argparse
import difflib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
FX = os.path.join(REPO, "tests", "fx")
TIMEOUT = 60
TAG = re.compile(r"\[([A-Z]{2,10}-\d{3})\]")
RUNTIME_TAG = re.compile(r"\b(AETH-RUNTIME-[A-Z0-9-]+)\b")

sys.path.insert(0, HERE)
import aether_caps  # noqa: E402


def read(path, mode="r"):
    if not os.path.exists(path):
        return None
    with open(path, mode, **({} if "b" in mode else {"encoding": "utf-8"})) as fh:
        return fh.read()


def find_fixtures():
    found = []
    for root, dirs, files in os.walk(FX):
        dirs.sort()
        for f in sorted(files):
            if f.endswith(".aether"):
                found.append(os.path.relpath(os.path.join(root, f[:-len(".aether")]), FX))
    return found


def run(cmd, stdin, cwd):
    try:
        p = subprocess.run(cmd, input=stdin, capture_output=True, cwd=cwd, timeout=TIMEOUT)
        return p.returncode, p.stdout, p.stderr.decode("utf-8", "replace")
    except subprocess.TimeoutExpired:
        return "timeout", b"", ""


def diag_codes(aether, flags, src, stdin, cwd, run_err):
    """(code set, number of JSON records with code null)."""
    codes = set(TAG.findall(run_err)) | set(RUNTIME_TAG.findall(run_err))
    nulls = 0
    _, _, err = run([aether, "--no-cache", "--diagnostics-json"] + flags + [src], stdin, cwd)
    text = err.strip()
    if text.startswith(("[", "{")):
        try:
            records = json.loads(text)
        except json.JSONDecodeError:
            records = []
        if isinstance(records, dict):
            records = records.get("diagnostics", [records])
        for r in records if isinstance(records, list) else []:
            if not isinstance(r, dict):
                continue
            if r.get("code"):
                codes.add(r["code"])
            else:
                nulls += 1
    return codes, nulls


def first_diff(expected, actual):
    exp = expected.decode("utf-8", "replace").splitlines()
    act = actual.decode("utf-8", "replace").splitlines()
    for i in range(max(len(exp), len(act))):
        e = exp[i] if i < len(exp) else "<missing>"
        a = act[i] if i < len(act) else "<missing>"
        if e != a:
            return f"stdout line {i + 1}: expected {e!r}, got {a!r}"
    return "stdout differs in trailing bytes"


def check(name, aether, work, update):
    """(status, [problems], [update diffs]) for one fixture."""
    base = os.path.join(FX, name)
    src = base + ".aether"
    want_out = read(base + ".out", "rb")
    unordered = os.path.exists(base + ".unordered")
    err_lines = [ln for ln in (read(base + ".err") or "").splitlines() if ln.strip()]
    codes_text = read(base + ".codes")
    want_codes = set(codes_text.split()) if codes_text is not None else None
    rc_text = read(base + ".rc")
    want_rc = int(rc_text.strip()) if rc_text and rc_text.strip() else 0
    stdin = read(base + ".in", "rb") or b""
    flags = (read(base + ".flags") or "").split()
    if want_out is None and not err_lines and want_codes is None and not update:
        return "FAIL", ["asserts nothing: add .out, .err or .codes"], []

    cwd = tempfile.mkdtemp(prefix=os.path.basename(name) + "_", dir=work)
    shutil.copy(src, os.path.join(cwd, os.path.basename(src)))
    local = os.path.basename(src)
    rc, out, err = run([aether, "--no-cache"] + flags + [local], stdin, cwd)
    problems, diffs = [], []
    if rc == "timeout":
        return "FAIL", [f"no exit within {TIMEOUT} s"], []
    if rc != want_rc:
        problems.append(f"exit {rc}, want {want_rc}")
    if want_out is not None:
        if unordered:
            if sorted(out.splitlines()) != sorted(want_out.splitlines()):
                problems.append("stdout lines differ from .out as a multiset")
        elif out != want_out:
            problems.append(first_diff(want_out, out))
    for ln in err_lines:
        if ln.startswith("!"):
            if ln[1:] in err:
                problems.append(f"stderr contains forbidden {ln[1:]!r}")
        elif ln not in err:
            problems.append(f"stderr lacks {ln!r}")
    got_codes, nulls = None, 0
    if want_codes is not None or update:
        got_codes, nulls = diag_codes(aether, flags, local, stdin, cwd, err)
        if want_codes is not None and got_codes != want_codes:
            problems.append(f"codes {sorted(got_codes)}, want {sorted(want_codes)}")
        if nulls and (want_codes is not None or got_codes):
            problems.append(f"{nulls} diagnostic record(s) with code null")

    if update:
        new = {}
        new[".out"] = out if (want_out is not None or rc == 0) else None
        if unordered and new[".out"] is not None:
            if want_out is not None and sorted(out.splitlines()) == sorted(want_out.splitlines()):
                new[".out"] = want_out
            else:
                new[".out"] = b"".join(sorted(out.splitlines(True)))
        new[".rc"] = (f"{rc}\n".encode() if rc != 0 else None)
        new[".codes"] = (("\n".join(sorted(got_codes)) + "\n").encode()
                         if got_codes else None)
        for ext, data in new.items():
            path = base + ext
            old = read(path, "rb")
            if data is None:
                if old is not None and ext in (".rc", ".codes"):
                    os.remove(path)
                    diffs.append(f"removed {name}{ext}")
                continue
            if old == data:
                continue
            with open(path, "wb") as fh:
                fh.write(data)
            diffs.append("".join(difflib.unified_diff(
                (old or b"").decode("utf-8", "replace").splitlines(True),
                data.decode("utf-8", "replace").splitlines(True),
                f"a/tests/fx/{name}{ext}", f"b/tests/fx/{name}{ext}")))
        if err_lines and problems:
            problems = [p for p in problems if p.startswith("stderr")]
        else:
            problems = []
    return ("FAIL" if problems else "PASS"), problems, diffs


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("names", nargs="*")
    ap.add_argument("--aether", default=os.environ.get("AETHER_BIN", os.path.join(REPO, "build", "aether")))
    ap.add_argument("--dir", help="only fixtures under tests/fx/DIR ('.' for the top level)")
    ap.add_argument("--update", action="store_true")
    ap.add_argument("--fx-dir", help="fixture tree to run instead of tests/fx (for testing the runner)")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()
    aether = os.path.abspath(args.aether)
    if not (os.path.isfile(aether) and os.access(aether, os.X_OK)):
        print(f"aether binary not found: {aether}", file=sys.stderr)
        return 2
    if args.fx_dir:
        global FX
        FX = os.path.abspath(args.fx_dir)
    fixtures = find_fixtures()
    if args.dir is not None:
        d = os.path.normpath(args.dir)
        fixtures = [f for f in fixtures if os.path.dirname(f) == ("" if d == "." else d)
                    or (d != "." and f.startswith(d + os.sep))]
    if args.names:
        unknown = [n for n in args.names if n not in fixtures]
        if unknown:
            print(f"no such fixture: {', '.join(unknown)}", file=sys.stderr)
            return 2
        fixtures = [f for f in fixtures if f in args.names]
    if not fixtures:
        print("fixtures: none selected")
        return 0

    caps = aether_caps.detect(aether)
    work = tempfile.mkdtemp(prefix="aether_fx_")
    counts = {"PASS": 0, "FAIL": 0, "SKIP": 0}
    failed = []
    try:
        for name in fixtures:
            need = aether_caps.read_caps_file(os.path.join(FX, name + ".cap"))
            if need - caps:
                counts["SKIP"] += 1
                print(f"SKIP  {name}: needs {', '.join(sorted(need - caps))}")
                continue
            status, problems, diffs = check(name, aether, work, args.update)
            counts[status] += 1
            if status == "FAIL":
                failed.append(name)
                print(f"FAIL  {name}: " + "; ".join(problems))
            elif args.verbose or diffs:
                print(f"PASS  {name}")
            for d in diffs:
                print(d.rstrip("\n"))
    finally:
        shutil.rmtree(work, ignore_errors=True)
    print(f"fixtures: {counts['PASS']} passed, {counts['FAIL']} failed, {counts['SKIP']} skipped"
          + (" (sidecars updated: review the diff)" if args.update else ""))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
