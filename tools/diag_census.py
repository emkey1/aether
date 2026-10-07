#!/usr/bin/env python3
"""Diagnostic census: what a failing program's diagnostics look like, measured.

Every assessment of Aether's diagnostics rested on unmeasured frequency
claims, so the W6 work could not be ranked and its effect on what a model sees
in a repair round could not be shown. This runs a fixed set of sources through
one binary and records, per source:

  compile_rc / run_rc    `--no-cache --no-run F`, then (if that compiles) a
                         `--deny net,proc` run with a 5 s timeout
  phase                  rc0 | compile | runtime | timeout | crash  (rc0: exit 0,
                         stdout not checked)
  coded                  the first error line carries a [CODE]
  hinted                 the first error's record has a `hint:` line
  has_path               the first error line starts with the file's name:line
  uncoded_backend        a line of backend vocabulary with no [CODE]
                         (^L<n>:, Compiler error, Runtime Error, UnicodeString,
                         Yyjson, INT64, POINTER, @<n>)
  first_severity/code    the first --diagnostics-json record of the failing phase
  json_null              --diagnostics-json records with code null (0 bytes of
                         JSON from a failing run counts as one)
  stderr_chars, over_1200  the failing phase's stderr, against the benchmark's
                         --repair-feedback-limit of 1,200 characters
  distinct_codes         distinct [CODE]s in that stderr

Sources come in groups:
  archive        every stored attempt in the umbrella's benchmark results
                 (--umbrella; resolved and given its task's files and stdin the
                 way tools/export_replay.py does)
  fail_fixtures  tests/*_fail*.aether
  mutants        single-token deletions of tests/*_pass.aether, sampled
                 deterministically (--mutants N), the review's deletion sweep
  <label>        --group label=GLOB for any other .aether files

Rates are over failing sources (phase compile or runtime), weighted by how often a source
occurs in its group. No model calls; about a minute.

  python3 tools/diag_census.py --umbrella <checkout> [--aether BIN] [--mutants 1500]
         [--csv out.csv] [--summary out.md] [--compare old.csv]
"""
import argparse
import csv
import glob
import hashlib
import json
import os
import random
import re
import shutil
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
TESTS = os.path.join(REPO, "tests")
TAG = re.compile(r"\[([A-Z]{2,10}-\d{3})\]")
BACKEND = re.compile(r"^L\d+:|Compiler error|Runtime Error|UnicodeString|Yyjson|INT64|POINTER|@\d")
LOCATED = re.compile(r"^\S+:\d+:")
FEEDBACK_LIMIT = 1200
RUN_TIMEOUT = 5
FIELDS = ["group", "name", "count", "sha8", "compile_rc", "run_rc", "phase", "coded", "hinted",
          "has_path", "uncoded_backend", "first_severity", "first_code", "json_null",
          "stderr_chars", "over_1200", "distinct_codes"]
RATES = [("coded", "coded first error"), ("hinted", "hinted first error"),
         ("has_path", "first error names file:line"), ("uncoded_backend", "uncoded backend line"),
         ("warning_first", "warning is the first JSON record"), ("over_1200", "stderr > 1,200 chars"),
         ("json_null_any", "JSON has a code:null record")]


# ----------------------------------------------------------------------------- sources
def sha(text):
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def archive_sources(umbrella):
    sys.path.insert(0, HERE)
    import export_replay  # noqa: E402
    adb, rb = export_replay.load_umbrella(umbrella)
    _, jobs, _ = export_replay.collect(adb, rb, [])
    by_key = {}
    for j in jobs:
        t = j["task"]
        key = (t.task_id, j["sha"])
        if key in by_key:
            by_key[key]["count"] += 1
            continue
        by_key[key] = {"group": "archive", "name": f"{t.task_id}__{j['sha'][:8]}",
                       "file": f"{t.task_id}.aether", "source": j["source"], "count": 1,
                       "files": t.files or {}, "cwd": t.cwd, "stdin": adb.task_stdin_text(t)}
    return list(by_key.values())


