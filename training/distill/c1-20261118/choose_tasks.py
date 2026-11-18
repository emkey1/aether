#!/usr/bin/env python3
"""Choose the control arm's tasks: a seeded sample of the tasks 8-bit gemma4-12b-v2 passed in f1, f2 and f3-20261103.

    python3 training/distill/c1-20261118/choose_tasks.py     # from the repo root; writes tasks_passed.json, provenance.json

The sample is larger than the 298 pairs the control needs, since some solves fail; run.sh keeps exactly 298.
"""
from __future__ import annotations

import json
import pathlib
import random

HERE = pathlib.Path(__file__).resolve().parent
SOURCES = ("f1-20261103", "f2-20261103", "f3-20261103")
SAMPLE = 340
SEED = 20261118


def main() -> None:
    passed, provenance = [], []
    for batch in SOURCES:
        d = HERE.parent / batch
        passed += json.loads((d / "tasks_passed.json").read_text(encoding="utf-8"))["tasks"]
        provenance.append(json.loads((d / "provenance.json").read_text(encoding="utf-8")))
    ids = [t["id"] for t in passed]
    if len(ids) != len(set(ids)):
        raise SystemExit("STOP: a task id repeats across the source batches")
    if not all(p.get("ok") for p in provenance):
        raise SystemExit("STOP: a source batch's provenance does not pass")
    chosen = sorted(random.Random(SEED).sample(passed, SAMPLE), key=lambda t: t["id"])
    (HERE / "tasks_passed.json").write_text(json.dumps({
        "version": "c1-20261118",
        "description": f"{SAMPLE} of the {len(passed)} tasks 8-bit gemma4-12b-v2 passed in {', '.join(SOURCES)} "
                       f"(random.Random({SEED}).sample)",
        "tasks": chosen}, indent=1, ensure_ascii=False) + "\n", encoding="utf-8")
    requests: dict[str, int] = {}
    for p in provenance:
        for model, count in (p.get("requests_by_model") or {}).items():
            requests[model] = requests.get(model, 0) + count
    (HERE / "provenance.json").write_text(json.dumps({
        "ok": True, "pool": provenance[0].get("pool"),
        "allowed_models": sorted({m for p in provenance for m in p.get("allowed_models") or []}),
        "requests_by_model": requests,
        "sources": {b: "provenance.json passed" for b in SOURCES},
        "note": "the tasks come from the source batches; their generation provenance is theirs"}, indent=1) + "\n",
        encoding="utf-8")
    print(f"chose {SAMPLE} of {len(passed)} passed tasks")


if __name__ == "__main__":
    main()
