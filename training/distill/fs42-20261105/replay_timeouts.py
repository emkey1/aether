#!/usr/bin/env python3
"""Re-grades the programs that hit the grader's limit in filter.json, as promised before the pass was read: it ran while the Mac was heavily loaded.

Run from a repo root on the Mac holding the raw filter.json: python3 replay_timeouts.py GRADER [--out replay_timeouts.json]. Prints no program or task text.
"""

from __future__ import annotations

import argparse
import dataclasses
import json
import pathlib
import sys

sys.path.insert(0, "tools")
import aether_doc_bench as bench  # noqa: E402

HERE = pathlib.Path(__file__).resolve().parent
LONG_LIMIT = 300


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("grader")
    ap.add_argument("--out", default=str(HERE / "replay_timeouts.json"))
    cli = ap.parse_args()

    tasks = {t.task_id: t for t in bench.load_tasks(HERE / "tasks_frozen.json")}
    report = json.loads((HERE / "filter.json").read_text(encoding="utf-8"))
    results = report["destinations"][0]["variants"][0]["results"]
    args = argparse.Namespace(aether_bin=cli.grader, aether_bin_display=cli.grader, binary_sha256=None, aether_flag=[], aether_flags=[])
    rows = []
    for result in results:
        attempt = result["attempts"][0]
        if not (attempt.get("run") or {}).get("timed_out"):
            continue
        task = tasks[result["task_id"]]
        same_limit = [bench.compile_and_run(task, attempt["source_code"], args) for _ in range(3)]
        long_limit = bench.compile_and_run(dataclasses.replace(task, timeout_seconds=LONG_LIMIT), attempt["source_code"], args)
        row = {
            "task_id": result["task_id"],
            "limit": task.timeout_seconds,
            "same_limit": [{"elapsed": r["elapsed_seconds"], "timed_out": r["timed_out"], "exact": r["exact_stdout_match"], "returncode": r["returncode"]} for r in same_limit],
            "long_limit": {"limit": LONG_LIMIT, "elapsed": long_limit["elapsed_seconds"], "timed_out": long_limit["timed_out"], "exact": long_limit["exact_stdout_match"], "returncode": long_limit["returncode"]},
        }
        rows.append(row)
        print(json.dumps(row))
    pathlib.Path(cli.out).write_text(json.dumps({"timeouts": rows}, indent=1) + "\n", encoding="utf-8")
    print(f"{len(rows)} programs replayed")


if __name__ == "__main__":
    main()