def file_sources(group, pattern):
    out = []
    for path in sorted(glob.glob(pattern)):
        with open(path, encoding="utf-8", errors="replace") as fh:
            src = fh.read()
        out.append({"group": group, "name": os.path.basename(path), "file": os.path.basename(path),
                    "source": src, "count": 1, "files": None, "cwd": None, "stdin": None,
                    "tests_cwd": True})
    return out


TOKEN = re.compile(r'"(?:\\.|[^"\\\n])*"?|//[^\n]*|/\*.*?\*/|\s+|\d+\.\d+|\d+|[A-Za-z_]\w*'
                   r'|\.\.|->|==|!=|<=|>=|&&|\|\||<<|>>|.', re.S)


def mutant_sources(total, seed=5):
    """Deterministic single-token deletions of the pass fixtures."""
    seeds = sorted(glob.glob(os.path.join(TESTS, "*_pass.aether")))
    cands = []
    for path in seeds:
        with open(path, encoding="utf-8", errors="replace") as fh:
            src = fh.read()
        toks = TOKEN.findall(src)
        for i, t in enumerate(toks):
            if t.isspace() or t.startswith("//") or t.startswith("/*"):
                continue
            cands.append((path, i, toks))
    rnd = random.Random(seed)
    picked = sorted(rnd.sample(range(len(cands)), min(total, len(cands))))
    out = []
    for k in picked:
        path, i, toks = cands[k]
        base = os.path.basename(path)[:-len(".aether")]
        out.append({"group": "mutants", "name": f"{base}#del{i}", "file": f"{base}_del{i}.aether",
                    "source": "".join(toks[:i] + toks[i + 1:]), "count": 1, "files": None,
                    "cwd": None, "stdin": None, "tests_cwd": True})
    return out


# ----------------------------------------------------------------------------- one source
def run(cmd, cwd, stdin, timeout):
    try:
        p = subprocess.run(cmd, cwd=cwd, input=(stdin or "").encode(), capture_output=True,
                           timeout=timeout)
        return p.returncode, p.stderr.decode("utf-8", "replace")
    except subprocess.TimeoutExpired:
        return "timeout", ""


CONTINUATION = ("hint:", "help:", "[Error Location]", " ", "\t")


def first_error(stderr):
    """(first error line, its record's lines). A record starts at a `file:N:`
    line or any other line that is not a hint/help/location continuation;
    warning records are skipped."""
    recs, cur = [], None
    for ln in stderr.splitlines():
        if not ln.strip():
            continue
        if cur is None or (not ln.startswith(CONTINUATION) and
                           (LOCATED.match(ln) or not cur or "warning:" in cur[0])):
            cur = [ln]
            recs.append(cur)
        else:
            cur.append(ln)
    for rec in recs:
        if "warning:" not in rec[0]:
            return rec[0], rec
    return "", []


def json_records(text):
    text = text.strip()
    start = text.find("[")
    if start == -1:
        return None
    try:
        recs = json.loads(text[start:])
    except json.JSONDecodeError:
        try:
            recs = json.loads(text[start:text.rfind("]") + 1])
        except json.JSONDecodeError:
            return None
    return recs if isinstance(recs, list) else None


