#!/usr/bin/env python3
"""Corpus A/B: run every program in a corpus directory under an old and a new
aether binary and report the programs whose stdout or exit status changed.

The pin gate (tools/pin_gate.sh) runs this with the base binary (aether HEAD at
HEAD's pins) as --old and the candidate as --new, over the umbrella's
Tests/aether_specialization/corpus_candidates. Every regular file in the
directory is taken as an Aether program (corpus files do not all end in
.aether). The directory is only read: each program runs with --no-cache, stdin
from /dev/null, in a fresh scratch working directory.

Deterministic subset: each program runs twice under --old. A program whose two
old runs disagree (a clock, random, a par race), or that times out, is
reported and left out of the comparison. The rest run once under --new. A
change in stdout or exit status (stderr too with --stderr) is confirmed before
it counts as a diff: three more old runs must agree and two more new runs must
all differ, otherwise the program is reported as nondeterministic.

A release that changes output on purpose (the INT64 fix, say) lists the
programs it expects to change in an --expect-diff file, one `<name>  # why`
per line; those diffs are explained. A listed program that no longer differs
is reported as stale.

Usage:
    tools/corpus_ab.py --old BIN --new BIN --corpus DIR [--jobs N] [--limit N]
                       [--match GLOB] [--timeout S] [--stderr] [--expect-diff FILE] [-v]
Exit status: 0 no unexplained diff, 1 an unexplained diff or a crash of the
runner itself, 2 usage error. The last line starts with `corpus-ab:`.
"""
import argparse
import concurrent.futures
import fnmatch
import os
import shutil
import subprocess
import sys
import tempfile


def run_once(binary, src, timeout, work):
    cwd = tempfile.mkdtemp(prefix="ab_", dir=work)
    env = dict(os.environ)
    env["HOME"] = cwd  # nothing reaches the caller's ~/.pscal
    try:
        p = subprocess.run([binary, "--no-cache", src], stdin=subprocess.DEVNULL,
                           capture_output=True, cwd=cwd, env=env, timeout=timeout)
        result = (p.returncode, p.stdout, p.stderr)
    except subprocess.TimeoutExpired:
        result = ("timeout", b"", b"")
    shutil.rmtree(cwd, ignore_errors=True)
    return result


def compare(name, src, args, work):
    old1 = run_once(args.old, src, args.timeout, work)
    if old1[0] == "timeout":
        return name, "timeout", ""
    old2 = run_once(args.old, src, args.timeout, work)
    key = (lambda r: r) if args.stderr else (lambda r: r[:2])
    if key(old1) != key(old2):
        return name, "nondeterministic", ""
    new = run_once(args.new, src, args.timeout, work)
    if key(new) == key(old1):
        return name, "same", ""
    # Confirm before counting it: two agreeing old runs can still be luck (a
    # time-seeded random walk). Three more old runs must agree, and two more
    # new runs must all differ from them.
    olds = [run_once(args.old, src, args.timeout, work) for _ in range(3)]
    if any(key(o) != key(old1) for o in olds):
        return name, "nondeterministic", ""
    news = [run_once(args.new, src, args.timeout, work) for _ in range(2)]
    if any(key(n) == key(old1) for n in news):
        return name, "nondeterministic", ""
    parts = []
    if new[0] != old1[0]:
        parts.append(f"exit {old1[0]} -> {new[0]}")
    if new[1] != old1[1]:
        a = old1[1].decode("utf-8", "replace").splitlines()
        b = new[1].decode("utf-8", "replace").splitlines()
        for i in range(max(len(a), len(b))):
            x = a[i] if i < len(a) else "<missing>"
            y = b[i] if i < len(b) else "<missing>"
            if x != y:
                parts.append(f"stdout line {i + 1}: {x[:80]!r} -> {y[:80]!r}")
                break
    if args.stderr and new[2] != old1[2]:
        parts.append("stderr changed")
    return name, "diff", "; ".join(parts)


def load_expected(path):
    names = {}
    if not path:
        return names
    with open(path, encoding="utf-8") as fh:
        for raw in fh:
            body, _, why = raw.partition("#")
            body = body.strip()
            if body:
                names[body] = why.strip()
    return names


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--old", required=True, help="base aether binary")
    ap.add_argument("--new", required=True, help="candidate aether binary")
    ap.add_argument("--corpus", required=True, help="directory of programs (read only)")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--limit", type=int, default=0, help="first N programs in name order (0 = all)")
    ap.add_argument("--match", default="*", help="only programs whose file name matches this glob")
    ap.add_argument("--timeout", type=float, default=10.0, help="seconds per run")
    ap.add_argument("--stderr", action="store_true", help="compare stderr too")
    ap.add_argument("--expect-diff", help="file of program names expected to differ")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    for b in (args.old, args.new):
        if not (os.path.isfile(b) and os.access(b, os.X_OK)):
            print(f"not an executable: {b}", file=sys.stderr)
            return 2
    if not os.path.isdir(args.corpus):
        print(f"not a directory: {args.corpus}", file=sys.stderr)
        return 2
    args.old = os.path.abspath(args.old)
    args.new = os.path.abspath(args.new)
    expected = load_expected(args.expect_diff)

    names = sorted(n for n in os.listdir(args.corpus)
                   if os.path.isfile(os.path.join(args.corpus, n))
                   and not n.startswith(".") and fnmatch.fnmatch(n, args.match))
    if args.limit:
        names = names[:args.limit]

    work = tempfile.mkdtemp(prefix="corpus_ab_")
    results = []
    try:
        with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
            futs = [pool.submit(compare, n, os.path.abspath(os.path.join(args.corpus, n)), args, work)
                    for n in names]
            for f in concurrent.futures.as_completed(futs):
                results.append(f.result())
    finally:
        shutil.rmtree(work, ignore_errors=True)
    results.sort()

    counts = {"same": 0, "diff": 0, "nondeterministic": 0, "timeout": 0}
    unexplained, explained = [], []
    for name, status, detail in results:
        counts[status] += 1
        if status == "diff":
            (explained if name in expected else unexplained).append((name, detail))
        elif status != "same" or args.verbose:
            print(f"{status:16} {name}")
    for name, detail in explained:
        print(f"diff (expected)  {name}: {detail}  [{expected[name]}]")
    for name, detail in unexplained:
        print(f"DIFF             {name}: {detail}")
    differing = {n for n, _ in explained} | {n for n, _ in unexplained}
    stale = sorted(n for n in expected if n in names and n not in differing)
    for name in stale:
        print(f"stale expectation {name}: listed in --expect-diff but unchanged")

    compared = counts["same"] + counts["diff"]
    print(f"corpus-ab: {len(names)} programs, {compared} compared, {counts['same']} same, "
          f"{len(unexplained)} unexplained diff(s), {len(explained)} expected diff(s), "
          f"{counts['nondeterministic']} nondeterministic and {counts['timeout']} timed out (left out)")
    return 1 if unexplained else 0


if __name__ == "__main__":
    sys.exit(main())
