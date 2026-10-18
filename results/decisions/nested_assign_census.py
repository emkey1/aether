#!/usr/bin/env python3
"""Census of archived attempts that declare a `fn` inside a function body.

Evidence for the nested-fn assignment proposal
(docs/proposals/2026-10-18-nested-fn-assignment.md). Reads every archived
report (bench/results/**/*.json[.gz] and training/training_runs/*/score/*.json.gz),
keeps the attempts whose source declares a `fn` inside another fn's body, and
grades each one on --aether-bin against its own task definition, the way
tools/replay_bench.py does (same task resolver, same compile_and_run, same
failure classes). With --lowered-dir, a file <sha12>.aether there is a hand
lowering of the attempt with that source sha256 prefix (the same program with
its assigned enclosing locals boxed in a record) and is graded against the
same task, to show what the attempt would do if the assignments were legal.

  python3 results/decisions/nested_assign_census.py --aether-bin build/aether \\
      --lowered-dir results/decisions/nested_assign_lowered --json out.json
"""

from __future__ import annotations

import argparse
import glob
import gzip
import hashlib
import json
import pathlib
import re
import shutil
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import aether_doc_bench as adb  # noqa: E402
import replay_bench as rb  # noqa: E402

PATTERNS = ["bench/results/**/*.json", "bench/results/**/*.json.gz",
            "training/training_runs/*/score/*.json.gz"]


def nested_fns(src: str) -> list[str]:
    """Names of the fns declared inside another fn's body. A brace scanner
    that skips strings and // comments: a `fn` opens an fn block, `type`
    opens a type block (its methods are not nested), any other `{` inherits
    the enclosing block's fn-ness."""
    out: list[str] = []
    stack: list[bool] = []          # per open brace: inside an fn body?
    pending: str | None = None      # kind of the next `{`: "fn", "type" or None
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if c == '"':
            i += 1
            while i < n and src[i] != '"':
                i += 2 if src[i] == "\\" else 1
            i += 1
            continue
        if src.startswith("//", i):
            while i < n and src[i] != "\n":
                i += 1
            continue
        if c == "{":
            in_fn = bool(stack and stack[-1])
            stack.append(True if pending == "fn" else (False if pending == "type" else in_fn))
            pending = None
            i += 1
            continue
        if c == "}":
            if stack:
                stack.pop()
            i += 1
            continue
        if (c.isalpha() or c == "_") and (i == 0 or not (src[i - 1].isalnum() or src[i - 1] == "_")):
            j = i
            while j < n and (src[j].isalnum() or src[j] == "_"):
                j += 1
            word = src[i:j]
            if word == "fn":
                m = re.match(r"\s+([A-Za-z_]\w*)\s*\(", src[j:])
                if m:
                    if stack and stack[-1]:
                        out.append(m.group(1))
                    pending = "fn"
            elif word == "type":
                pending = "type"
            i = j
            continue
        i += 1
    return out


def load(path: pathlib.Path):
    try:
        raw = gzip.decompress(path.read_bytes()) if path.suffix == ".gz" else path.read_bytes()
        return json.loads(raw.decode("utf-8"))
    except (OSError, ValueError):
        return None


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--aether-bin", type=pathlib.Path, default=ROOT / "build" / "aether")
    ap.add_argument("--lowered-dir", type=pathlib.Path)
    ap.add_argument("--sandbox-deny", default="net,proc")
    ap.add_argument("--json", type=pathlib.Path)
    args = ap.parse_args()
    args.aether_args = []
    run_dir = pathlib.Path(tempfile.mkdtemp(prefix="nested-census-"))
    try:
        toolchain = adb.snapshot_aether_binary(args.aether_bin, run_dir)
        args.aether_bin = toolchain.pop("path")
        args.aether_bin_display = toolchain["aether_bin"]
        args.binary_sha256 = toolchain["binary_sha256"]
        args.toolchain = toolchain
        resolver = rb.TaskResolver()
        paths = rb.iter_report_paths(PATTERNS, rb.DEFAULT_EXCLUDES)
        rows = []
        for path in paths:
            report = load(path)
            if not isinstance(report, dict) or "destinations" not in report:
                continue
            for dest in report.get("destinations", []):
                for variant in dest.get("variants", []):
                    for case in variant.get("results", []):
                        attempts = case.get("attempts") or []
                        if not any(nested_fns(a.get("source_code") or "") for a in attempts):
                            continue
                        raw, basis = resolver.resolve(report.get("tasks_file"), report.get("tasks_version"),
                                                      case.get("task_id"))
                        if raw is None:
                            continue
                        task = rb.to_task(raw)
                        for index, attempt in enumerate(attempts):
                            src = attempt.get("source_code") or ""
                            names = nested_fns(src)
                            if not names:
                                continue
                            sha = hashlib.sha256(src.encode("utf-8")).hexdigest()
                            new_run = adb.compile_and_run(task, src, args)
                            row = {
                                "report": adb.display_path(path), "model": dest.get("model"),
                                "destination": dest.get("destination_id"), "doc": variant.get("doc_name"),
                                "task_id": case.get("task_id"), "repeat": case.get("repeat_index", 0),
                                "attempt": index, "kind": "first" if index == 0 else "repair",
                                "sha12": sha[:12], "nested": names,
                                "stored_class": adb.classify_attempt(rb.stored_attempt_view(attempt, task)),
                                "stored_version": report.get("aether_version"),
                                "new_class": adb.classify_attempt({"generated_ok": True, "source_code": src,
                                                                   "run": new_run}),
                                "new_stderr": (new_run.get("stderr") or "").strip().splitlines()[:1],
                                "shown_error": ((((attempts[index - 1].get("run") or {}).get("stderr") or "")
                                                 .strip().splitlines() or [""])[0][:200]) if index else "",
                            }
                            lowered = args.lowered_dir / f"{sha[:12]}.aether" if args.lowered_dir else None
                            if lowered and lowered.exists():
                                low_run = adb.compile_and_run(task, lowered.read_text(encoding="utf-8"), args)
                                row["lowered_class"] = adb.classify_attempt(
                                    {"generated_ok": True, "source_code": "x", "run": low_run})
                                row["lowered_stderr"] = (low_run.get("stderr") or "").strip().splitlines()[:1]
                            rows.append(row)
    finally:
        shutil.rmtree(run_dir, ignore_errors=True)

    print(f"binary {toolchain.get('language_version')} {args.binary_sha256[:12]}: "
          f"{len(rows)} attempts with a nested fn")
    for r in rows:
        low = f" lowered={r['lowered_class']}" if "lowered_class" in r else ""
        why = r["new_stderr"][0][:110] if r["new_stderr"] and r["new_class"] != "pass" else ""
        print(f"  {r['model'] or r['destination']} {r['doc']} {r['task_id']} r{r['repeat']} a{r['attempt']} "
              f"{r['sha12']} {r['stored_class']}->{r['new_class']}{low} {why}")
    if args.json:
        args.json.write_text(json.dumps(rows, indent=1))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
