#!/usr/bin/env python3
"""Offline tests for tools/aether_train_run.py and the training registry (no ssh, no GPU)."""

from __future__ import annotations

import json
import pathlib
import subprocess
import sys
import tempfile
import unittest

REPO_ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT / "tools"))
import aether_train_run as tr  # noqa: E402

RUNS_DIR = REPO_ROOT / "training" / "config" / "runs"
REQUIRED_FAMILY_KEYS = {"description", "image", "instruction_part", "response_part", "target_modules",
                        "chat_template_kwargs", "tokenizer_from_base", "serve", "leak_prefixes"}


def write_run(tmp: pathlib.Path, **overrides) -> pathlib.Path:
    run = {"tag": "t-run", "family": "qwen3_5", "base_model": "/storage/models/Base", "host": "host-x",
           "dataset": {"id": "ds-1", "aether_version": "2026-10-15-1"}} | overrides
    path = tmp / "run.json"
    path.write_text(json.dumps(run), encoding="utf-8")
    return path


class RegistryTests(unittest.TestCase):
    def test_every_family_is_complete(self):
        registry = json.loads(tr.FAMILIES_PATH.read_text(encoding="utf-8"))
        for name, family in registry["families"].items():
            with self.subTest(family=name):
                self.assertFalse(REQUIRED_FAMILY_KEYS - set(family), f"missing {REQUIRED_FAMILY_KEYS - set(family)}")
                for key in ("image", "dtype", "max_model_len", "gpu_memory_utilization", "extra_args"):
                    self.assertIn(key, family["serve"])

    def test_tracked_runs_load_and_name_known_families(self):
        runs = sorted(RUNS_DIR.glob("*.json"))
        self.assertTrue(runs)
        for path in runs:
            with self.subTest(run=path.name):
                run = tr.load_run(path)
                self.assertEqual(run["tag"], path.stem)
                self.assertIsNone(tr.PRIVATE.search(path.read_text(encoding="utf-8")))


