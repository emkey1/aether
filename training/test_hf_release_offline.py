#!/usr/bin/env python3
"""Offline tests for tools/hf_release.py: the comparison and the refusals (no network, no token)."""

from __future__ import annotations

import json
import pathlib
import subprocess
import sys
import tempfile
import unittest

REPO_ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT / "tools"))
import hf_release as hr  # noqa: E402


def make_folder(tmp: str) -> pathlib.Path:
    folder = pathlib.Path(tmp) / "release"
    (folder / ".cache" / "huggingface").mkdir(parents=True)
    (folder / ".cache" / "huggingface" / "x.lock").write_bytes(b"")
    (folder / "README.md").write_bytes(b"# card\n")
    (folder / "model-00001-of-00001.safetensors").write_bytes(b"\x01" * 4096)
    return folder


def hub_view(folder: pathlib.Path) -> dict:
    weights = folder / "model-00001-of-00001.safetensors"
    return {"README.md": {"size": 7, "lfs_sha256": None, "blob_id": hr.blob_id((folder / "README.md").read_bytes())},
            weights.name: {"size": 4096, "lfs_sha256": hr.sha256_file(weights), "blob_id": "pointer"},
            ".gitattributes": {"size": 1, "lfs_sha256": None, "blob_id": "x"}}


class CompareTests(unittest.TestCase):
    def test_blob_id_matches_git(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = pathlib.Path(tmp) / "f"
            path.write_bytes(b"hello\n")
            git = subprocess.run(["git", "hash-object", str(path)], capture_output=True, text=True).stdout.strip()
            self.assertEqual(hr.blob_id(b"hello\n"), git)

    def test_a_matching_repo_passes_and_the_cache_is_ignored(self):
        with tempfile.TemporaryDirectory() as tmp:
            folder = make_folder(tmp)
            problems, digests = hr.compare(folder, hub_view(folder))
            self.assertEqual(problems, [])
            self.assertEqual(sorted(digests), ["README.md", "model-00001-of-00001.safetensors"])

    def test_missing_changed_and_extra_files_are_each_a_problem(self):
        with tempfile.TemporaryDirectory() as tmp:
            folder = make_folder(tmp)
            view = hub_view(folder)
            del view["README.md"]
            view["model-00001-of-00001.safetensors"]["lfs_sha256"] = "0" * 64
            view["old.bin"] = {"size": 1, "lfs_sha256": None, "blob_id": "y"}
            problems, _ = hr.compare(folder, view)
            self.assertEqual(sorted(p.split(":")[0] for p in problems),
                             ["content differs", "missing on the Hub", "on the Hub but not local"])

    def test_a_truncated_lfs_file_with_the_right_hash_field_still_needs_the_size(self):
        with tempfile.TemporaryDirectory() as tmp:
            folder = make_folder(tmp)
            view = hub_view(folder)
            view["model-00001-of-00001.safetensors"]["size"] = 1
            self.assertEqual(len(hr.compare(folder, view)[0]), 1)

    def test_an_empty_folder_does_not_pass(self):
        with tempfile.TemporaryDirectory() as tmp:
            (pathlib.Path(tmp) / "empty").mkdir()
            self.assertTrue(hr.compare(pathlib.Path(tmp) / "empty", {})[0])


class RecordTests(unittest.TestCase):
    def record(self, folder, **overrides):
        _, digests = hr.compare(folder, hub_view(folder))
        rec = {"repo": "o/r", "revision": "v1", "commit": "c" * 40, "files": digests, "result": "pass"} | overrides
        hr.record_path(folder).write_text(json.dumps(rec), encoding="utf-8")

    def test_a_passing_record_for_the_same_contents_is_accepted(self):
        with tempfile.TemporaryDirectory() as tmp:
            folder = make_folder(tmp)
            self.record(folder)
            self.assertEqual(hr.check_record(folder, "o/r", "v1")["commit"], "c" * 40)

    def test_refusals(self):
        with tempfile.TemporaryDirectory() as tmp:
            folder = make_folder(tmp)
            with self.assertRaises(SystemExit):
                hr.check_record(folder, "o/r", "v1")  # never verified
            for overrides in ({"result": "fail"}, {"repo": "o/other"}, {"revision": "main"}):
                self.record(folder, **overrides)
                with self.subTest(**overrides), self.assertRaises(SystemExit):
                    hr.check_record(folder, "o/r", "v1")
            self.record(folder)
            (folder / "README.md").write_bytes(b"# edited after the check\n")
            with self.assertRaises(SystemExit):
                hr.check_record(folder, "o/r", "v1")
            self.record(folder)
            (folder / "extra.json").write_bytes(b"{}")
            with self.assertRaises(SystemExit):
                hr.check_record(folder, "o/r", "v1")

    def test_no_token_is_an_error_before_any_network_call(self):
        out = subprocess.run([sys.executable, str(REPO_ROOT / "tools" / "hf_release.py"), "status", "o/r"],
                             capture_output=True, text=True, env={"PATH": "/usr/bin:/bin"})
        self.assertNotEqual(out.returncode, 0)
        self.assertIn("HF_TOKEN is not set", out.stderr)


CARDS = REPO_ROOT / "training" / "config" / "hf_release"
MOE_FRONT = "---\nlicense: apache-2.0\nbase_model: Qwen/Qwen3.6-35B-A3B\n---\n\n# card\n"


class CardTests(unittest.TestCase):
    """Roadmap T3: no MoE card states the third-epoch gain without the fresh-task reading."""

    def test_every_committed_card_passes(self):
        cards = sorted(CARDS.glob("README*.md"))
        self.assertGreaterEqual(len(cards), 7)
        for card in cards:
            self.assertEqual(hr.card_problems(card.read_text(encoding="utf-8")), [], card.name)

    def test_both_moe_cards_are_held_to_the_rule(self):
        moe = [c for c in sorted(CARDS.glob("README*.md"))
               if "base_model: Qwen/Qwen3.6-35B-A3B" in c.read_text(encoding="utf-8")]
        self.assertEqual([c.name for c in moe], ["README.moe.lora.md", "README.moe.merged.md"])

    def test_the_old_wording_and_a_missing_block_are_refused(self):
        old = MOE_FRONT + "Training on for three full epochs raised eval loss, yet it lifted the\n  held-out score to 39.\n"
        problems = hr.card_problems(old)
        self.assertEqual(len(problems), 2, problems)
        self.assertEqual(len(hr.card_problems(old + hr.MOE_FRESH_TASK_DISCLOSURE)), 1)
        rewrapped = " ".join(hr.MOE_FRESH_TASK_DISCLOSURE.split()).replace(". ", ".\n")
        self.assertEqual(hr.card_problems(MOE_FRONT + rewrapped), [])

    def test_other_base_models_are_not_held_to_it(self):
        other = "---\nbase_model: google/gemma-4-12b\n---\n\nIt lifted the held-out score.\n"
        self.assertEqual(hr.card_problems(other), [])

    def test_readme_refuses_a_bad_card_before_the_token_is_read(self):
        with tempfile.TemporaryDirectory() as tmp:
            card = pathlib.Path(tmp) / "README.md"
            card.write_text(MOE_FRONT + "It lifted the held-out score to 39.\n", encoding="utf-8")
            out = subprocess.run([sys.executable, str(REPO_ROOT / "tools" / "hf_release.py"), "readme", "o/r",
                                  str(card), "-m", "m"], capture_output=True, text=True, env={"PATH": "/usr/bin:/bin"})
        self.assertNotEqual(out.returncode, 0)
        self.assertIn("refusing: a Qwen/Qwen3.6-35B-A3B card must carry the disclosure", out.stderr)
        self.assertNotIn("HF_TOKEN", out.stderr)


if __name__ == "__main__":
    unittest.main()
