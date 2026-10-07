#!/usr/bin/env python3
"""Freeze the replayed passing benchmark programs as aether's replay fixture.

About 245 real model-written programs with known-correct stdout exist only in
the umbrella's benchmark results, so aether's own tests could not see a silent
semantic change in natural programs. This exports them: it re-runs every
stored attempt the way the umbrella's tools/replay_bench.py does (same task
resolution, same compile_and_run, same --deny net,proc sandbox), keeps the
attempts that pass on the given binary, deduplicates them by (task, source
sha256), and writes

  tests/replay/<task>__<sha8>.aether   the program, byte for byte
  tests/replay/<task>__<sha8>.out      the task's expected stdout
  tests/replay/<task>__<sha8>.in       its stdin, when the task has one
  tests/replay/files/<task>-<hash8>/   the task's files, when it has any
  tests/replay/MANIFEST.tsv            case, task, files dir, cwd, rc, timeout
  tests/replay/EXPORT                  what was exported, from what, on what

tests/run_replay.sh (CTest aether_replay) replays them. Regenerate only
through this export, never by hand. A declared language break is not
re-exported away: list the case in tests/replay/WAIVED with the CHANGELOG
version that declared it; the export keeps WAIVED lines whose case it still
writes and reports the ones it drops.

The umbrella checkout is only read (Python bytecode caching is off so its
tools/ stays untouched):

  python3 tools/export_replay.py --umbrella <umbrella checkout> [--aether build/aether]
         [--workers 8] [result globs ...] [--dry-run]
"""
import argparse
import hashlib
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import types

sys.dont_write_bytecode = True

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parent
OUT = REPO / "tests" / "replay"


def sha256_text(text):
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def load_umbrella(path):
    tools = pathlib.Path(path).resolve() / "tools"
    if not (tools / "replay_bench.py").is_file():
        raise SystemExit(f"{tools}/replay_bench.py not found: --umbrella must name an umbrella checkout")
    sys.path.insert(0, str(tools))
    import aether_doc_bench as adb  # noqa: E402
    import replay_bench as rb  # noqa: E402
    return adb, rb


def collect(adb, rb, patterns):
    """[(job dict)] for every stored aether attempt with a resolvable task."""
    paths = rb.iter_report_paths(patterns or rb.DEFAULT_RESULTS, rb.DEFAULT_EXCLUDES)
    resolver = rb.TaskResolver()
    jobs, skipped = [], {}
    for path in paths:
        try:
            report = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            continue
        if not isinstance(report, dict) or "destinations" not in report:
            continue
        for dest in report.get("destinations", []):
            for variant in dest.get("variants", []):
                for case in variant.get("results", []):
                    raw, basis = resolver.resolve(report.get("tasks_file"), report.get("tasks_version"),
                                                  case.get("task_id"))
                    if raw is None:
                        skipped[basis] = skipped.get(basis, 0) + 1
                        continue
                    for attempt in case.get("attempts") or []:
                        src = attempt.get("source_code") or ""
                        if not src.strip() or attempt.get("runner", "aether") != "aether":
                            continue
                        jobs.append({"raw": raw, "task": rb.to_task(raw), "source": src,
                                     "sha": sha256_text(src), "report": adb.display_path(path)})
    return paths, jobs, skipped