def census_one(item, aether, work, tests_copy):
    if item.get("tests_cwd"):
        base = tests_copy
        cwd = tests_copy
        path = os.path.join(tests_copy, "__census_" + item["file"])
    else:
        base = tempfile.mkdtemp(prefix="src_", dir=work)
        for rel, content in (item["files"] or {}).items():
            target = os.path.join(base, rel)
            os.makedirs(os.path.dirname(target), exist_ok=True)
            with open(target, "w", encoding="utf-8") as fh:
                fh.write(content)
        cwd = os.path.join(base, item["cwd"]) if item["cwd"] else base
        os.makedirs(cwd, exist_ok=True)
        path = os.path.join(base, item["file"])
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(item["source"])
    stdin = item["stdin"]
    try:
        crc, cerr = run([aether, "--no-cache", "--no-run", path], cwd, stdin, 30)
        rrc, rerr = (None, "")
        if crc == 0:
            rrc, rerr = run([aether, "--no-cache", "--deny", "net,proc", path], cwd, stdin, RUN_TIMEOUT)
        if crc != 0:
            phase = "crash" if crc == "timeout" or crc < 0 or crc >= 128 else "compile"
            stderr, jcmd = cerr, [aether, "--no-cache", "--no-run", "--diagnostics-json", path]
        elif rrc == 0:
            phase, stderr, jcmd = "rc0", "", None
        else:
            phase = ("timeout" if rrc == "timeout" else
                     "crash" if rrc < 0 or rrc >= 128 else "runtime")
            stderr = rerr
            jcmd = [aether, "--no-cache", "--deny", "net,proc", "--diagnostics-json", path]
        recs = None
        if jcmd and phase in ("compile", "runtime"):
            _, jerr = run(jcmd, cwd, stdin, RUN_TIMEOUT if phase == "runtime" else 30)
            recs = json_records(jerr)
    finally:
        if item.get("tests_cwd"):
            os.unlink(path)
        else:
            shutil.rmtree(base, ignore_errors=True)
    row = {"group": item["group"], "name": item["name"], "count": item["count"],
           "sha8": sha(item["source"])[:8], "compile_rc": crc,
           "run_rc": "" if rrc is None else rrc, "phase": phase}
    if phase == "rc0":
        row.update({k: "" for k in FIELDS if k not in row})
        return row
    # Diagnostics name the file as the compiler was given it; report it by basename.
    stderr = stderr.replace(os.path.dirname(path) + os.sep, "").replace(
        os.path.dirname(path).lstrip("/") + os.sep, "")
    fname = os.path.basename(path)
    line, record = first_error(stderr)
    row["coded"] = int(bool(TAG.search(line)))
    row["hinted"] = int(any(r.lstrip().startswith("hint:") or " hint: " in r for r in record))
    row["has_path"] = int(bool(re.match(re.escape(fname) + r":\d+", line)))
    row["uncoded_backend"] = int(any(BACKEND.search(ln) and not TAG.search(ln)
                                     for ln in stderr.splitlines()))
    if recs:
        row["first_severity"] = recs[0].get("severity", "")
        row["first_code"] = recs[0].get("code") or "null"
        row["json_null"] = sum(1 for r in recs if isinstance(r, dict) and not r.get("code"))
    else:
        row["first_severity"], row["first_code"], row["json_null"] = "none", "none", 1
    row["stderr_chars"] = len(stderr)
    row["over_1200"] = int(len(stderr) > FEEDBACK_LIMIT)
    row["distinct_codes"] = len(set(TAG.findall(stderr)))
    return row


# ----------------------------------------------------------------------------- summary
def summarize(rows):
    groups = {}
    for r in rows:
        groups.setdefault(r["group"], []).append(r)
    out = {}
    for g, rs in list(groups.items()) + [("all", rows)]:
        w = lambda r: int(r["count"])  # noqa: E731
        total = sum(w(r) for r in rs)
        failing = [r for r in rs if r["phase"] != "rc0"]
        diag = [r for r in failing if r["phase"] in ("compile", "runtime")]
        nf = sum(w(r) for r in diag)
        s = {"sources": len(rs), "weight": total, "failing": sum(w(r) for r in failing),
             "compile": sum(w(r) for r in rs if r["phase"] == "compile"),
             "runtime": sum(w(r) for r in rs if r["phase"] == "runtime"),
             "timeout_crash": sum(w(r) for r in rs if r["phase"] in ("timeout", "crash")),
             "diagnosed": nf}
        for key, _ in RATES:
            if key == "warning_first":
                hit = sum(w(r) for r in diag if r["first_severity"] == "warning")
            elif key == "json_null_any":
                hit = sum(w(r) for r in diag if int(r["json_null"] or 0) > 0)
            else:
                hit = sum(w(r) for r in diag if str(r[key]) == "1")
            s[key] = hit / nf if nf else 0.0
        s["mean_distinct_codes"] = (sum(w(r) * int(r["distinct_codes"]) for r in diag) / nf) if nf else 0.0
        s["mean_stderr_chars"] = (sum(w(r) * int(r["stderr_chars"]) for r in diag) / nf) if nf else 0.0
        out[g] = s
    return out


