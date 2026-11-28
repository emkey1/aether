#!/usr/bin/env python3
"""The contamination gate must fail a contaminated guide (CTest
aether_guide_contamination_seeded, label `guide`).

Writes three one-block guides to a temp directory and runs
tools/check_guide_contamination.py --strict on each:

  stdout  a program printing one board task's expected stdout   -> exit 1
  ident   prose naming one board support-module identifier      -> exit 1
  clean   a program printing text no board task expects         -> exit 0

The seeded task and identifier are picked at run time from a board manifest
that is not sealed, and the test prints only their task ids, so no task's
expected output ever lands in a tracked file or a log. Needs AETHER_BIN (the
stdout check runs the block).
"""

from __future__ import annotations

import os
import pathlib
import re
import subprocess
import sys
import tempfile

REPO = pathlib.Path(__file__).resolve().parents[2]
TOOL = REPO / "tools" / "check_guide_contamination.py"
sys.path.insert(0, str(REPO / "tools"))
import aether_specialization_corpus_policy as policy  # noqa: E402
import check_guide_contamination as gate  # noqa: E402

PRINTABLE = {chr(c) for c in range(0x20, 0x7F)} | {"\n", "\t"}


def aether_text(value: str) -> str:
    escaped = value.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n").replace("\t", "\\t")
    return '"' + escaped + '"'


def open_manifests() -> list[pathlib.Path]:
    return [path for path in policy.default_board_manifests() if path.name not in policy.SEALED_MANIFESTS]


def seeded_stdout_task(manifests: list[pathlib.Path]) -> tuple[str, str]:
    for manifest in manifests:
        for task in policy.load_tasks(manifest):
            expected = task.get("expected_stdout")
            if (isinstance(expected, str) and 0 < len(expected) <= 400 and set(expected) <= PRINTABLE
                    and not set(expected) & set("{}%")):  # no placeholder for FMT-001 to flag
                return f"{manifest.stem}:{task.get('id')}", expected
    raise SystemExit("no open board task has a short ASCII expected_stdout to seed")


def seeded_identifier(manifests: list[pathlib.Path]) -> tuple[str, str]:
    identifiers = gate.board_facts(manifests)["identifiers"]
    if not identifiers:
        raise SystemExit("no open board task has a distinctive support-module identifier to seed")
    name = sorted(identifiers)[0]
    return ", ".join(sorted(identifiers[name])), name


def run_gate(docs: pathlib.Path, guide: str, aether_bin: str) -> subprocess.CompletedProcess:
    return subprocess.run(
        [sys.executable, str(TOOL), "--strict", "--docs-dir", str(docs), "--guide", guide,
         "--aether-bin", aether_bin],
        text=True, capture_output=True,
    )


def main() -> int:
    aether_bin = os.environ.get("AETHER_BIN") or str(REPO / "build" / "aether")
    if not pathlib.Path(aether_bin).is_file():
        print(f"no aether binary at {aether_bin}: set AETHER_BIN", file=sys.stderr)
        return 1
    manifests = open_manifests()
    stdout_task, expected = seeded_stdout_task(manifests)
    ident_tasks, identifier = seeded_identifier(manifests)
    guides = {
        "stdout": (
            "# Seeded\n\n```aether\nfn main() -> Void {\n"
            f"    fx {{ print({aether_text(expected)}); }}\n    ret;\n}}\n```\n",
            1, "stdout=1",
        ),
        "ident": (f"# Seeded\n\nCall `{identifier}` from the support module.\n", 1, "identifiers=1"),
        "clean": (
            '# Clean\n\n```aether\nfn main() -> Void {\n    fx { println("seeded clean block 7f3a"); }\n'
            "    ret;\n}\n```\n",
            0, "stdout=0",
        ),
    }
    # Each seeded program must itself run, or a stdout verdict would mean nothing.
    for name in ("stdout", "clean"):
        source = guides[name][0].split("```aether\n", 1)[1].split("```", 1)[0]
        returncode, _ = gate.run_block(pathlib.Path(aether_bin).resolve(), source)
        if returncode != 0:
            print(f"FAIL the seeded {name} program does not run (rc={returncode})")
            return 1
    failures = []
    with tempfile.TemporaryDirectory(prefix="aether-contam-seeded-") as tmp_name:
        docs = pathlib.Path(tmp_name)
        for name, (text, want_rc, want_total) in guides.items():
            (docs / f"{name}.md").write_text(text, encoding="utf-8")
            proc = run_gate(docs, f"{name}.md", aether_bin)
            summary = proc.stdout.strip().splitlines()[-1] if proc.stdout.strip() else proc.stderr.strip()
            ok = proc.returncode == want_rc and re.search(rf"\b{want_total}\b", summary) is not None
            print(f"{'ok  ' if ok else 'FAIL'} {name}: rc={proc.returncode} (want {want_rc}), {want_total} wanted")
            if not ok:
                failures.append(name)
                print(f"     gate said: {summary[:300]}")
    print(f"seeded from {stdout_task} (stdout) and {ident_tasks} (identifier)")
    if failures:
        print(f"{len(failures)} of {len(guides)} seeded guides got the wrong verdict: {', '.join(failures)}")
        return 1
    print("the strict gate fails both contaminated guides and passes the clean one")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
