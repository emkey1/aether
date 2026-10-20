#!/usr/bin/env python3
"""Offline tests for tools/aether_distill.py helpers (no network, no models)."""

from __future__ import annotations

import pathlib
import sys
import unittest

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

if __name__ == "__main__":
    unittest.main()