def render(summary, title):
    groups = [g for g in summary if g != "all"] + ["all"]
    lines = [f"### {title}", "",
             "| | " + " | ".join(groups) + " |", "|---|" + "---|" * len(groups)]
    def row(label, fn):
        lines.append(f"| {label} | " + " | ".join(fn(summary[g]) for g in groups) + " |")
    row("sources (distinct)", lambda s: str(s["sources"]))
    row("weighted sources", lambda s: str(s["weight"]))
    row("failing (compile / runtime / timeout+crash)",
        lambda s: f"{s['failing']} ({s['compile']} / {s['runtime']} / {s['timeout_crash']})")
    for key, label in RATES:
        row(label, lambda s, k=key: f"{100 * s[k]:.1f}%")
    row("mean distinct codes", lambda s: f"{s['mean_distinct_codes']:.2f}")
    row("mean stderr chars", lambda s: f"{s['mean_stderr_chars']:.0f}")
    lines.append("")
    lines.append("Rates are over failing sources with a compile or runtime diagnostic "
                 "(timeouts and crashes are counted, not rated), weighted by occurrence.")
    return "\n".join(lines) + "\n"


def load_csv(path):
    with open(path, encoding="utf-8") as fh:
        return list(csv.DictReader(fh))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--aether", default=os.environ.get("AETHER_BIN", os.path.join(REPO, "build", "aether")))
    ap.add_argument("--umbrella", default=os.environ.get("PSCAL_UMBRELLA"),
                    help="umbrella checkout whose benchmark results form the archive group")
    ap.add_argument("--mutants", type=int, default=1500, help="deletion mutants to sample (0: none)")
    ap.add_argument("--group", action="append", default=[], metavar="LABEL=GLOB")
    ap.add_argument("--no-fixtures", action="store_true", help="leave out tests/*_fail*.aether")
    ap.add_argument("--workers", type=int, default=8)
    ap.add_argument("--csv", help="write one row per source here")
    ap.add_argument("--summary", help="write the summary table (Markdown) here")
    ap.add_argument("--title", help="summary heading (default: the binary's version)")
    ap.add_argument("--compare", help="a previous --csv: print the rate deltas against it")
    args = ap.parse_args()
    aether = os.path.abspath(args.aether)
    version = subprocess.run([aether, "--version"], capture_output=True, text=True).stdout.strip()

    items = []
    if args.umbrella:
        items += archive_sources(args.umbrella)
    else:
        print("no --umbrella: the archive group is left out", file=sys.stderr)
    if not args.no_fixtures:
        items += file_sources("fail_fixtures", os.path.join(TESTS, "*_fail*.aether"))
    for spec in args.group:
        label, _, pattern = spec.partition("=")
        items += file_sources(label, pattern)
    if args.mutants:
        items += mutant_sources(args.mutants)

    work = tempfile.mkdtemp(prefix="aether_census_")
    try:
        tests_copy = os.path.join(work, "tests")
        shutil.copytree(TESTS, tests_copy, ignore=shutil.ignore_patterns("replay", "backlog", "fx"))
        with ThreadPoolExecutor(max(1, args.workers)) as pool:
            rows = list(pool.map(lambda it: census_one(it, aether, work, tests_copy), items))
    finally:
        shutil.rmtree(work, ignore_errors=True)

    if args.csv:
        with open(args.csv, "w", encoding="utf-8", newline="") as fh:
            wr = csv.DictWriter(fh, FIELDS)
            wr.writeheader()
            wr.writerows(rows)
    summary = summarize([{k: str(v) for k, v in r.items()} for r in rows])
    text = render(summary, args.title or f"Diagnostic census: {version}")
    print(text)
    if args.summary:
        with open(args.summary, "w", encoding="utf-8") as fh:
            fh.write(text)
    if args.compare:
        old = summarize(load_csv(args.compare))
        print(f"### Deltas against {os.path.basename(args.compare)} (percentage points)\n")
        groups = [g for g in summary if g in old]
        print("| | " + " | ".join(groups) + " |\n|---|" + "---|" * len(groups))
        for key, label in RATES:
            print(f"| {label} | " + " | ".join(
                f"{100 * (summary[g][key] - old[g][key]):+.1f}" for g in groups) + " |")
    return 0


if __name__ == "__main__":
    sys.exit(main())