def task_key(task):
    fields = {"task_id": task.task_id, "expected_stdout": task.expected_stdout, "files": task.files,
              "cwd": task.cwd, "stdin": task.stdin, "expected_returncode": task.expected_returncode}
    return sha256_text(json.dumps(fields, sort_keys=True, default=str))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("results", nargs="*", help="result-JSON globs (default: the umbrella's tracked results)")
    ap.add_argument("--umbrella", default=os.environ.get("PSCAL_UMBRELLA"),
                    help="umbrella checkout to read (default $PSCAL_UMBRELLA)")
    ap.add_argument("--aether", default=str(REPO / "build" / "aether"))
    ap.add_argument("--workers", type=int, default=8)
    ap.add_argument("--dry-run", action="store_true", help="report the counts, write nothing")
    args = ap.parse_args()
    if not args.umbrella:
        raise SystemExit("--umbrella (or PSCAL_UMBRELLA) must name an umbrella checkout")
    adb, rb = load_umbrella(args.umbrella)

    run_dir = pathlib.Path(tempfile.mkdtemp(prefix="aether-replay-export-"))
    try:
        toolchain = adb.snapshot_aether_binary(pathlib.Path(args.aether), run_dir)
        ns = types.SimpleNamespace(aether_bin=toolchain["path"], aether_bin_display="aether",
                                   binary_sha256=toolchain["binary_sha256"], sandbox_deny="net,proc",
                                   aether_args=[])
        paths, jobs, skipped = collect(adb, rb, args.results)
        unique = {}
        for job in jobs:
            unique.setdefault((task_key(job["task"]), job["sha"]), job)
        import concurrent.futures
        with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, args.workers)) as pool:
            futures = {k: pool.submit(adb.compile_and_run, j["task"], j["source"], ns)
                       for k, j in unique.items()}
            results = {k: f.result() for k, f in futures.items()}
    finally:
        shutil.rmtree(run_dir, ignore_errors=True)

    cases = {}
    for key, job in sorted(unique.items(), key=lambda kv: (kv[1]["task"].task_id, kv[1]["sha"])):
        if not results[key]["exact_stdout_match"]:
            continue
        name = f"{job['task'].task_id}__{job['sha'][:8]}"
        if name in cases:  # the same program passing under two task definitions
            continue
        cases[name] = job
    failing = sum(1 for k in unique if not results[k]["exact_stdout_match"])
    print(f"replay export on aether {toolchain['aether_version']} "
          f"(sha256 {toolchain['binary_sha256'][:16]}): {len(paths)} report(s), {len(jobs)} attempts, "
          f"{len(unique)} distinct (task, program) pairs, {len(cases)} passing exported, "
          f"{failing} not passing")
    if skipped:
        print(f"  skipped cases (task unresolved): {skipped}")
    if args.dry_run:
        return 0

    waived_path = OUT / "WAIVED"
    waived_lines = waived_path.read_text(encoding="utf-8").splitlines() if waived_path.exists() else []
    if OUT.exists():
        for child in OUT.iterdir():
            if child.name in ("WAIVED",):
                continue
            shutil.rmtree(child) if child.is_dir() else child.unlink()
    OUT.mkdir(parents=True, exist_ok=True)
    rows = []
    for name, job in cases.items():
        task = job["task"]
        (OUT / f"{name}.aether").write_text(job["source"], encoding="utf-8")
        (OUT / f"{name}.out").write_text(task.expected_stdout, encoding="utf-8")
        stdin = adb.task_stdin_text(task)
        if stdin is not None:
            (OUT / f"{name}.in").write_text(stdin, encoding="utf-8")
        files_dir = "-"
        if task.files:
            files_dir = f"{task.task_id}-{sha256_text(json.dumps(task.files, sort_keys=True))[:8]}"
            root = OUT / "files" / files_dir
            if not root.exists():
                for rel, content in task.files.items():
                    target = root / rel
                    target.parent.mkdir(parents=True, exist_ok=True)
                    target.write_text(content, encoding="utf-8")
        rows.append("\t".join([name, task.task_id, files_dir, task.cwd or "-",
                               str(task.expected_returncode), str(task.timeout_seconds)]))
    with open(OUT / "MANIFEST.tsv", "w", encoding="utf-8") as fh:
        fh.write("# Generated by tools/export_replay.py -- regenerate, never edit.\n")
        fh.write("# case\ttask_id\tfiles_dir\tcwd\texpected_rc\ttimeout_seconds\n")
        fh.write("\n".join(rows) + "\n")
    umbrella_head = subprocess.run(["git", "-C", args.umbrella, "rev-parse", "--short=12", "HEAD"],
                                   capture_output=True, text=True).stdout.strip() or "unknown"
    reports = sorted({j["report"] for j in cases.values()})
    with open(OUT / "EXPORT", "w", encoding="utf-8") as fh:
        fh.write("# Generated by tools/export_replay.py -- regenerate, never edit.\n")
        fh.write(f"language_version {toolchain['language_version']}\n")
        fh.write(f"aether_version {toolchain['aether_version']}\n")
        fh.write(f"umbrella_commit {umbrella_head}\n")
        fh.write("sandbox --deny net,proc --no-cache\n")
        fh.write(f"attempts {len(jobs)}\ndistinct {len(unique)}\nexported {len(cases)}\n")
        for r in reports:
            fh.write(f"report {r}\n")
    kept, dropped = [], []
    for line in waived_lines:
        word = line.split("#", 1)[0].split()
        if word and word[0] not in cases:
            dropped.append(line)
        else:
            kept.append(line)
    if not kept:
        kept = ["# Declared breaks: `<case> <CHANGELOG version> <reason>`, one per line.",
                "# tests/run_replay.sh expects a listed case to fail and fails when it passes.",
                "# Add a line in the commit whose language change breaks the case; the next",
                "# tools/export_replay.py run drops cases that no longer pass, and their lines."]
    waived_path.write_text("\n".join(kept) + "\n", encoding="utf-8")
    for line in dropped:
        print(f"  WAIVED line dropped (case no longer exported): {line}")
    print(f"wrote {len(cases)} case(s) to {OUT.relative_to(REPO)}/; review `git status` and the diff")
    return 0


if __name__ == "__main__":
    sys.exit(main())
