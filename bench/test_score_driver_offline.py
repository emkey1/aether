#!/usr/bin/env python3
"""Offline tests for tools/check_bench_report.py and the refusals of tools/score_ollama_model.sh (no Ollama, no model).

    python3 bench/test_score_driver_offline.py
"""
from __future__ import annotations

import io
import contextlib
import json
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import check_bench_report as cbr  # noqa: E402

DRIVER = ROOT / "tools" / "score_ollama_model.sh"


def case(source: str) -> dict:
    return {"attempts": [{"source_code": source}]}


def variant(doc_name="none", sources=("print(1)",), infra=0, sha=None, tokens=None, key="results") -> dict:
    prefix = key[: -len("results")]
    v = {"doc_name": doc_name, "doc_sha256": sha, "doc_tokens_o200k": tokens, "doc_approx_tokens": tokens or 0,
         key: [case(s) for s in sources],
         f"{prefix}summary": {"total_cases": len(sources), "first_attempt_exact": 1, "final_exact": 1,
                              "infra_failed": infra}}
    return v


def report(*variants) -> dict:
    return {"destinations": [{"variants": list(variants)}]}


def problems(rep, leaks=("thinking",), complete=True, guide=None, no_guide=False):
    with contextlib.redirect_stdout(io.StringIO()):
        return cbr.check(rep, list(leaks), complete, guide, no_guide)


def test_a_clean_unguided_report_passes():
    assert problems(report(variant()), no_guide=True) == []


def test_all_empty_generations_fail():
    assert any("empty" in p for p in problems(report(variant(sources=("", "  ")))))
    assert problems(report(variant(sources=("", "print(1)")))) == []  # some empty answers are measurements


def test_a_reasoning_leak_fails_case_insensitively():
    assert any("reasoning" in p for p in problems(report(variant(sources=("Thinking about it...",)))))


def test_unmeasured_cases_fail_only_when_complete_is_asked():
    rep = report(variant(infra=2))
    assert any("unmeasured" in p for p in problems(rep))
    assert problems(rep, complete=False) == []


def test_a_guided_report_must_carry_the_named_guide():
    good = report(variant("card", sha="ab" * 32, tokens=13500))
    assert problems(good, guide=("card", "ab" * 32)) == []
    assert any("expected guide" in p for p in problems(good, guide=("card", "cd" * 32)))
    assert any("expected guide" in p for p in problems(report(variant()), guide=("card", "ab" * 32)))
    assert any("no tokens" in p for p in problems(report(variant("card", sha="ab" * 32, tokens=0)),
                                                   guide=("card", "ab" * 32)))


def test_an_unguided_run_must_not_carry_a_guide():
    assert any("none was asked" in p for p in problems(report(variant("card", sha="ab" * 32, tokens=9)),
                                                       no_guide=True))


def test_baseline_case_lists_are_checked():
    rep = report(variant(sources=("",), key="python_baseline_results"))
    assert any("python_baseline" in p and "empty" in p for p in problems(rep))
    assert problems(report({"doc_name": "none"})) == ["none: no cases in the report"]


def test_the_cli_exits_nonzero_on_a_problem():
    with tempfile.TemporaryDirectory() as tmp:
        path = pathlib.Path(tmp) / "r.json"
        path.write_text(json.dumps(report(variant(sources=("<think>",)))), encoding="utf-8")
        proc = subprocess.run([sys.executable, str(ROOT / "tools" / "check_bench_report.py"), str(path),
                               "--leak", "<think"], capture_output=True, text=True)
        assert proc.returncode == 1 and "reasoning" in proc.stdout, proc.stdout


def driver(*args: str) -> subprocess.CompletedProcess:
    return subprocess.run(["bash", str(DRIVER), *args], capture_output=True, text=True, timeout=60)


def test_the_driver_refuses_before_touching_ollama():
    with tempfile.TemporaryDirectory() as tmp:
        out = str(pathlib.Path(tmp) / "out")
        grader = pathlib.Path(tmp) / "aether"
        grader.write_text("#!/bin/sh\necho aether\n", encoding="utf-8")
        assert "required" in driver("--tag", "t").stderr
        assert "--guide" in driver("--tag", "t", "--out", out, "--model", "m", "--guided-suites", "v2_pos",
                                   "--grader", str(grader), "--grader-sha256", "0").stderr
        assert "no suite" in driver("--tag", "t", "--out", out, "--model", "m", "--suites", "no_such_suite",
                                    "--grader", str(grader), "--grader-sha256", "0").stderr
        proc = driver("--tag", "t", "--out", out, "--model", "m", "--suites", "v2_pos",
                      "--grader", str(grader), "--grader-sha256", "0" * 64)
        assert proc.returncode == 2 and "grader's sha256" in proc.stdout, proc.stdout + proc.stderr
        assert (pathlib.Path(out) / "finished").read_text().startswith("FAILED the grader")
        sha = subprocess.run(["shasum", "-a", "256", str(grader)], capture_output=True, text=True).stdout.split()[0] \
            if subprocess.run(["which", "shasum"], capture_output=True).returncode == 0 else None
        if sha:
            proc = driver("--tag", "t", "--out", out, "--model", "m", "--guided-suites", "v2_pos",
                          "--guide", f"card={tmp}/missing.md", "--grader", str(grader), "--grader-sha256", sha)
            assert proc.returncode == 2 and "guide" in proc.stdout and "missing" in proc.stdout, proc.stdout


def _main() -> int:
    failures = 0
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            try:
                fn()
            except Exception as exc:  # noqa: BLE001
                failures += 1
                print(f"FAIL {name}: {type(exc).__name__}: {exc}")
            else:
                print(f"ok   {name}")
    print("FAILED" if failures else "all tests passed")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(_main())
