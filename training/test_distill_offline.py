#!/usr/bin/env python3
"""Offline tests for tools/aether_distill.py helpers (no network, no models)."""

from __future__ import annotations

import argparse
import pathlib
import sys
import tempfile
import unittest
from unittest import mock

REPO_ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT / "tools"))
import aether_distill as distill  # noqa: E402


class PromptContradictionTests(unittest.TestCase):
    def test_wrong_literal_line_is_reported(self):
        prompt = "Line 1: `Car 1 -> spot 1`\nLine 2: `Car 4 -> turned away`"
        self.assertEqual(distill.prompt_contradictions(prompt, "Car 1 -> spot 1\nCar 4 -> spot 5\n"),
                         ["Car 4 -> turned away"])

    def test_placeholders_are_wildcards(self):
        prompt = "Line 1: `TANK REPORT`\nLine 2: `Filled: N`\nLine 3: `Lowest: <name>`"
        self.assertEqual(distill.prompt_contradictions(prompt, "TANK REPORT\nFilled: 3\nLowest: Kelp\n"), [])

    def test_quoted_prefix_matches_a_longer_line(self):
        prompt = "Line 2: `Filled: ` followed by the count\nLine 3: `Missing species:` then names"
        self.assertEqual(distill.prompt_contradictions(prompt, "TANK\nFilled: 3\nMissing species: Kelp\n"), [])

    def test_prompt_without_line_specs_passes(self):
        self.assertEqual(distill.prompt_contradictions("Print the total.", "42\n"), [])



class CategoryTests(unittest.TestCase):
    def test_map_categories_state_an_output_order(self):
        self.assertEqual(len(distill.MAP_CATEGORIES), 6)
        for c in distill.MAP_CATEGORIES:
            self.assertIn("order", c)
        self.assertFalse(set(distill.MAP_CATEGORIES) & set(distill.CATEGORIES))


MODEL_PROGRAM = "fn main() { print(\"written by the filter model\"); }"
SOLVER_PROGRAM = "fn main() { print(\"written by the guided solver\"); }"


def report(model, results, doc_name="none"):
    return {"aether_version": "test", "destinations": [{"model": model, "variants": [{"doc_name": doc_name,
                                                                                      "results": results}]}]}


def result(task_id, source, stdout_ok, *, returncode=0, stderr="", **extra):
    return {"task_id": task_id, **extra, "attempts": [{"source_code": source, "run": {
        "exact_stdout_match": stdout_ok, "returncode": returncode, "expected_returncode": 0, "stderr": stderr}}]}


class FilterTests(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        patcher = mock.patch.object(distill, "DISTILL_DIR", pathlib.Path(tmp.name))
        patcher.start()
        self.addCleanup(patcher.stop)
        self.batch = distill.batch_dir("b")
        ids = ["pass", "coded", "rc", "wrong", "infra", "missing"]
        distill.save_json(self.batch / "tasks_frozen.json", {"version": "b", "tasks": [
            {"id": i, "title": i, "prompt": f"task {i}", "expected_stdout": "ok\n"} for i in ids]})

    def run_filter(self):
        distill.save_json(self.batch / "filter.json", report("tuned", [
            result("pass", MODEL_PROGRAM, True),
            result("coded", MODEL_PROGRAM, False, returncode=1, stderr="error [SYN-001]: expected ;"),
            result("rc", MODEL_PROGRAM, False, returncode=1),
            result("wrong", MODEL_PROGRAM, False),
            result("infra", MODEL_PROGRAM, False, infra_failed=True)]))
        return distill.stage_filter(argparse.Namespace(batch="b"))

    def tasks(self, split):
        return {t["id"]: t for t in distill.load_json(self.batch / f"tasks_{split}.json", None)["tasks"]}

    def test_splits_by_first_attempt_and_class(self):
        self.assertEqual(self.run_filter(), 1)  # two tasks were not measured
        self.assertEqual({i: t["filter"]["first_attempt"] for i, t in self.tasks("failed").items()},
                         {"coded": "coded", "rc": "rc", "wrong": "wrong"})
        self.assertEqual(set(self.tasks("passed")), {"pass"})

    def test_a_task_not_measured_joins_neither_split(self):
        self.run_filter()
        for name in ("infra", "missing"):
            self.assertNotIn(name, self.tasks("failed"))
            self.assertNotIn(name, self.tasks("passed"))

    def test_a_guided_report_is_refused(self):
        distill.save_json(self.batch / "filter.json", report("tuned", [], doc_name="medium"))
        with self.assertRaises(SystemExit):
            distill.stage_filter(argparse.Namespace(batch="b"))

    def test_the_filter_models_programs_never_reach_a_pair(self):
        self.run_filter()
        distill.save_json(self.batch / "aether.json", report("solver", [
            result("coded", SOLVER_PROGRAM, True), result("rc", SOLVER_PROGRAM, False)], doc_name="medium"))
        distill.stage_harvest(argparse.Namespace(batch="b", split="failed", skip_provenance=True))
        for name in ("tasks_failed.json", "tasks_passed.json", "pairs.json"):
            self.assertNotIn("written by the filter model", (self.batch / name).read_text(encoding="utf-8"))
        pairs = distill.load_json(self.batch / "pairs.json", None)["pairs"]
        self.assertEqual([p["id"] for p in pairs], ["coded"])
        self.assertEqual(pairs[0]["solution"], SOLVER_PROGRAM)
        self.assertEqual(pairs[0]["_provenance"]["filter"], {"model": "tuned", "first_attempt": "coded"})

    def test_harvest_keeps_only_the_chosen_split(self):
        self.run_filter()
        distill.save_json(self.batch / "aether.json", report("solver", [
            result("pass", SOLVER_PROGRAM, True), result("coded", SOLVER_PROGRAM, True)], doc_name="medium"))
        distill.stage_harvest(argparse.Namespace(batch="b", split="passed", skip_provenance=True))
        self.assertEqual([p["id"] for p in distill.load_json(self.batch / "pairs.json", None)["pairs"]], ["pass"])


if __name__ == "__main__":
    unittest.main()
