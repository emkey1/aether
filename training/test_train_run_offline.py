#!/usr/bin/env python3
"""Offline tests for tools/aether_train_run.py and the training registry (no ssh, no GPU)."""

from __future__ import annotations

import functools
import json
import pathlib
import shutil
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

    def test_lora_layers_are_every_layer_unless_a_run_names_them(self):
        self.assertNotIn("--layers-to-transform", tr.trainer_argv(self.load()))
        argv = tr.trainer_argv(self.load(lora={"layers_to_transform": list(range(20, 48))}))
        self.assertEqual(argv[argv.index("--layers-to-transform") + 1], ",".join(str(i) for i in range(20, 48)))
        for bad in ([], "20-47", [20, 20], [21, 20], [-1], [True], [1.0]):
            with self.assertRaises(SystemExit):
                self.load(lora={"layers_to_transform": bad})

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
    # check_generations.py --complete fails while FAKE_CHECK_RC says so; the harness itself always exits 0. A probe
    # (python3 -c) runs on the real interpreter.
    "python3": """case "$1" in -c) exec "$REAL_PYTHON3" "$@" ;; esac
case "$*" in *check_generations.py*--complete*) exit "${FAKE_CHECK_RC:-0}" ;; esac
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

    def bash(self, script: str, shell: str = "/bin/bash", **env) -> subprocess.CompletedProcess:
        path = self.root / "script.sh"
        path.write_text(script, encoding="utf-8")
        full = {"HOME": str(self.home), "PATH": f"{self.bin}:/usr/bin:/bin", "FAKE_STATE": str(self.state),
                "FAKE_NAME": tr.served_name(self.run, "base"), "REAL_PYTHON3": sys.executable} \
            | {k: str(v) for k, v in env.items()}
        return subprocess.run([shell, str(path)], env=full, capture_output=True, text=True, timeout=60)

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


class ScoreStageMemoryGateTests(unittest.TestCase):
    """A direct score refuses to start without the serve's device share. min_available_gb (30 GB for the 4B runs)
    passed on 2026-11-25 while vLLM's 0.85 of the device did not fit beside another process."""

    def stage(self, memory_out: str, ignore_memory: bool = False):
        import argparse
        import contextlib
        import io
        from unittest import mock
        with tempfile.TemporaryDirectory() as tmp:
            run = tr.load_run(write_run(pathlib.Path(tmp), min_available_gb=30))
        calls = []

        def fake_remote(run, command, *, check=True, capture=True):
            calls.append(command)
            if command.startswith("tmux has-session"):
                return subprocess.CompletedProcess([], 1, stdout="", stderr="")  # no scoring session yet
            if command.endswith("\nmemory_mib"):
                return subprocess.CompletedProcess([], 0, stdout=memory_out, stderr="")
            if command.startswith("free -g"):  # host_busy: 54 GB available, no container up
                return subprocess.CompletedProcess([], 0, stdout="54\n", stderr="")
            return subprocess.CompletedProcess([], 0, stdout="", stderr="")

        printed, error = io.StringIO(), None
        with mock.patch.object(tr, "remote", fake_remote), contextlib.redirect_stdout(printed):
            try:
                tr.stage_score(run, argparse.Namespace(when_ready=False, ignore_memory=ignore_memory))
            except SystemExit as exc:
                error = str(exc)
        started = any(c.startswith("tmux new-session") for c in calls)
        probed = any(c.endswith("\nmemory_mib") for c in calls)
        return started, probed, error, printed.getvalue()

    def test_short_of_the_share_refuses_with_the_numbers(self):
        started, probed, error, _ = self.stage("124000 56000\n")
        self.assertFalse(started)
        self.assertTrue(probed)
        self.assertIn("54 GiB of device memory available; the serve asks for 102 GiB (0.85 of 121 GiB", error)
        self.assertIn("score --when-ready", error)

    def test_enough_memory_starts_scoring(self):
        started, _, error, _ = self.stage("124000 110000\n")
        self.assertIsNone(error)
        self.assertTrue(started)

    def test_ignore_memory_skips_the_probe(self):
        started, probed, error, _ = self.stage("124000 56000\n", ignore_memory=True)
        self.assertIsNone(error)
        self.assertTrue(started)
        self.assertFalse(probed)

    def test_an_unreadable_probe_warns_and_starts(self):
        for out in ("", "[N/A] 5\n"):
            with self.subTest(out=out):
                started, _, error, printed = self.stage(out)
                self.assertIsNone(error)
                self.assertTrue(started)
                self.assertIn("warning: cannot read the device memory", printed)

    def test_the_share_matches_the_waiters(self):
        run = FakeHost(self).run
        self.assertEqual(tr.serve_share_mib(run, 124000), 105400)
        self.assertEqual(tr.serve_share_mib(run, 81920), 69632)
        self.assertEqual(tr.serve_share_mib(run, 81921), 69633)  # rounded up, as vLLM's check needs

    def test_the_probe_runs_in_a_posix_login_shell_too(self):
        host = FakeHost(self)
        for shell in ("/bin/bash", "/bin/sh"):
            with self.subTest(shell=shell):
                self.assertEqual(host.bash(tr.MEMORY_MIB + "\nmemory_mib", shell=shell).stdout.split(),
                                 ["124000", "110000"])
                self.assertEqual(host.bash(tr.MEMORY_MIB + "\nmemory_mib", shell=shell,
                                           FAKE_NVIDIA="81920, 20000").stdout.split(), ["81920", "20000"])


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



