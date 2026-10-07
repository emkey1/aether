#!/usr/bin/env python3
"""Backend conformance pack: run the fixtures listed in manifest.txt.

Two Aether regressions arrived through pin bumps that no Aether test caught
(INT32 truncation from pscal-core 95efdcb, the per-access ArrayObj leak from
62d4139). These fixtures pin what Aether programs need from the shared engine,
and the known engine defects are listed as expected failures (`xfail:<item>`)
that must flip to `pass` in the commit whose pin bump fixes them.

Result per fixture:
  PASS    expected pass, passed
  XFAIL   expected failure, failed (the defect is still there)
  FAIL    expected pass, failed: a regression
  XPASS   expected failure, passed: the defect is fixed, so change the manifest
          line to `pass` in this commit
  REPORT  a D19 probe: output printed beside the recorded `.today` lines
  SKIP    a probe whose interpreter was not given

Usage:
    python3 tests/backend_conformance/run.py [--aether BIN] [--pascal BIN] [--clike BIN]
                                             [--only NAME ...] [--no-cache-modes] [-v]
Exit status: 0 no FAIL or XPASS, 1 otherwise, 2 setup error.
The last line, `expected-fail: ...`, lists the XFAIL fixtures with their items.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
MANIFEST = os.path.join(HERE, "manifest.txt")
TIMEOUT = 60
RSS_SIZES = (8000, 200000)
RSS_BYTES_PER_ITER = 8.0


def load_manifest():
    entries = []
    with open(MANIFEST, encoding="utf-8") as fh:
        for lineno, raw in enumerate(fh, 1):
            line = raw.split("#", 1)[0].rstrip()
            if not line.strip():
                continue
            parts = line.split(None, 3)
            if len(parts) < 3:
                raise SystemExit(f"manifest.txt:{lineno}: need <name> <kind> <expect>")
            name, kind, expect = parts[:3]
            note = parts[3] if len(parts) > 3 else ""
            if kind not in ("stdout", "rss", "probe"):
                raise SystemExit(f"manifest.txt:{lineno}: unknown kind {kind!r}")
            if kind == "probe":
                if expect != "report":
                    raise SystemExit(f"manifest.txt:{lineno}: a probe's expect is `report`")
            elif expect != "pass" and not expect.startswith("xfail:"):
                raise SystemExit(f"manifest.txt:{lineno}: expect is pass or xfail:<item>")
            entries.append({"name": name, "kind": kind, "expect": expect, "note": note})
    return entries


def read_opt(path, mode="r"):
    if not os.path.exists(path):
        return None
    with open(path, mode) as fh:
        return fh.read()


def run_proc(cmd, stdin_bytes, cwd, env):
    try:
        p = subprocess.run(cmd, input=stdin_bytes, capture_output=True, cwd=cwd,
                           env=env, timeout=TIMEOUT)
        return p.returncode, p.stdout, p.stderr
    except subprocess.TimeoutExpired:
        return "timeout", b"", b""


def first_diff(expected, actual):
    exp = expected.decode("utf-8", "replace").splitlines()
    act = actual.decode("utf-8", "replace").splitlines()
    for i in range(max(len(exp), len(act))):
        e = exp[i] if i < len(exp) else "<missing>"
        a = act[i] if i < len(act) else "<missing>"
        if e != a:
            return f"line {i + 1}: expected {e!r}, got {a!r}"
    return "trailing bytes differ"


def check_stdout(entry, aether, work, modes, verbose):
    """Return (passed, detail) for a stdout fixture over every run mode."""
    base = os.path.join(HERE, entry["name"])
    src = base + ".aether"
    expected = read_opt(base + ".out", "rb")
    if expected is None or not os.path.exists(src):
        raise SystemExit(f"{entry['name']}: missing .aether or .out")
    rc_text = read_opt(base + ".rc")
    want_rc = int(rc_text.strip()) if rc_text else 0
    stdin = read_opt(base + ".in", "rb") or b""
    needles = [n.strip().lower() for n in (read_opt(base + ".stderr_has") or "").splitlines() if n.strip()]

    runs = [("no-cache", ["--no-cache"], None)]
    if modes:
        home = tempfile.mkdtemp(prefix="cache_home_", dir=work)
        runs += [("cold-cache", [], home), ("warm-cache", [], home)]
    problems = []
    outputs = set()
    for label, flags, home in runs:
        env = dict(os.environ)
        if home:
            env["HOME"] = home
        cwd = tempfile.mkdtemp(prefix="cwd_", dir=work)
        rc, out, err = run_proc([aether] + flags + [src], stdin, cwd, env)
        outputs.add((rc, out))
        why = []
        if rc != want_rc:
            why.append(f"exit {rc}, want {want_rc}")
        if out != expected:
            if b"\x1b" in out:
                why.append("stdout carries ESC bytes")
            why.append(first_diff(expected, out))
        err_l = err.decode("utf-8", "replace").lower()
        for n in needles:
            if n not in err_l:
                why.append(f"stderr lacks {n!r}")
        problems.append((label, "; ".join(why)))
        if verbose:
            print(f"    {label}: rc={rc} stdout={out[:200]!r}")
    failed = [(label, why) for label, why in problems if why]
    if not failed:
        return True, ""
    if len(failed) == len(runs) and len({why for _, why in failed}) == 1:
        detail = failed[0][1] + (" (every run)" if len(runs) > 1 else "")
    else:
        detail = " | ".join(f"[{label}] {why or 'ok'}" for label, why in problems)
    if len(outputs) > 1:
        detail += " | no-cache and cached runs differ"
    return False, detail


def max_rss_bytes(cmd, cwd):
    """Run cmd and return (exit code, stdout, peak RSS in bytes) for that child."""
    with tempfile.TemporaryFile() as out:
        p = subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=out,
                             stderr=subprocess.DEVNULL, cwd=cwd)
        _, status, usage = os.wait4(p.pid, 0)
        p.returncode = os.waitstatus_to_exitcode(status)
        out.seek(0)
        data = out.read()
    rss = usage.ru_maxrss
    if sys.platform != "darwin":
        rss *= 1024  # Linux reports KiB, macOS bytes
    return p.returncode, data, rss


def check_rss(entry, aether, work, verbose):
    base = os.path.join(HERE, entry["name"])
    tmpl = read_opt(base + ".aether.tmpl")
    expected = read_opt(base + ".out", "rb")
    if tmpl is None or expected is None:
        raise SystemExit(f"{entry['name']}: missing .aether.tmpl or .out")
    measured = []
    for n in RSS_SIZES:
        src = os.path.join(work, f"{entry['name']}_{n}.aether")
        with open(src, "w", encoding="utf-8") as fh:
            fh.write(tmpl.replace("@N@", str(n)))
        rc, out, rss = max_rss_bytes([aether, "--no-cache", src], work)
        if rc != 0 or out != expected:
            return False, f"N={n}: exit {rc}, stdout {out[:80]!r}"
        measured.append(rss)
    per_iter = (measured[1] - measured[0]) / float(RSS_SIZES[1] - RSS_SIZES[0])
    detail = (f"peak RSS {measured[0] // 1024} KiB at N={RSS_SIZES[0]}, "
              f"{measured[1] // 1024} KiB at N={RSS_SIZES[1]}: {per_iter:.1f} B/iteration "
              f"(limit {RSS_BYTES_PER_ITER:g})")
    if verbose:
        print("    " + detail)
    return per_iter <= RSS_BYTES_PER_ITER, detail


def run_probe(entry, bins, work):
    path = os.path.join(HERE, entry["name"])
    lang = "pascal" if path.endswith(".pas") else "clike"
    binary = bins.get(lang)
    if not binary:
        return "SKIP", f"no --{lang} binary"
    env = dict(os.environ)
    env["HOME"] = tempfile.mkdtemp(prefix="probe_home_", dir=work)
    rc, out, err = run_proc([binary, path], b"", work, env)
    got = out.decode("utf-8", "replace").splitlines()
    today_text = read_opt(os.path.splitext(path)[0] + ".today") or ""
    today = today_text.splitlines()
    lines = [f"exit {rc}"]
    moved = 0
    for i in range(max(len(got), len(today))):
        g = got[i] if i < len(got) else "<missing>"
        t = today[i] if i < len(today) else "<missing>"
        mark = "  " if g == t else "* "
        moved += g != t
        lines.append(f"{mark}{g}" + ("" if g == t else f"   (recorded: {t})"))
    head = "unchanged from the recorded lines" if not moved else f"{moved} line(s) moved (*)"
    return "REPORT", head + "\n      " + "\n      ".join(lines)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--aether", default=os.environ.get("AETHER_BIN", os.path.join(REPO, "build", "aether")))
    ap.add_argument("--pascal", default=os.environ.get("PASCAL_BIN"))
    ap.add_argument("--clike", default=os.environ.get("CLIKE_BIN"))
    ap.add_argument("--only", nargs="*", help="run only these fixture names")
    ap.add_argument("--no-cache-modes", action="store_true",
                    help="run each stdout fixture with --no-cache only (skip the cold/warm cache runs)")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    if not (os.path.isfile(args.aether) and os.access(args.aether, os.X_OK)):
        print(f"aether binary not found: {args.aether}", file=sys.stderr)
        return 2
    entries = load_manifest()
    if args.only:
        entries = [e for e in entries if e["name"] in args.only]
    bins = {"pascal": args.pascal, "clike": args.clike}

    work = tempfile.mkdtemp(prefix="aether_conformance_")
    counts = {}
    xfails = []
    bad = []
    try:
        for e in entries:
            name, kind, expect = e["name"], e["kind"], e["expect"]
            if kind == "probe":
                status, detail = run_probe(e, bins, work)
                print(f"{status:6} {name}: {detail}")
                counts[status] = counts.get(status, 0) + 1
                continue
            if kind == "stdout":
                ok, detail = check_stdout(e, args.aether, work, not args.no_cache_modes, args.verbose)
            else:
                ok, detail = check_rss(e, args.aether, work, args.verbose)
            if expect == "pass":
                status = "PASS" if ok else "FAIL"
            else:
                status = "XPASS" if ok else "XFAIL"
            counts[status] = counts.get(status, 0) + 1
            item = expect.split(":", 1)[1] if expect.startswith("xfail:") else ""
            if status == "PASS":
                print(f"PASS   {name}")
            elif status == "XFAIL":
                xfails.append(f"{name}({item})")
                print(f"XFAIL  {name} [{item}: {e['note']}]: {detail}")
            elif status == "XPASS":
                bad.append(name)
                print(f"XPASS  {name} [{item}]: passes on this build. Change its manifest.txt "
                      f"line to `pass` in the commit that brings the fix.")
            else:
                bad.append(name)
                print(f"FAIL   {name}: {detail}")
    finally:
        shutil.rmtree(work, ignore_errors=True)

    summary = ", ".join(f"{counts[k]} {k.lower()}" for k in
                        ("PASS", "XFAIL", "FAIL", "XPASS", "REPORT", "SKIP") if counts.get(k))
    print(f"conformance: {summary}")
    print("expected-fail: " + (" ".join(xfails) if xfails else "none"))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
