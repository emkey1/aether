#!/usr/bin/env python3
"""Compile and run one set of Aether programs under several (binary, experiment)
arms and report what changed.

Two jobs, one tool:

  * Flags-off identity. Run a base binary and a candidate binary with no
    AETHER_EXPERIMENT set and require every program to produce the same
    `--dump-ast-json` output (stdout and stderr, so diagnostics are covered),
    the same run exit status and the same run stdout. Any difference is listed
    and the tool exits 1.

  * Experiment census. Run one binary under several AETHER_EXPERIMENT values
    (see src/aether/experiment.c) and tabulate, per arm: programs whose rc or
    stdout changed against the first arm, pass->fail and fail->pass against a
    golden stdout where one exists, and every [CODE] the arm's diagnostics
    carry. `--waivers` writes each changed program's old and new rc and stdout,
    the per-rule waiver file the D4 census asks for.

Sources:
  --tests DIR          every *.aether directly in DIR
  --examples DIR       every extensionless or *.aether file under DIR
  --dir DIR            every *.aether directly in DIR
  --corpus MANIFEST    a corpus manifest ({"items": [{"repo_path", "stdout"}]});
                       repo_path is resolved against --corpus-root
  --results GLOB       benchmark results JSON; every distinct `source_code`
                       found anywhere in it (first attempts and repairs)

Programs run in a fresh temporary directory (never in the source tree: some
programs write files), with HOME pointed at a private directory so the
bytecode cache is neither read from nor shared with the user's, with stdin
from /dev/null, and with --no-cache. A program whose source mentions the
network surface (http, sockets, ai_chat) is compiled but not run.

Examples:
  tools/aether_experiment_census.py --arm base=/path/base/aether: \\
      --arm head=build/aether: --tests tests --examples examples --identity
  tools/aether_experiment_census.py --arm current=build/aether: \\
      --arm int=build/aether:div=int --corpus M --corpus-root R --json out.json
An arm is NAME=BINARY:EXPERIMENT. EXPERIMENT (may be empty) is the
AETHER_EXPERIMENT value, optionally followed by ;KEY=VALUE environment
settings, e.g. `head=build/aether:;AETHER_DUMP_TYPES=/dev/null`.
"""
import argparse
import collections
import concurrent.futures
import glob
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

NETWORK_RE = re.compile(r"\b(http_|httpsession|httprequest|socket_|socket\w*\(|ai_chat|openai)",
                        re.IGNORECASE)
CODE_RE = re.compile(r"\[([A-Z]+-\d{3})\]")
TIMEOUT = 20


def sha(data):
    return hashlib.sha256(data).hexdigest()[:16]


def collect_sources(args):
    """Returns [(key, path, golden_stdout_or_None)]."""
    out = []
    for d in args.tests or []:
        for p in sorted(glob.glob(os.path.join(d, "*.aether"))):
            out.append(("tests/" + os.path.basename(p), os.path.abspath(p), None))
    for d in args.examples or []:
        for root, _dirs, files in os.walk(d):
            for f in sorted(files):
                if f.endswith((".md", ".json", ".txt", ".toon")):
                    continue
                p = os.path.join(root, f)
                if "." in f and not f.endswith(".aether"):
                    continue
                out.append(("examples/" + os.path.relpath(p, d), os.path.abspath(p), None))
    for d in args.dir or []:
        tag = os.path.basename(os.path.normpath(d))
        for p in sorted(glob.glob(os.path.join(d, "*.aether"))):
            out.append((tag + "/" + os.path.basename(p), os.path.abspath(p), None))
    for m in args.corpus or []:
        items = json.load(open(m)).get("items", [])
        root = args.corpus_root or os.path.dirname(os.path.abspath(m))
        for it in items:
            p = os.path.join(root, it["repo_path"])
            if os.path.isfile(p):
                out.append(("corpus/" + os.path.basename(p), p, it.get("stdout")))
    if args.results:
        store = os.path.join(args.workdir, "results_sources")
        os.makedirs(store, exist_ok=True)
        seen = {}

        def walk(x):
            if isinstance(x, dict):
                s = x.get("source_code")
                if isinstance(s, str) and s.strip():
                    seen.setdefault(hashlib.sha1(s.encode()).hexdigest(), s)
                for v in x.values():
                    walk(v)
            elif isinstance(x, list):
                for v in x:
                    walk(v)
        for g in args.results:
            for rf in sorted(glob.glob(g, recursive=True)):
                try:
                    walk(json.load(open(rf)))
                except (OSError, ValueError):
                    continue
        for h, s in sorted(seen.items()):
            p = os.path.join(store, h[:12] + ".aether")
            with open(p, "w") as f:
                f.write(s)
            out.append(("results/" + h[:12], p, None))
    return out