# --------------------------------------------------------------------------- the divergence stop

# gemma4-12b-c1's evals (2026-11-25): 0.14 at step 50, 6.59 at step 75, 4.13 at the end. Step 25 and the steps after 75
# stand in for evals the README does not give.
C1_EVAL_LOSSES = [0.31, 0.14, 6.59, 5.2, 4.13]


class FakeTrainerCallback:
    def on_train_begin(self, args, state, control, **kwargs):
        pass

    def on_evaluate(self, args, state, control, **kwargs):
        pass

    def on_save(self, args, state, control, **kwargs):
        pass


class FakeEarlyStopping(FakeTrainerCallback):
    def __init__(self, **kwargs):
        self.kwargs = kwargs


class FakeTokenizer:
    pad_token, eos_token, padding_side = None, "<eos>", "left"

    def apply_chat_template(self, messages, tokenize=False, add_generation_prompt=False, **kwargs):
        return "".join(f"<{m['role']}>{m['content']}" for m in messages)

    def __call__(self, text, add_special_tokens=False):
        return {"input_ids": list(range(len(text)))}

    def save_pretrained(self, path):
        pathlib.Path(path).mkdir(parents=True, exist_ok=True)
        (pathlib.Path(path) / "tokenizer_config.json").write_text("{}")


class FakeAutoTokenizer:
    loads = 0

    @classmethod
    def from_pretrained(cls, *args, **kwargs):
        cls.loads += 1
        return FakeTokenizer()


class FakeModel:
    def __init__(self):
        from types import SimpleNamespace
        self.config = SimpleNamespace(quantization_config=None)

    def named_modules(self):
        return [("model.layers.0.self_attn.q_proj.lora_A", None)]

    def named_parameters(self):
        return []

    def parameters(self):
        from types import SimpleNamespace
        return [SimpleNamespace(numel=lambda: 4, requires_grad=True)]

    def save_pretrained_merged(self, path, tokenizer, save_method):
        pathlib.Path(path).mkdir(parents=True, exist_ok=True)
        (pathlib.Path(path) / "config.json").write_text("{}")


class FakeFastLanguageModel:
    @staticmethod
    def from_pretrained(**kwargs):
        return FakeModel(), FakeTokenizer()

    @staticmethod
    def get_peft_model(model, **kwargs):
        return model