class ConfigTests(unittest.TestCase):
    def test_defaults_fill_in(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = tr.load_run(write_run(pathlib.Path(tmp)))
        self.assertEqual(run["precision"], "4bit")
        self.assertEqual(run["lora"], {"r": 32, "alpha": 64, "dropout": 0.0})
        self.assertEqual(run["image"], "aether-unsloth-qwen3-coder-30b:base26-fla")
        self.assertEqual(run["paths"]["merged"], "/storage/aether-train/t-run/merged_16bit")
        self.assertEqual(run["score"]["variants"], ["base", "tuned"])

    def test_bad_values_are_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = pathlib.Path(tmp)
            for overrides, message in [({"family": "nope"}, "unknown family"),
                                       ({"precision": "3bit"}, "precision"),
                                       ({"tag": "Bad Tag"}, "tag"),
                                       ({"score": {"variants": ["guided"]}}, "variant")]:
                with self.subTest(overrides=overrides), self.assertRaises(SystemExit) as ctx:
                    tr.load_run(write_run(tmp, **overrides))
                self.assertIn(message, str(ctx.exception))


    def test_a_dataset_may_name_its_instruction_files(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = pathlib.Path(tmp)
            run = tr.load_run(write_run(tmp, dataset={"id": "ds-c", "instruction_manifests": [
                "training/seed_instruction_pairs.json", "training/distilled/d1-20261016.json"]}))
            self.assertEqual(len(run["dataset"]["instruction_manifests"]), 2)
            self.assertTrue(tr._instruction_manifest("training/distilled/d1-20261016.json").is_file())
            with self.assertRaises(SystemExit):
                tr._instruction_manifest("training/distilled/no-such-batch.json")
            for bad in ([], "training/x.json", ["a.json", "a.json"], [""]):
                with self.subTest(bad=bad), self.assertRaises(SystemExit) as ctx:
                    tr.load_run(write_run(tmp, dataset={"id": "ds-c", "instruction_manifests": bad}))
                self.assertIn("instruction_manifests", str(ctx.exception))

    def test_dataset_stage_passes_the_named_files_to_the_builder(self):
        from unittest import mock
        with tempfile.TemporaryDirectory() as tmp:
            tmp = pathlib.Path(tmp)
            run = tr.load_run(write_run(tmp, dataset={"id": "ds-c", "aether_version": "2026-10-15-1",
                                                      "instruction_manifests": ["training/seed_instruction_pairs.json"]}))
            run["paths"]["dataset_local"] = str(tmp / "out")
            seen = {}

            class Done(Exception):
                pass

            def fake_sh(argv, *a, **k):
                seen["argv"] = argv
                raise Done()
            fake_version = mock.Mock(stdout="Aether Compiler Version: 2026-10-15-1\n")
            with mock.patch.object(tr, "sh", fake_sh), mock.patch.object(tr.subprocess, "run", return_value=fake_version):
                with self.assertRaises(Done):
                    tr.stage_dataset(run, mock.Mock(aether_bin="aether", force=False))
            argv = seen["argv"]
            i = argv.index("--instruction-manifest")
            self.assertTrue(argv[i + 1].endswith("training/seed_instruction_pairs.json"))
            self.assertEqual(argv.count("--instruction-manifest"), 1)


class ScriptTests(unittest.TestCase):
    def load(self, **overrides):
        with tempfile.TemporaryDirectory() as tmp:
            return tr.load_run(write_run(pathlib.Path(tmp), **overrides))

    def test_trainer_argv_carries_family_markers_and_recipe(self):
        argv = tr.trainer_argv(self.load())
        self.assertEqual(argv[argv.index("--instruction-part") + 1], "<|im_start|>user\\n")
        self.assertEqual(argv[argv.index("--response-part") + 1], "<|im_start|>assistant\\n")
        self.assertEqual(argv[argv.index("--lora-r") + 1], "32")
        self.assertEqual(argv[argv.index("--seed") + 1], argv[argv.index("--random-state") + 1])
        # Rendered like the served prompt (Qwen3.8 adds a reasoning system turn otherwise).
        self.assertEqual(json.loads(argv[argv.index("--chat-template-kwargs") + 1]), {"enable_thinking": False})
        self.assertNotIn("--no-load-in-4bit", argv)
        self.assertIn("--no-load-in-4bit", tr.trainer_argv(self.load(precision="16bit")))

    def test_gemma_loads_through_fastmodel_and_renders_as_served(self):
        argv = tr.trainer_argv(self.load(family="gemma4_unified"))
        self.assertEqual(argv[argv.index("--loader") + 1], "fast_model")
        self.assertIn("--render-as-served", argv)
        self.assertEqual(argv[argv.index("--instruction-part") + 1], "<|turn>user\\n")
        self.assertEqual(argv[argv.index("--response-part") + 1], "<|turn>model\\n<|channel>thought\\n<channel|>")
        qwen = tr.trainer_argv(self.load())
        self.assertNotIn("--loader", qwen)
        self.assertNotIn("--render-as-served", qwen)

    def test_lora_targets_default_to_the_family_and_a_run_may_override(self):
        argv = tr.trainer_argv(self.load())
        family = argv[argv.index("--target-modules") + 1].split(",")
        self.assertIn("q_proj", family)
        moe = family + ["gate_up_proj"]
        argv = tr.trainer_argv(self.load(lora={"target_modules": moe}))
        self.assertEqual(argv[argv.index("--target-modules") + 1].split(","), moe)
        for bad in ([], "q_proj", [""]):
            with self.assertRaises(SystemExit):
                self.load(lora={"target_modules": bad})

    def test_python_check_is_scored_on_the_host_only_when_asked(self):
        import subprocess
        plain = tr.score_script(self.load())
        self.assertNotIn("--python-baseline", plain)
        with_py = tr.score_script(self.load(score={"python_check": True}))
        self.assertIn("--python-baseline --skip-aether", with_py)
        self.assertIn('python__$SUITE.done', with_py)
        for script in (plain, with_py):
            subprocess.run(["bash", "-n"], input=script, text=True, check=True)

    def test_an_over_training_run_exports_its_last_step_and_keeps_epochs(self):
        argv = tr.trainer_argv(self.load())
        self.assertNotIn("--no-load-best-model-at-end", argv)
        self.assertNotIn("--keep-checkpoint-steps", argv)
        argv = tr.trainer_argv(self.load(train={"early_stopping_patience": 0, "load_best_model_at_end": False,
                                                "keep_checkpoint_steps": [250, 500]}))
        self.assertEqual(argv[argv.index("--early-stopping-patience") + 1], "0")
        self.assertIn("--no-load-best-model-at-end", argv)
        self.assertEqual(argv[argv.index("--keep-checkpoint-steps") + 1], "250,500")

    def test_eval_split_size_and_cadence_are_optional(self):
        argv = tr.trainer_argv(self.load())
        self.assertNotIn("--eval-cases", argv)
        argv = tr.trainer_argv(self.load(train={"eval_cases": 60, "eval_steps": 10}))
        self.assertEqual(argv[argv.index("--eval-cases") + 1], "60")
        self.assertEqual(argv[argv.index("--eval-steps") + 1], "10")
        self.assertEqual(argv[argv.index("--save-steps") + 1], "10")

    def test_mistral_tuned_is_served_with_the_base_tokenizer(self):
        run = self.load(family="mistral")
        tuned, base = tr.serve_command(run, "tuned"), tr.serve_command(run, "base")
        self.assertEqual(tuned[tuned.index("--tokenizer") + 1], "/storage/models/Base")
        self.assertNotIn("--tokenizer", base)
        self.assertNotIn("--tokenizer", tr.serve_command(self.load(), "tuned"))

    def test_thinking_switch_reaches_the_destination(self):
        dest = tr.destinations(self.load(), "base")["destinations"][0]
        self.assertEqual(dest["extra_body"], {"chat_template_kwargs": {"enable_thinking": False}})
        self.assertNotIn("extra_body", tr.destinations(self.load(family="chatml"), "base")["destinations"][0])

    def test_generated_scripts_parse_and_keep_home_expandable(self):
        run = self.load()
        for name, text in (("train.sh", tr.train_script(run)), ("score.sh", tr.score_script(run)),
                           ("score_when_ready.sh", tr.score_when_ready_script(run))):
            with self.subTest(script=name):
                self.assertEqual(subprocess.run(["bash", "-n"], input=text, text=True).returncode, 0)
                self.assertNotIn("'$HOME", text)
                self.assertIsNone(tr.PRIVATE.search(text))
        score = tr.score_script(run)
        for suite in run["score"]["suites"]:
            self.assertIn(f"{suite}=", score)
        self.assertIn("--aether-bin-sha256", score)
        self.assertNotIn('"$OUT/$TAGV.done" ] &&', score)  # a variant is skipped per suite, not by one marker
        self.assertIn("*2026-10-15-1*", score)
        newer = tr.score_script(self.load(score={"grader_version": "2026-10-15-2"}))
        self.assertIn("*2026-10-15-2*", newer)
        self.assertNotIn("*2026-10-15-1*", newer)

    def test_memory_wait_hours_must_be_positive(self):
        self.assertEqual(self.load()["score"]["memory_wait_hours"], 24)
        for bad in (0, -1, "24", True):
            with self.subTest(bad=bad), self.assertRaises(SystemExit) as ctx:
                self.load(score={"memory_wait_hours": bad})
            self.assertIn("memory_wait_hours", str(ctx.exception))


# Stand-ins for the host's commands, so the generated scripts run here: docker, curl, sleep, the harness
# (python3), the grader, sha256sum, free and nvidia-smi. Each reads its behaviour from FAKE_* variables;
# count NAME prints how many times NAME has been called, this call included.
COUNT = 'count() { local n; n=$(cat "$FAKE_STATE/$1" 2>/dev/null || echo 0); n=$((n + 1)); echo $n > "$FAKE_STATE/$1"; echo $n; }\n'
FAKES = {
    # The training container runs for the first FAKE_RUNNING_CALLS inspections; another run's container is up
    # for the first FAKE_PS_CALLS listings.
    "docker": COUNT + """case "$1" in
  run) exit "${FAKE_DOCKER_RUN_RC:-0}" ;;
  inspect) [ "$(count inspect)" -le "${FAKE_RUNNING_CALLS:-0}" ] && echo true || echo false ;;
  ps) [ "$(count ps)" -le "${FAKE_PS_CALLS:-0}" ] && echo aether-serve-other ;;
esac
exit 0""",
    "curl": """[ "${FAKE_READY:-1}" = 1 ] && echo "{\\"data\\": [{\\"id\\": \\"$FAKE_NAME\\"}]}"
exit 0""",
    "sleep": "exit 0",
    "sha256sum": 'echo "0000  $1"',
    # check_generations.py --complete fails while FAKE_CHECK_RC says so; the harness itself always exits 0.
    "python3": """case "$*" in *check_generations.py*--complete*) exit "${FAKE_CHECK_RC:-0}" ;; esac
exit 0""",
    "nvidia-smi": 'echo "${FAKE_NVIDIA:-[N/A], [N/A]}"',
    # Available memory is FAKE_LOW_MIB for the first FAKE_LOW_CALLS calls, then FAKE_HIGH_MIB.
    "free": COUNT + """avail=110000; [ "$(count free)" -le "${FAKE_LOW_CALLS:-0}" ] && avail=56000
echo "               total        used        free      shared  buff/cache   available"
echo "Mem:          124000       60000       10000           0       50000       $avail"
exit 0""",
}


class FakeHost:
    """A temporary $HOME with the run's remote directory, a grader and FAKES first on PATH."""

    def __init__(self, test: unittest.TestCase, **overrides):
        self._tmp = tempfile.TemporaryDirectory()
        test.addCleanup(self._tmp.cleanup)
        self.root = pathlib.Path(self._tmp.name)
        self.home, self.bin, self.state = self.root / "home", self.root / "bin", self.root / "state"
        for d in (self.home, self.bin, self.state):
            d.mkdir()
        for name, body in FAKES.items():
            (self.bin / name).write_text("#!/bin/bash\n" + body + "\n", encoding="utf-8")
            (self.bin / name).chmod(0o755)
        overrides.setdefault("weights_root", str(self.root / "weights"))
        score = {"grader": "$HOME/grader/aether", "variants": ["base"], "suites": {"a": "bench/a.json"}}
        overrides["score"] = score | overrides.get("score", {})
        self.run = tr.load_run(write_run(self.root, **overrides))
        self.remote = self.home / "aether-train" / self.run["tag"]
        (self.remote / "logs").mkdir(parents=True)
        self.grader = self.home / "grader" / "aether"
        self.set_grader("Aether Compiler Version: 2026-10-15-1 (1.0)")

    def set_grader(self, version: str | None, executable: bool = True) -> None:
        if version is None:
            self.grader.unlink(missing_ok=True)
            return
        self.grader.parent.mkdir(parents=True, exist_ok=True)
        self.grader.write_text(f"#!/bin/bash\necho '{version}'\n", encoding="utf-8")
        self.grader.chmod(0o755 if executable else 0o644)

    def bash(self, script: str, **env) -> subprocess.CompletedProcess:
        path = self.root / "script.sh"
        path.write_text(script, encoding="utf-8")
        full = {"HOME": str(self.home), "PATH": f"{self.bin}:/usr/bin:/bin", "FAKE_STATE": str(self.state),
                "FAKE_NAME": tr.served_name(self.run, "base")} | {k: str(v) for k, v in env.items()}
        return subprocess.run(["/bin/bash", str(path)], env=full, capture_output=True, text=True, timeout=60)

    def log(self, name: str) -> str:
        path = self.remote / name
        return path.read_text(encoding="utf-8") if path.exists() else ""


class GraderProbeTests(unittest.TestCase):
    """push and check probe the grader score.sh uses (missing on a new training host, 2026-11-25)."""

    def facts(self, host: FakeHost) -> dict:
        out = host.bash(tr.grader_probe_cmd(host.run)).stdout
        return dict(line.split("=", 1) for line in out.split() if "=" in line)

    def test_probe_tells_missing_not_executable_and_version(self):
        host = FakeHost(self)
        self.assertIn("$HOME/grader/aether", tr.grader_probe_cmd(host.run))  # expanded on the host
        facts = self.facts(host)
        self.assertEqual(facts["grader"], "ok")
        self.assertIn("2026-10-15-1", facts["grader_version"])
        host.set_grader("Aether Compiler Version: 2026-10-15-1", executable=False)
        self.assertEqual(self.facts(host), {"grader": "not-executable"})
        host.set_grader(None)
        self.assertEqual(self.facts(host), {"grader": "missing"})

    def check(self, run: dict, probe_out: str) -> tuple[int, str, str]:
        import contextlib
        import io
        from unittest import mock
        base = "model=ok\nimage=ok\nserve_image=ok\navail_gb=100\ncontainers=\nmerged=no\ndisk_gb=500\n"
        done = subprocess.CompletedProcess([], 0, stdout=base + probe_out, stderr="")
        printed = io.StringIO()
        with mock.patch.object(tr, "remote", return_value=done) as remote, contextlib.redirect_stdout(printed):
            rc = tr.stage_check(run, mock.Mock(when_ready=False))
        return rc, printed.getvalue(), remote.call_args[0][1]

    def test_check_reports_a_missing_or_wrong_grader_as_a_problem(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = tr.load_run(write_run(pathlib.Path(tmp), score={"grader": "$HOME/g/aether",
                                                                  "grader_version": "2026-10-30-1"}))
        rc, out, command = self.check(run, "grader=ok\ngrader_version=Aether_Compiler_Version:_2026-10-30-1_(1.0)\n")
        self.assertEqual(rc, 0, out)
        self.assertIn("$HOME/g/aether --version", command)
        for probe, wanted in (("grader=missing\n", "grader missing"),
                              ("grader=not-executable\n", "grader not-executable"),
                              ("", "grader missing"),
                              ("grader=ok\ngrader_version=Aether_Compiler_Version:_2026-10-15-1\n",
                               "'Aether Compiler Version: 2026-10-15-1', not language 2026-10-30-1")):
            with self.subTest(probe=probe):
                rc, out, _ = self.check(run, probe)
                self.assertEqual(rc, 1)
                problems = [line for line in out.splitlines() if line.startswith("PROBLEM:")]
                self.assertEqual(len(problems), 1, out)
                self.assertIn(wanted, problems[0])
                self.assertIn("$HOME/g/aether", problems[0])


class ScoreScriptExitTests(unittest.TestCase):
    """score.sh exits non-zero when a variant failed or is incomplete, so a caller's log shows it (2026-11-25:
    the served model never became ready and the pass still ended with exit 0)."""

    def score(self, host: FakeHost, **env) -> subprocess.CompletedProcess:
        return host.bash(tr.score_script(host.run), **env)

    def test_a_finished_pass_exits_zero_and_a_rerun_skips_it(self):
        host = FakeHost(self)
        first = self.score(host)
        self.assertEqual(first.returncode, 0, host.log("score/score.log"))
        self.assertTrue((host.remote / "score" / "base__a.done").exists())
        self.assertTrue((host.remote / "score" / "base.done").exists())
        self.assertRegex(host.log("score/score.log"), r"score pass finished\n.*served model stopped\n$")
        again = self.score(host, FAKE_DOCKER_RUN_RC=1)
        self.assertEqual(again.returncode, 0)
        self.assertIn("SKIP base (every suite done)", host.log("score/score.log"))

    def test_failed_variants_exit_one(self):
        for env, why in (({"FAKE_DOCKER_RUN_RC": 1}, "base: docker run failed"),
                         ({"FAKE_READY": 0}, "base: never became ready")):
            with self.subTest(why=why):
                host = FakeHost(self)
                result = self.score(host, **env)
                self.assertEqual(result.returncode, 1)
                self.assertIn(f"score pass finished with failures: {why} (re-run to resume)",
                              host.log("score/score.log"))
                self.assertFalse((host.remote / "score" / "base.done").exists())

    def test_no_export_counts_as_a_failure_beside_a_scored_variant(self):
        host = FakeHost(self, score={"variants": ["base", "tuned"]})
        result = self.score(host)
        self.assertEqual(result.returncode, 1)
        self.assertTrue((host.remote / "score" / "base.done").exists())
        self.assertIn("failures: tuned: no merged export", host.log("score/score.log"))

    def test_an_incomplete_suite_exits_one_and_resumes(self):
        host = FakeHost(self, score={"suites": {"a": "bench/a.json", "b": "bench/b.json"}})
        result = self.score(host, FAKE_CHECK_RC=1)
        self.assertEqual(result.returncode, 1)
        self.assertIn("failures: base: incomplete", host.log("score/score.log"))
        self.assertFalse((host.remote / "score" / "base__a.done").exists())
        self.assertTrue((host.remote / "score" / "base__smoke.ok").exists())
        resumed = self.score(host)
        self.assertEqual(resumed.returncode, 0)
        self.assertTrue((host.remote / "score" / "base__b.done").exists())

    def test_a_wrong_or_missing_grader_aborts_with_two(self):
        host = FakeHost(self)
        host.set_grader("Aether Compiler Version: 2026-10-14-1")
        self.assertEqual(self.score(host).returncode, 2)
        host.set_grader(None)
        self.assertEqual(self.score(host).returncode, 2)
        self.assertIn("ABORT grader missing", host.log("score/score.log"))


class ScoreWaiterMemoryTests(unittest.TestCase):
    """score_when_ready.sh waits for the serve's memory share (vLLM's --gpu-memory-utilization of the device),
    logging the shortfall, gives up after score.memory_wait_hours, and exits with score.sh's status."""

    def waiter(self, host: FakeHost, score_rc: int = 0, **env) -> subprocess.CompletedProcess:
        (host.remote / "score.sh").write_text(f"touch \"$(dirname \"$0\")/scored\"\nexit {score_rc}\n")
        return host.bash(tr.score_when_ready_script(host.run), **env)

    def test_waits_for_memory_then_scores_and_passes_on_the_exit_status(self):
        host = FakeHost(self)
        result = self.waiter(host, score_rc=1, FAKE_LOW_CALLS=2)
        log = host.log("logs/when_ready.log")
        self.assertEqual(result.returncode, 1, log)
        # 0.85 of 124,000 MiB is 105,400 MiB (102 GiB); 56,000 MiB (54 GiB) is short of it.
        self.assertEqual(log.count("waiting for memory: the serve asks for 102 GiB (0.85 of 121 GiB), "
                                   "54 GiB available"), 2, log)
        self.assertIn("memory ok: the serve asks for 102 GiB (0.85 of 121 GiB), 107 GiB available", log)
        self.assertIn("score.sh exit 1; see", log)
        self.assertTrue((host.remote / "scored").exists())

    def test_gives_up_after_the_limit_without_scoring(self):
        host = FakeHost(self, score={"memory_wait_hours": 0.25})
        result = self.waiter(host, FAKE_LOW_CALLS=1000)
        log = host.log("logs/when_ready.log")
        self.assertEqual(result.returncode, 5, log)
        self.assertEqual(log.count("waiting for memory:"), 3)
        self.assertIn("GAVE UP after 15 min short of memory", log)
        self.assertFalse((host.remote / "scored").exists())

    def test_own_jobs_do_not_use_up_the_memory_wait(self):
        host = FakeHost(self, score={"memory_wait_hours": 0.25})
        result = self.waiter(host, FAKE_PS_CALLS=10, FAKE_LOW_CALLS=1000)
        log = host.log("logs/when_ready.log")
        self.assertEqual(result.returncode, 5, log)
        self.assertEqual(log.count("not yet: other GPU jobs running: aether-serve-other"), 10)
        self.assertEqual(log.count("waiting for memory:"), 3)  # the 15 minutes start when the box is ours

    def test_reads_a_discrete_gpu_from_nvidia_smi(self):
        host = FakeHost(self, score={"memory_wait_hours": 0.25})
        short = self.waiter(host, FAKE_NVIDIA="81920, 20000")
        self.assertEqual(short.returncode, 5)
        self.assertIn("the serve asks for 68 GiB (0.85 of 80 GiB), 19 GiB available", host.log("logs/when_ready.log"))
        enough = self.waiter(host, FAKE_NVIDIA="81920, 70000")
        self.assertEqual(enough.returncode, 0)
        self.assertIn("memory ok: the serve asks for 68 GiB (0.85 of 80 GiB), 68 GiB available",
                      host.log("logs/when_ready.log"))
        self.assertFalse((host.state / "free").exists())  # free is the fallback for unified memory only

    def test_says_what_it_waits_for_before_the_memory(self):
        host = FakeHost(self, score={"variants": ["tuned"]})
        merged = pathlib.Path(host.run["paths"]["merged"])
        merged.mkdir(parents=True)
        (merged / "config.json").write_text("{}")
        result = self.waiter(host, FAKE_RUNNING_CALLS=2)
        log = host.log("logs/when_ready.log")
        self.assertEqual(result.returncode, 0, log)
        self.assertNotIn("waiting for the merged export", log)  # already there
        self.assertIn("waiting for aether-train-t-run to exit", log)
        self.assertRegex(log, r"exit\n.*memory ok.*\n.*scoring\n.*score\.sh exit 0\n$")

    def test_a_waiter_for_a_tuned_variant_waits_for_the_export_first(self):
        text = tr.score_when_ready_script(self.fake_run(["tuned"]))
        self.assertLess(text.index("waiting for the merged export"), text.index('to exit"'))
        self.assertNotIn("merged export", tr.score_when_ready_script(self.fake_run(["base"])))

    def fake_run(self, variants):
        with tempfile.TemporaryDirectory() as tmp:
            return tr.load_run(write_run(pathlib.Path(tmp), score={"variants": variants}))


class MergeTwinTests(unittest.TestCase):
    def load(self, **overrides):
        with tempfile.TemporaryDirectory() as tmp:
            return tr.load_run(write_run(pathlib.Path(tmp), **overrides))

    def test_merge_script_uses_this_runs_trainer_and_the_source_adapter(self):
        run = self.load(tag="t-run-ep2", merge_from={"run": "t-run", "step": 500})
        text = tr.merge_script(run)
        self.assertEqual(subprocess.run(["bash", "-n"], input=text, text=True).returncode, 0)
        self.assertIsNone(tr.PRIVATE.search(text))
        self.assertNotIn("'$HOME", text)
        self.assertIn("/workspace/scripts/unsloth_qwen_coder_30b_sft.py", text)
        self.assertIn("--adapter-dir /source/train/keep/checkpoint-500", text)
        self.assertIn("$HOME/aether-train/t-run:/source:ro", text)
        self.assertIn("--merged-output-dir /storage/aether-train/t-run-ep2/merged_16bit", text)
        self.assertIn("--name aether-train-merge-t-run-ep2", text)  # seen by the other stages' GPU gate
        self.assertNotIn("--epochs", text)

    def test_merge_keeps_the_runs_precision(self):
        self.assertIn("--no-load-in-4bit", tr.merge_script(self.load(
            tag="t-run-ep2", precision="16bit", merge_from={"run": "t-run", "step": 5})))

    def test_bad_merge_from_is_refused(self):
        for bad in ({"run": "t-run"}, {"run": "t-run", "step": 0}, {"run": "t-run", "step": "500"},
                    {"run": "t-run", "step": True}, {"run": "../x", "step": 5}, {"run": "t-run-ep2", "step": 5},
                    "t-run"):
            with self.subTest(merge_from=bad), self.assertRaises(SystemExit):
                self.load(tag="t-run-ep2", merge_from=bad)

    def test_a_twin_is_never_trained_and_only_a_twin_merges(self):
        import argparse
        with self.assertRaises(SystemExit) as twin:
            tr.stage_train(self.load(tag="t-run-ep2", merge_from={"run": "t-run", "step": 5}), argparse.Namespace())
        self.assertIn("merge stage", str(twin.exception))
        with self.assertRaises(SystemExit) as plain:
            tr.stage_merge(self.load(), argparse.Namespace())
        self.assertIn("no merge_from", str(plain.exception))


class ProgressTests(unittest.TestCase):
    """twins writes a scoring-only twin per kept checkpoint; progress tables their fetched scores by step."""

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = pathlib.Path(self._tmp.name)
        self.cfg = self.tmp / "runs"
        self.cfg.mkdir()

    def tearDown(self):
        self._tmp.cleanup()

    def write(self, **overrides) -> pathlib.Path:
        path = write_run(self.cfg, **overrides)
        return path.rename(self.cfg / f"{json.loads(path.read_text())['tag']}.json")

    def twins(self, path, force=False):
        import argparse
        return tr.stage_twins(tr.load_run(path), argparse.Namespace(run=path, force=force))

    def test_a_twin_per_kept_step_on_the_development_pool(self):
        path = self.write(train={"keep_checkpoint_steps": [100, 200]},
                          progress={"suites": {"v2_pos": "bench/tasks_v2_pos.json"}},
                          score={"variants": ["base", "tuned"]})
        self.assertEqual(self.twins(path), 0)
        twin = json.loads((self.cfg / "t-run-s200.json").read_text())
        self.assertEqual(twin["merge_from"], {"run": "t-run", "step": 200})
        self.assertEqual(twin["score"]["variants"], ["tuned"])
        self.assertEqual(twin["score"]["suites"], {"v2_pos": "bench/tasks_v2_pos.json"})
        self.assertNotIn("progress", twin)
        self.assertEqual(tr.load_run(self.cfg / "t-run-s100.json")["merge_from"]["step"], 100)

    def test_twins_refuses_held_out_suites_and_runs_without_kept_steps(self):
        with self.assertRaises(SystemExit) as none_kept:
            self.twins(self.write())
        self.assertIn("keep_checkpoint_steps", str(none_kept.exception))
        held = self.write(tag="t-held", train={"keep_checkpoint_steps": [5]},
                          progress={"suites": {"fs": "bench/tasks_frontier_spec.json"}})
        with self.assertRaises(SystemExit) as refused:
            self.twins(held)
        self.assertIn("held-out", str(refused.exception))
        self.assertFalse((self.cfg / "t-held-s5.json").exists())

    def test_twins_keeps_an_existing_config_unless_forced(self):
        path = self.write(train={"keep_checkpoint_steps": [7]})
        self.twins(path)
        twin = self.cfg / "t-run-s7.json"
        twin.write_text(twin.read_text().replace("Scoring-only", "Edited"))
        self.twins(path)
        self.assertIn("Edited", twin.read_text())
        self.twins(path, force=True)
        self.assertNotIn("Edited", twin.read_text())

    def test_progress_reads_twins_by_merge_from_and_splits_held_out(self):
        import gzip
        suites = {"v2_pos": "bench/tasks_v2_pos.json", "fs": "bench/tasks_frontier_spec.json"}
        path = self.write(train={"keep_checkpoint_steps": [10]}, score={"suites": suites})
        (self.cfg / "old-name.json").write_text(json.dumps({"tag": "old-name", "merge_from": {"run": "t-run", "step": 10}}))
        out = self.tmp / "out"

        def report(tag, suite, cases, first, final, packed=False):
            d = out / tag / "score"
            d.mkdir(parents=True, exist_ok=True)
            body = json.dumps({"destinations": [{"variants": [{"summary": {
                "total_cases": cases, "first_attempt_exact": first, "final_exact": final}}]}]})
            if packed:
                with gzip.open(d / f"tuned__{suite}.json.gz", "wt", encoding="utf-8") as fh:
                    fh.write(body)
            else:
                (d / f"tuned__{suite}.json").write_text(body)

        report("old-name", "v2_pos", 35, 30, 33, packed=True)
        report("old-name", "fs", 12, 4, 6)
        report("t-run", "v2_pos", 35, 34, 35)
        (out / "t-run" / "train").mkdir(parents=True)
        (out / "t-run" / "train" / "run_metadata.json").write_text(
            json.dumps({"train_records": 160, "batch_size": 1, "grad_accum": 8}))
        rows = tr.progress_rows(tr.load_run(path), path, out_root=out)
        self.assertEqual([r["label"] for r in rows], ["step 10", "final"])
        self.assertEqual(rows[0]["tag"], "old-name")
        self.assertEqual(rows[0]["epoch"], 0.5)
        self.assertEqual(rows[0]["dev"], (35, 30, 33))
        self.assertEqual(rows[0]["held_out"], (12, 4, 6))
        self.assertTrue(rows[0]["complete"])
        self.assertFalse(rows[1]["complete"])
        self.assertEqual(rows[1]["held_out"], (0, 0, 0))


class ModelProbeTests(unittest.TestCase):
    def probe(self, files, index=None, incomplete=False):
        with tempfile.TemporaryDirectory() as tmp:
            d = pathlib.Path(tmp)
            for name, size in files.items():
                (d / name).write_bytes(b"x" * size)
            if index is not None:
                (d / "model.safetensors.index.json").write_text(json.dumps({"weight_map": index}))
            if incomplete:
                (d / ".cache/huggingface/download").mkdir(parents=True)
                (d / ".cache/huggingface/download/a.incomplete").write_bytes(b"")
            out = subprocess.run([sys.executable, "-c", tr.MODEL_PROBE, str(d)], capture_output=True, text=True)
            return out.stdout.strip()

    def test_states(self):
        index = {"a": "s1.safetensors", "b": "s2.safetensors"}
        self.assertEqual(self.probe({}), "model=missing")
        self.assertEqual(self.probe({"config.json": 2}, index), "model=partial")
        self.assertEqual(self.probe({"config.json": 2, "s1.safetensors": 4, "s2.safetensors": 0}, index), "model=partial")
        self.assertEqual(self.probe({"config.json": 2, "s1.safetensors": 4, "s2.safetensors": 4}, index), "model=ok")
        self.assertEqual(self.probe({"config.json": 2, "s1.safetensors": 4, "s2.safetensors": 4}, index, True),
                         "model=partial")
        self.assertEqual(self.probe({"config.json": 2, "m.safetensors": 4}), "model=ok")

    def test_waiter_script_parses(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = tr.load_run(write_run(pathlib.Path(tmp)))
        text = tr.when_ready_script(run)
        self.assertEqual(subprocess.run(["bash", "-n"], input=text, text=True).returncode, 0)
        score = tr.score_when_ready_script(run)
        self.assertEqual(subprocess.run(["bash", "-n"], input=score, text=True).returncode, 0)
        self.assertIn("aether-train-t-run", score)
        self.assertIn("-ge 60", text)


class CheckGenerationsTests(unittest.TestCase):
    def run_check(self, sources, *extra, infra=0):
        report = {"destinations": [{"variants": [{
            "results": [{"attempts": [{"source_code": s}]} for s in sources],
            "summary": {"total_cases": len(sources), "first_attempt_exact": 0, "final_exact": 0,
                        "infra_failed": infra}}]}]}
        with tempfile.TemporaryDirectory() as tmp:
            script = pathlib.Path(tmp) / "check.py"
            script.write_text(tr.CHECK_GENERATIONS, encoding="utf-8")
            path = pathlib.Path(tmp) / "r.json"
            path.write_text(json.dumps(report), encoding="utf-8")
            return subprocess.run([sys.executable, str(script), str(path), '["thinking", "<think"]', *extra],
                                  capture_output=True, text=True).returncode

    def test_programs_pass(self):
        self.assertEqual(self.run_check(["fn main() -> Void { ret; }", "fn main() -> Void { ret; }"]), 0)

    def test_reasoning_trace_fails(self):
        self.assertEqual(self.run_check(["fn main() -> Void { ret; }", "Thinking Process: ..."]), 1)

    def test_all_empty_fails(self):
        self.assertEqual(self.run_check(["", "  "]), 1)

    def test_infra_failure_fails_only_when_complete_is_asked(self):
        self.assertEqual(self.run_check(["fn main() -> Void { ret; }"], infra=1), 0)
        self.assertEqual(self.run_check(["fn main() -> Void { ret; }"], "--complete", infra=1), 1)



class HostAliasTests(unittest.TestCase):
    """Runs name hosts by alias; the machine and ssh user come only from the private overlay."""

    def test_alias_maps_to_a_variable(self):
        self.assertEqual(tr.host_variable("host-c"), "AETHER_HOST_C")
        self.assertEqual(tr.host_variable("gpu-2"), "AETHER_HOST_GPU_2")

    def test_unset_alias_and_user_name_the_variable_to_set(self):
        import os
        from unittest import mock
        with mock.patch.dict(os.environ, {"PSCAL_FLEET_ENV": "/nonexistent/fleet.env"}, clear=False):
            for key in ("AETHER_HOST_X", "AETHER_TRAIN_SSH_USER"):
                os.environ.pop(key, None)
            tr.fleet_env._cache = None
            with self.assertRaises(SystemExit) as ctx:
                tr.ssh_target({"host": "host-x"})
            self.assertIn("AETHER_TRAIN_SSH_USER", str(ctx.exception))
            os.environ["AETHER_TRAIN_SSH_USER"] = "u"
            with self.assertRaises(SystemExit) as ctx:
                tr.ssh_target({"host": "host-x"})
            self.assertIn("AETHER_HOST_X", str(ctx.exception))
            os.environ["AETHER_HOST_X"] = "box"
            self.assertEqual(tr.ssh_target({"host": "host-x"}), "u@box")
        tr.fleet_env._cache = None


if __name__ == "__main__":
    unittest.main()
