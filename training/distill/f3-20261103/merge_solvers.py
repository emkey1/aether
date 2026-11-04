#!/usr/bin/env python3
"""Fold a slice's two solves into aether.json: the guided 27B's report, with MiMo's agreeing program on each task the 27B missed.

    python3 training/distill/f3-20261103/merge_solvers.py DIR

DIR holds aether_base.json, aether_mimo.json and tasks_left.json. Each result gains `_solver`, which `harvest` copies to its pair.
"""

from __future__ import annotations

import json
import pathlib
import sys

BASE = "qwen3.8-27b-base+medium-guide"
MIMO = "mimo-v2.6-flash+medium-guide"


def results(report: dict) -> list[dict]:
    return report["destinations"][0]["variants"][0]["results"]


def exact(result: dict) -> bool:
    return any((a.get("run") or {}).get("exact_stdout_match") for a in result.get("attempts") or [])


def main() -> None:
    batch = pathlib.Path(sys.argv[1])
    base = json.loads((batch / "aether_base.json").read_text(encoding="utf-8"))
    mimo = {r["task_id"]: r for r in results(json.loads((batch / "aether_mimo.json").read_text(encoding="utf-8")))}
    left = {t["id"] for t in json.loads((batch / "tasks_left.json").read_text(encoding="utf-8"))["tasks"]}
    missed = {r["task_id"] for r in results(base) if not exact(r)}
    if missed != left or set(mimo) - left:
        sys.exit(f"STOP: the 27B missed {len(missed)}, tasks_left has {len(left)}, MiMo answered {len(mimo)}")
    merged, replaced = [], 0
    for result in results(base):
        answer = mimo.get(result["task_id"])
        if answer is not None and exact(answer):
            merged.append({**answer, "_solver": MIMO})
            replaced += 1
        else:
            merged.append({**result, "_solver": BASE})
    base["_merged_from"] = [{"report": "aether_base.json", "results": len(merged), "solver": BASE},
                            {"report": "aether_mimo.json", "results": len(mimo), "solver": MIMO, "replaced_unsolved": replaced}]
    base["destinations"][0]["variants"][0]["results"] = merged
    (batch / "aether.json").write_text(json.dumps(base, indent=1), encoding="utf-8")
    print(f"{batch.name}: {sum(map(exact, merged))} of {len(merged)} agree, {replaced} of them MiMo's")


if __name__ == "__main__":
    main()