class FakeDataset(list):
    @classmethod
    def from_list(cls, rows):
        return cls(rows)

    @property
    def column_names(self):
        return list(self[0]) if self else []

    def remove_columns(self, names):
        return FakeDataset({k: v for k, v in row.items() if k not in names} for row in self)


class FakeSFTTrainer:
    """train() walks EVAL_LOSSES, one eval every eval_steps, as the HF Trainer calls its callbacks: on_train_begin, then
    per eval the log_history entry before on_evaluate, stopping after the eval at which one sets should_training_stop."""
    EVAL_LOSSES: list = []
    last = None

    def __init__(self, model, args, train_dataset, eval_dataset, **kwargs):
        self.model, self.args, self.train_dataset = model, args, train_dataset
        self.callbacks, self.evaluated = [], []
        FakeSFTTrainer.last = self

    def add_callback(self, callback):
        self.callbacks.append(callback)

    def train(self, resume_from_checkpoint=None):
        from types import SimpleNamespace
        state = SimpleNamespace(global_step=0, log_history=[], epoch=0.0)
        control = SimpleNamespace(should_training_stop=False)
        for callback in self.callbacks:
            callback.on_train_begin(self.args, state, control)
        for i, loss in enumerate(self.EVAL_LOSSES, start=1):
            state.global_step = i * self.args.eval_steps
            state.log_history.append({"eval_loss": loss, "step": state.global_step})
            for callback in self.callbacks:
                callback.on_evaluate(self.args, state, control, metrics={"eval_loss": loss})
            self.evaluated.append(state.global_step)
            if control.should_training_stop:
                break

    def save_model(self, path):
        pathlib.Path(path).mkdir(parents=True, exist_ok=True)
        (pathlib.Path(path) / "adapter_model.safetensors").write_bytes(b"")


@functools.lru_cache(maxsize=None)
def load_trainer():
    """tools/unsloth_qwen_coder_30b_sft.py over stand-ins for torch, datasets, unsloth, transformers and trl, which it
    imports at the top and which only the training image has."""
    import importlib.util
    import types
    from types import SimpleNamespace
    from unittest import mock
    stubs = {"peft": None}  # its peft patches are skipped on ImportError
    for name, attrs in (("torch", {"bfloat16": "bf16", "float16": "fp16", "manual_seed": lambda seed: None}),
                        ("datasets", {"Dataset": FakeDataset}),
                        ("unsloth", {"FastLanguageModel": FakeFastLanguageModel}),
                        ("transformers", {"AutoTokenizer": FakeAutoTokenizer, "TrainerCallback": FakeTrainerCallback,
                                          "EarlyStoppingCallback": FakeEarlyStopping}),
                        ("trl", {"SFTConfig": SimpleNamespace, "SFTTrainer": FakeSFTTrainer})):
        stubs[name] = types.ModuleType(name)
        stubs[name].__dict__.update(attrs)
    spec = importlib.util.spec_from_file_location("aether_sft_trainer_under_test", tr.TRAINER)
    module = importlib.util.module_from_spec(spec)
    find_spec = importlib.util.find_spec
    try:
        with mock.patch.dict(sys.modules, stubs):
            spec.loader.exec_module(module)
    finally:
        importlib.util.find_spec = find_spec  # the trainer patches it for the image's kernels package
    return module