def run_one(binary, experiment, src, fixtures, home, run, timeout=TIMEOUT):
    env = dict(os.environ)
    env.pop("AETHER_EXPERIMENT", None)
    env.pop("AETHER_DUMP_TYPES", None)
    # EXPERIMENT is the AETHER_EXPERIMENT value, optionally followed by
    # ;KEY=VALUE environment settings (e.g. "div=int;AETHER_DUMP_TYPES=/dev/null").
    parts = experiment.split(";") if experiment else []
    if parts and parts[0]:
        env["AETHER_EXPERIMENT"] = parts[0]
    for extra in parts[1:]:
        k, _, v = extra.partition("=")
        if k:
            env[k] = v
    env["HOME"] = home
    res = {}
    with tempfile.TemporaryDirectory(dir=home) as cwd:
        if fixtures:
            for f in os.listdir(fixtures):
                fp = os.path.join(fixtures, f)
                if os.path.isfile(fp):
                    shutil.copy(fp, cwd)
        try:
            p = subprocess.run([binary, "--no-cache", "--dump-ast-json", src], cwd=cwd, env=env,
                               stdin=subprocess.DEVNULL, capture_output=True, timeout=timeout)
            res["ast"] = sha(p.stdout + b"\0" + p.stderr)
            res["ast_rc"] = p.returncode
        except subprocess.TimeoutExpired:
            res["ast"], res["ast_rc"] = "timeout", 124
        argv = [binary, "--no-cache"] + ([] if run else ["--no-run"]) + [src]
        try:
            p = subprocess.run(argv, cwd=cwd, env=env, stdin=subprocess.DEVNULL,
                               capture_output=True, timeout=timeout)
            res["rc"] = p.returncode
            res["stdout"] = p.stdout.decode("utf-8", "replace")
            res["stderr"] = p.stderr.decode("utf-8", "replace")
        except subprocess.TimeoutExpired as e:
            res["rc"] = 124
            res["stdout"] = (e.stdout or b"").decode("utf-8", "replace")
            res["stderr"] = ""
    res["codes"] = sorted(set(CODE_RE.findall(res["stderr"])))
    first = CODE_RE.search(res["stderr"])
    res["first_code"] = first.group(1) if first else None
    return res


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--arm", action="append", required=True, help="NAME=BINARY:EXPERIMENT")
    ap.add_argument("--tests", action="append")
    ap.add_argument("--examples", action="append")
    ap.add_argument("--dir", action="append")
    ap.add_argument("--corpus", action="append")
    ap.add_argument("--corpus-root")
    ap.add_argument("--results", action="append")
    ap.add_argument("--fixtures", help="files copied into every run directory")
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) - 2))
    ap.add_argument("--timeout", type=int, default=TIMEOUT, help="seconds per compile or run")
    ap.add_argument("--identity", action="store_true",
                    help="exit 1 unless every arm matches the first on ast, rc and stdout")
    ap.add_argument("--json", help="write per-program rows here")
    ap.add_argument("--waivers", help="write changed programs (old/new rc and stdout) here")
    ap.add_argument("--workdir", default=None)
    ap.add_argument("--nondeterministic", help="file of program keys (one per line) whose run "
                    "output differs between two runs of one binary; their run rc/stdout is "
                    "not compared (their AST still is)")
    args = ap.parse_args()

    arms = []
    for a in args.arm:
        name, rest = a.split("=", 1)
        binary, _, exp = rest.partition(":")
        arms.append((name, os.path.abspath(binary), exp))
    args.workdir = args.workdir or tempfile.mkdtemp(prefix="aether_census_")
    srcs = collect_sources(args)
    homes = os.path.join(args.workdir, "home")
    os.makedirs(homes, exist_ok=True)

    def job(item):
        key, path, golden = item
        try:
            text = open(path, encoding="utf-8", errors="replace").read()
        except OSError:
            text = ""
        run = not NETWORK_RE.search(text)
        row = {"key": key, "run": run, "golden": golden is not None}
        for name, binary, exp in arms:
            r = run_one(binary, exp, path, args.fixtures, homes, run, args.timeout)
            if golden is not None:
                r["pass"] = (r["rc"] == 0 and r["stdout"] == golden)
            row[name] = r
        return row

    rows = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as ex:
        for row in ex.map(job, srcs):
            rows.append(row)

    base = arms[0][0]
    noisy = set()
    if args.nondeterministic:
        noisy = {l.strip() for l in open(args.nondeterministic) if l.strip()}
    summary = {"programs": len(rows), "ran": sum(1 for r in rows if r["run"]), "arms": {}}
    waivers = {}
    diffs = []
    for name, binary, exp in arms:
        s = collections.Counter()
        codes = collections.Counter()
        first = collections.Counter()
        for r in rows:
            a, b = r[base], r[name]
            for c in b["codes"]:
                codes[c] += 1
            if b["first_code"]:
                first[b["first_code"]] += 1
            s["rc0"] += b["rc"] == 0
            if a["ast"] != b["ast"]:
                s["ast_changed"] += 1
            run_diff = (a["rc"] != b["rc"] or a["stdout"] != b["stdout"]) and r["key"] not in noisy
            if run_diff:
                s["run_changed"] += 1
                waivers.setdefault(name, []).append({
                    "key": r["key"], "old_rc": a["rc"], "new_rc": b["rc"],
                    "old_stdout": a["stdout"], "new_stdout": b["stdout"],
                    "new_first_code": b["first_code"]})
            if a["stderr"] != b["stderr"]:
                s["stderr_changed"] += 1
            if a["ast"] != b["ast"] or run_diff:
                if name != base:
                    diffs.append((name, r["key"]))
            if r["golden"]:
                s["golden"] += 1
                s["pass"] += b["pass"]
                if a["pass"] and not b["pass"]:
                    s["pass_to_fail"] += 1
                if b["pass"] and not a["pass"]:
                    s["fail_to_pass"] += 1
        summary["arms"][name] = {"binary_sha": sha(open(binary, "rb").read()),
                                 "experiment": exp, **dict(s),
                                 "codes": dict(codes.most_common()),
                                 "first_codes": dict(first.most_common())}
    print(json.dumps(summary, indent=1, sort_keys=True))
    if diffs:
        print("changed against %s:" % base)
        for name, key in diffs[:200]:
            print("  %s: %s" % (name, key))
    if args.json:
        with open(args.json, "w") as f:
            json.dump({"summary": summary, "rows": rows}, f, indent=1, sort_keys=True)
    if args.waivers:
        with open(args.waivers, "w") as f:
            json.dump(waivers, f, indent=1, sort_keys=True)
    if args.identity and diffs:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
