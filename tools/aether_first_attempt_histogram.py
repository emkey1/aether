#!/usr/bin/env python3
"""First-attempt failure histogram over stored benchmark results (the D12 gate,
W8-10).

For every result row in the given results JSON files, take its first attempt
(the initial generation, before any repair round) and classify it:

  pass          rc 0 and stdout equal to the task's expected stdout
  silent-wrong  rc 0, wrong stdout
  coded         rejected or failed with a [CODE] diagnostic (histogram key: the
                first code printed)
  uncoded       nonzero rc with no code (runtime errors, crashes, timeouts)
  no-source     the generation produced no program

Two readings:
  stored        the rc/stdout/stderr the benchmark recorded (the binary of the
                day)
  --recompile   recompile and run each first-attempt source with --aether-bin
                (optionally under --experiment, an AETHER_EXPERIMENT value), in
                a directory holding the task's `files`, and compare with the
                task's expected stdout, read from the tasks file the results
                name (resolved against --bench-root)

The D12 gate is FX-001's share of first-attempt failures: run the fx A/B only if
it is at least 5%.

    tools/aether_first_attempt_histogram.py --bench-root <umbrella> \\
        '<umbrella>/Tests/aether_doc_bench/results/**/*.json'
    tools/aether_first_attempt_histogram.py --bench-root <umbrella> \\
        --recompile --aether-bin build/aether '<umbrella>/.../results/**/*.json'
"""
import argparse
import collections
import glob
import json
import os
import re
import subprocess
import sys
import tempfile

CODE_RE = re.compile(r"\[([A-Z]+-\d{3})\]")


def load_expected(bench_root, tasks_file, cache):
    if tasks_file in cache:
        return cache[tasks_file]
    exp = {}
    path = os.path.join(bench_root, tasks_file) if bench_root else tasks_file
    try:
        for t in json.load(open(path)).get("tasks", []):
            if "expected_stdout" in t:
                exp[t["id"]] = (t["expected_stdout"], t.get("files") or {})
    except (OSError, ValueError):
        pass
    cache[tasks_file] = exp
    return exp


def classify(rc, stdout, stderr, expected, source):
    if not source:
        return "no-source", None
    if rc == 0:
        if expected is None:
            return "unknown-expected", None
        return ("pass", None) if stdout == expected else ("silent-wrong", None)
    m = CODE_RE.search(stderr or "")
    if m:
        return "coded", m.group(1)
    return "uncoded", None


def run(binary, experiment, source, files, timeout=20):
    env = dict(os.environ)
    env.pop("AETHER_EXPERIMENT", None)
    if experiment:
        env["AETHER_EXPERIMENT"] = experiment
    with tempfile.TemporaryDirectory() as d:
        env["HOME"] = d
        for name, text in files.items():
            fp = os.path.join(d, name)
            os.makedirs(os.path.dirname(fp), exist_ok=True)
            with open(fp, "w") as f:
                f.write(text)
        p = os.path.join(d, "attempt.aether")
        with open(p, "w") as f:
            f.write(source)
        try:
            r = subprocess.run([binary, "--no-cache", p], cwd=d, env=env, capture_output=True,
                               text=True, timeout=timeout, stdin=subprocess.DEVNULL)
            return r.returncode, r.stdout, r.stderr
        except subprocess.TimeoutExpired:
            return 124, "", ""


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("results", nargs="+", help="results JSON globs")
    ap.add_argument("--bench-root", help="directory the results' tasks_file paths are relative to")
    ap.add_argument("--recompile", action="store_true")
    ap.add_argument("--aether-bin")
    ap.add_argument("--experiment", default="")
    ap.add_argument("--json", help="write per-attempt rows here")
    args = ap.parse_args()

    cache = {}
    rows = []
    for g in args.results:
        for rf in sorted(glob.glob(g, recursive=True)):
            try:
                d = json.load(open(rf))
            except (OSError, ValueError):
                continue
            exp = load_expected(args.bench_root, d.get("tasks_file", ""), cache)
            for dest in d.get("destinations", []):
                for v in dest.get("variants", []):
                    for r in v.get("results", []):
                        attempts = r.get("attempts") or [r]
                        a = attempts[0]
                        source = a.get("source_code") or ""
                        run_rec = a.get("run") or {}
                        expected, files = exp.get(r.get("task_id"), (None, {}))
                        if args.recompile and source:
                            rc, out, err = run(args.aether_bin, args.experiment, source, files)
                        else:
                            try:
                                rc = int(run_rec.get("returncode", 1))
                            except (TypeError, ValueError):
                                rc = 1
                            out = run_rec.get("stdout") or ""
                            err = run_rec.get("stderr") or ""
                        cls, code = classify(rc, out, err, expected, source)
                        rows.append({"file": os.path.basename(rf), "task": r.get("task_id"),
                                     "destination": dest.get("destination_id"),
                                     "doc": v.get("doc_name"), "class": cls, "code": code})
    classes = collections.Counter(r["class"] for r in rows)
    codes = collections.Counter(r["code"] for r in rows if r["class"] == "coded")
    graded = [r for r in rows if r["class"] != "unknown-expected"]
    failures = [r for r in graded if r["class"] != "pass"]
    fx = sum(1 for r in failures if r["code"] == "FX-001")
    print("first attempts: %d (graded %d)" % (len(rows), len(graded)))
    for k, n in classes.most_common():
        print("  %-16s %4d" % (k, n))
    print("first coded diagnostic of a failing first attempt:")
    for k, n in codes.most_common():
        print("  %-16s %4d" % (k, n))
    share = (100.0 * fx / len(failures)) if failures else 0.0
    print("FX-001 share of first-attempt failures: %d / %d = %.1f%%" % (fx, len(failures), share))
    sourced = [r for r in failures if r["class"] != "no-source"]
    share2 = (100.0 * fx / len(sourced)) if sourced else 0.0
    print("FX-001 share of failing first attempts that produced a program: %d / %d = %.1f%%"
          % (fx, len(sourced), share2))
    per_dest = collections.defaultdict(lambda: [0, 0])
    for r in failures:
        per_dest[r["destination"]][1] += 1
        if r["code"] == "FX-001":
            per_dest[r["destination"]][0] += 1
    print("by destination (FX-001 / failures):")
    for k, (a, b) in sorted(per_dest.items()):
        print("  %-40s %3d / %3d" % (k, a, b))
    if args.json:
        with open(args.json, "w") as f:
            json.dump(rows, f, indent=1)
    return 0


if __name__ == "__main__":
    sys.exit(main())