class TrainerDivergenceStopTests(unittest.TestCase):
    """The trainer's divergence stop (--divergence-stop-ratio): gemma4-12b-c1 diverged at step 54 and trained on to
    step 978, and its export was the diverged model (2026-11-25)."""

    @classmethod
    def setUpClass(cls):
        cls.sft = load_trainer()

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.tmp = pathlib.Path(self._tmp.name)

    def main(self, out: pathlib.Path, *flags: str, losses=C1_EVAL_LOSSES) -> tuple[int, str]:
        import contextlib
        import io
        from unittest import mock
        data = self.tmp / "data"
        data.mkdir(exist_ok=True)
        rows = [{"id": f"i{n}", "kind": "program", "verification": {"returncode": 0},
                 "messages": [{"role": "user", "content": f"task {n}"}, {"role": "assistant", "content": "prog"}]}
                for n in range(3)]
        (data / "instruction.jsonl").write_text("".join(json.dumps(r) + "\n" for r in rows))
        (data / "repair.jsonl").write_text("")
        argv = ["trainer", "--instruction-jsonl", str(data / "instruction.jsonl"),
                "--repair-jsonl", str(data / "repair.jsonl"), "--output-dir", str(out),
                "--merged-output-dir", str(self.tmp / "merged_16bit"), "--eval-cases", "1", "--eval-steps", "25",
                "--no-train-on-responses-only", "--early-stopping-patience", "0", "--no-load-best-model-at-end", *flags]
        FakeSFTTrainer.EVAL_LOSSES, FakeSFTTrainer.last = list(losses), None
        printed = io.StringIO()
        with mock.patch.object(sys, "argv", argv), contextlib.redirect_stdout(printed):
            rc = self.sft.main()
        return rc, printed.getvalue()

    def metadata(self, out: pathlib.Path) -> dict:
        return json.loads((out / "run_metadata.json").read_text(encoding="utf-8"))

    def test_off_by_default_the_diverged_run_trains_to_the_end_and_exports(self):
        out = self.tmp / "train"
        rc, printed = self.main(out)
        self.assertEqual(rc, 0, printed)
        self.assertIn("divergence stop: off", printed)
        self.assertEqual(FakeSFTTrainer.last.evaluated, [25, 50, 75, 100, 125])
        self.assertFalse([c for c in FakeSFTTrainer.last.callbacks if isinstance(c, self.sft.DivergenceStop)])
        self.assertTrue((self.tmp / "merged_16bit" / "config.json").is_file())
        self.assertTrue((out / "final" / "adapter_model.safetensors").is_file())
        meta = self.metadata(out)
        self.assertIsNone(meta["divergence_stop_ratio"])
        self.assertIsNone(meta["divergence_stop_min_evals"])
        self.assertNotIn("diverged", meta)

    def test_stops_at_the_first_eval_above_the_ratio_without_an_export(self):
        out = self.tmp / "train"
        rc, printed = self.main(out, "--divergence-stop-ratio", "2")
        self.assertEqual(rc, self.sft.EXIT_DIVERGED, printed)
        self.assertEqual(FakeSFTTrainer.last.evaluated, [25, 50, 75])  # stopped at step 75, not trained to the end
        self.assertFalse((self.tmp / "merged_16bit").exists())
        self.assertFalse((out / "final").exists())
        self.assertFalse((out / "merged_export.json").exists())
        meta = self.metadata(out)
        self.assertEqual((meta["divergence_stop_ratio"], meta["divergence_stop_min_evals"]), (2.0, 2))
        record = meta["diverged"]
        self.assertEqual({k: record[k] for k in ("step", "eval_loss", "best_eval_loss", "best_step", "evals")},
                         {"step": 75, "eval_loss": 6.59, "best_eval_loss": 0.14, "best_step": 50, "evals": 3})
        self.assertIn("eval loss 6.5900 at step 75 is 47.07 x the best, 0.1400 at step 50", record["message"])
        self.assertIn("DIVERGED: " + record["message"], printed)

    def test_a_diverged_run_is_not_resumed(self):
        out = self.tmp / "train"
        self.main(out, "--divergence-stop-ratio", "2")
        record = self.metadata(out)["diverged"]
        loads = FakeAutoTokenizer.loads
        for flags in (["--divergence-stop-ratio", "2"], []):  # nor with the stop turned off
            with self.subTest(flags=flags):
                rc, printed = self.main(out, *flags, losses=[0.1] * 5)
                self.assertEqual(rc, self.sft.EXIT_DIVERGED)
                self.assertIn("Not resuming: retrain under a new tag", printed)
                self.assertIsNone(FakeSFTTrainer.last)  # refused before the trainer was built
                self.assertEqual(FakeAutoTokenizer.loads, loads)  # and before anything loaded
                self.assertEqual(self.metadata(out)["diverged"], record)  # the record stays
                self.assertFalse((self.tmp / "merged_16bit").exists())

    def test_a_healthy_run_is_untouched(self):
        healthy = [0.52, 0.33, 0.264, 0.228, 0.235, 0.208]  # a small rise (1.03 x) mid-run, as qwen38-27b-toon's
        off, on = self.tmp / "off", self.tmp / "on"
        self.main(off, losses=healthy)
        off_evals = FakeSFTTrainer.last.evaluated
        shutil.rmtree(self.tmp / "merged_16bit")
        rc, printed = self.main(on, "--divergence-stop-ratio", "2", losses=healthy)
        self.assertEqual(rc, 0, printed)
        self.assertEqual(FakeSFTTrainer.last.evaluated, off_evals)
        self.assertTrue((self.tmp / "merged_16bit" / "config.json").is_file())
        self.assertNotIn("diverged", self.metadata(on))

    def test_the_rule_waits_for_min_evals(self):
        stop = self.sft.DivergenceStop(2.0, 4)
        self.assertEqual([stop.check(25 * n, loss) for n, loss in enumerate([0.31, 0.14, 6.59], start=1)],
                         [None, None, None])
        record = stop.check(100, 5.2)  # eval 4: still above 2 x 0.14
        self.assertEqual((record["step"], record["best_step"], record["evals"]), (100, 50, 4))
        rc, printed = self.main(self.tmp / "train", "--divergence-stop-ratio", "2", "--divergence-stop-min-evals", "4")
        self.assertEqual(rc, self.sft.EXIT_DIVERGED)
        self.assertEqual(FakeSFTTrainer.last.evaluated, [25, 50, 75, 100])

    def test_a_nan_or_inf_eval_loss_stops_at_any_eval(self):
        for bad in (float("nan"), float("inf")):
            with self.subTest(bad=bad):
                stop = self.sft.DivergenceStop(2.0, 5)  # before min_evals
                record = stop.check(25, bad)
                self.assertEqual((record["step"], record["eval_loss"], record["best_eval_loss"]), (25, str(bad), None))
                self.assertIn("is not finite", record["message"])
                out = self.tmp / f"train-{bad}"
                rc, _ = self.main(out, "--divergence-stop-ratio", "2", losses=[0.31, 0.14, bad, 0.12])
                self.assertEqual(rc, self.sft.EXIT_DIVERGED)
                self.assertEqual(FakeSFTTrainer.last.evaluated, [25, 50, 75])
                text = (out / "run_metadata.json").read_text()
                self.assertNotIn("NaN", text)
                self.assertNotIn("Infinity", text)  # strict JSON
                self.assertEqual(self.metadata(out)["diverged"]["best_eval_loss"], 0.14)

    def test_a_resumed_run_keeps_its_earlier_best(self):
        import contextlib
        import io
        from types import SimpleNamespace
        stop = self.sft.DivergenceStop(2.0, 2)
        stop.on_train_begin(None, SimpleNamespace(log_history=[{"loss": 0.2, "step": 49},
                                                               {"eval_loss": 0.14, "step": 50}]), None)
        control = SimpleNamespace(should_training_stop=False)
        with contextlib.redirect_stdout(io.StringIO()):
            stop.on_evaluate(None, SimpleNamespace(global_step=75), control, metrics={"eval_loss": 6.59})
        self.assertTrue(control.should_training_stop)
        self.assertEqual((stop.stopped["best_step"], stop.stopped["evals"]), (50, 2))

    def test_the_fetched_healthy_histories_never_fire(self):
        # Three fetched runs keep their eval history; none rose above 1.06 x its best so far.
        for tag in ("qwen38-27b-algo", "qwen38-27b-l1-pilot", "qwen38-27b-toon"):
            with self.subTest(run=tag):
                state = json.loads((tr.RUNS_OUT_DIR / tag / "train" / "trainer_state.json").read_text())
                evals = [(e["step"], e["eval_loss"]) for e in state["log_history"] if "eval_loss" in e]
                self.assertGreater(len(evals), 10)
                stop = self.sft.DivergenceStop(2.0, 2)
                self.assertEqual([stop.check(step, loss) for step, loss in evals], [None] * len(evals))

    def test_bad_flags_are_refused(self):
        for flags in (["--divergence-stop-ratio", "1"], ["--divergence-stop-ratio", "0.5"],
                      ["--divergence-stop-ratio", "nan"],
                      ["--divergence-stop-ratio", "2", "--divergence-stop-min-evals", "0"]):
            with self.subTest(flags=flags), self.assertRaises(SystemExit) as ctx:
                self.main(self.tmp / "train", *flags)
            self.assertIn("--divergence-stop", str(ctx.exception))


