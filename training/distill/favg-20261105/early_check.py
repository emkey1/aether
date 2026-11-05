#!/usr/bin/env python3
"""The early stop fixed in README.md. Exits 2 to stop, 0 when the pass has cleared the check, 1 when it has too few tasks yet. Pass or fail only. Run from the repo root."""

from __future__ import annotations

import json
import pathlib
import sys

sys.path.insert(0, "tools")
import aether_distill as distill  # noqa: E402

HERE = pathlib.Path(__file__).resolve().parent
TASKS_NEEDED = 246


def failed_ids(folder: pathlib.Path) -> set:
    return {t["id"] for t in json.loads((folder / "tasks_failed.json").read_text(encoding="utf-8"))["tasks"]}


def main() -> int:
    try:
        report = json.loads((HERE / "filter.json").read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return 1
    results = report["destinations"][0]["variants"][0]["results"]
    done = [r for r in results if r.get("attempts") and not r.get("infra_failed")]
    if len(done) < TASKS_NEEDED:
        return 1
    ids = {r["task_id"] for r in done}
    published = set().union(*(failed_ids(HERE.parent / s) for s in ("f1-20261103", "f2-20261103", "f3-20261103"))) & ids
    second = failed_ids(HERE.parent / "fs42-20261105") & ids
    average = {r["task_id"] for r in done if distill.filter_failure_class(r["attempts"][0].get("run") or {})}
    worse = max(len(published), len(second))
    print(f"early check at {len(done)} tasks: average failed {len(average)}, published seed {len(published)}, seed 42 {len(second)}")
    return 2 if len(average) >= 2 * worse else 0


if __name__ == "__main__":
    sys.exit(main())