class DivergenceStopDriverTests(unittest.TestCase):
    """train.divergence_stop_ratio in a run config, and what the driver does with a run the stop ended."""

    def load(self, **train):
        with tempfile.TemporaryDirectory() as tmp:
            return tr.load_run(write_run(pathlib.Path(tmp), **({"train": train} if train else {})))

    def test_off_unless_named_and_nothing_is_filled_in(self):
        for run in (self.load(), self.load(divergence_stop_ratio=None)):
            self.assertNotIn("divergence_stop_min_evals", run["train"])
            self.assertIsNone(run["train"].get("divergence_stop_ratio"))
            self.assertFalse([a for a in tr.trainer_argv(run) if a.startswith("--divergence")])
        self.assertNotIn("divergence_stop_ratio", self.load()["train"])  # a run without it resolves as before

    def test_a_ratio_and_min_evals_reach_the_trainer(self):
        argv = tr.trainer_argv(self.load(divergence_stop_ratio=2))
        self.assertEqual(argv[argv.index("--divergence-stop-ratio") + 1], "2")
        self.assertNotIn("--divergence-stop-min-evals", argv)  # the trainer's default
        argv = tr.trainer_argv(self.load(divergence_stop_ratio=2.5, divergence_stop_min_evals=4))
        self.assertEqual(argv[argv.index("--divergence-stop-ratio") + 1], "2.5")
        self.assertEqual(argv[argv.index("--divergence-stop-min-evals") + 1], "4")

    def test_bad_values_are_refused(self):
        for train in ({"divergence_stop_ratio": 1}, {"divergence_stop_ratio": 0.5}, {"divergence_stop_ratio": "2"},
                      {"divergence_stop_ratio": True}, {"divergence_stop_ratio": float("inf")},
                      {"divergence_stop_min_evals": 3},
                      {"divergence_stop_ratio": 2, "divergence_stop_min_evals": 0},
                      {"divergence_stop_ratio": 2, "divergence_stop_min_evals": 1.5},
                      {"divergence_stop_ratio": 2, "divergence_stop_min_evals": True}):
            with self.subTest(train=train), self.assertRaises(SystemExit) as ctx:
                self.load(**train)
            self.assertIn("train.divergence_stop", str(ctx.exception))

    def diverged_host(self, **overrides) -> FakeHost:
        """A host whose train/ holds what the trainer writes when the stop ends a c1-like run."""
        host = FakeHost(self, **overrides)
        train = host.remote / "train"
        sft = load_trainer()
        stop = sft.DivergenceStop(2.0, 2)
        record = [r for r in (stop.check(25 * n, x) for n, x in enumerate(C1_EVAL_LOSSES[:3], start=1)) if r][0]
        train.mkdir()
        (train / "checkpoint-75").mkdir()
        (train / "run_metadata.json").write_text(json.dumps({"seed": 3407, "diverged": record}, indent=2))
        return host

    def via(self, host: FakeHost, calls: list):
        def fake_remote(run, command, *, check=True, capture=True):
            calls.append(command)
            return host.bash(command)
        return fake_remote

    def stage(self, stage, host: FakeHost, args=None) -> tuple[int, str, list]:
        import argparse
        import contextlib
        import io
        from unittest import mock
        calls, printed = [], io.StringIO()
        args = args or argparse.Namespace(when_ready=False, ignore_memory=False)
        with mock.patch.object(tr, "remote", self.via(host, calls)), \
                mock.patch.object(tr, "stage_check", return_value=0), contextlib.redirect_stdout(printed):
            rc = stage(host.run, args)
        return rc, printed.getvalue(), calls

    def test_the_probe_reads_the_trainers_record(self):
        host = FakeHost(self)
        self.assertEqual(host.bash(tr.diverged_probe_cmd(host.run)).stdout, "")  # no run_metadata.json yet
        (host.remote / "train").mkdir()
        (host.remote / "train" / "run_metadata.json").write_text(json.dumps({"seed": 3407}, indent=2))
        self.assertEqual(host.bash(tr.diverged_probe_cmd(host.run)).stdout, "")
        self.assertIn("$HOME/aether-train/t-run/train/run_metadata.json", tr.diverged_probe_cmd(host.run))
        diverged = self.diverged_host()
        record = tr.diverged_in(diverged.bash(tr.diverged_probe_cmd(diverged.run)).stdout)
        self.assertEqual((record["step"], record["best_step"], record["best_eval_loss"]), (75, 50, 0.14))

    def test_status_reports_a_diverged_stop_and_exits_one(self):
        rc, printed, _ = self.stage(tr.stage_status, self.diverged_host())
        self.assertEqual(rc, 1, printed)
        self.assertIn("DIVERGED: training stopped at step 75: eval loss 6.5900 at step 75 is 47.07 x the best, "
                      "0.1400 at step 50", printed)
        self.assertIn("No final adapter or merged export was written", printed)
        self.assertIn("merged export: none (the run diverged)", printed)
        self.assertNotIn("merged export: not yet", printed)
        self.assertNotIn("diverged={", printed)
        rc, printed, _ = self.stage(tr.stage_status, FakeHost(self))
        self.assertEqual(rc, 0, printed)
        self.assertIn("merged export: not yet", printed)
        self.assertNotIn("DIVERGED", printed)

    def test_train_refuses_to_resume_a_diverged_run(self):
        import argparse
        for when_ready in (False, True):
            with self.subTest(when_ready=when_ready):
                rc, printed, calls = self.stage(tr.stage_train, self.diverged_host(),
                                                argparse.Namespace(when_ready=when_ready, ignore_memory=False))
                self.assertEqual(rc, 1, printed)
                self.assertIn("DIVERGED: training stopped at step 75", printed)
                self.assertFalse([c for c in calls if "train.sh" in c or "train_when_ready.sh" in c])
        rc, printed, calls = self.stage(tr.stage_train, FakeHost(self))
        self.assertEqual(rc, 0, printed)
        self.assertTrue([c for c in calls if c.endswith("/train.sh")])  # a healthy run still launches

    def test_the_score_waiter_gives_up_on_a_diverged_run(self):
        host = self.diverged_host(score={"variants": ["tuned"]})
        (host.remote / "score.sh").write_text('touch "$(dirname "$0")/scored"\n')
        result = host.bash(tr.score_when_ready_script(host.run))
        log = host.log("logs/when_ready.log")
        self.assertEqual(result.returncode, tr.EXIT_DIVERGED, log)
        self.assertIn("GAVE UP: training stopped diverged and wrote no export (retrain under a new tag): {", log)
        self.assertIn('"best_step":50', log)
        self.assertFalse((host.remote / "scored").exists())

    def test_the_driver_and_trainer_agree_on_the_exit_status(self):
        self.assertEqual(tr.EXIT_DIVERGED, load_trainer().EXIT_DIVERGED)


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
