# Benchmark harness fixes of 2026-11-24: what they do to the archive

**Question.** The review of 2026-11-24 found that the harness
(`tools/aether_doc_bench.py`) and the re-run tool
(`bench/rerun_nogen_cases.py`) could score the wrong text as the program,
score a carriage return as a newline, re-run a case under a later guide or
other settings, and ask a measured first attempt again. Fixing the first two
changes how an answer is read and scored. Does any archived verdict change?

No decision row: these are instrument fixes, not language changes. The
harness's sha in each report (`harness.files`) identifies which rule scored
it.

## 1. Answer extraction and line endings over the archive

```bash
git show fbbe472:tools/aether_doc_bench.py > tools/_adb_old_probe.py
python3 results/decisions/bench_tools_archive_probe.py . tools/_adb_old_probe.py \
    build/aether probe.json
rm tools/_adb_old_probe.py
```

Run on the build machine (macOS arm64) with the language `2026-11-24-2`
binary, over the 307 harness reports under `bench/results`.

| | count |
|---|---:|
| attempts in the archive | 12,542 |
| ... that stored the model's raw reply | 11,217 |
| ... whose reply holds a code fence | 3,123 |
| stored program differs from what the old rule extracts today | 0 |
| new rule extracts a different program | 4 |
| ... of those, verdict changes (re-graded, same binary) | 0 |
| distinct archived Aether programs re-run | 6,085 |
| ... whose stdout or stderr holds a carriage return | 0 |

The four replies are from one hosted 9B model that printed its reasoning as
the answer: the first fence was the task's input data or a sketch of the
expected grid, and the program came in a later ` ```aether ` block. The old
rule scored the data; the new rule scores the program. All four fail either
way (the programs do not compile or print the wrong thing).

So both changes leave every archived verdict as it was, and stored stdout is
still comparable with earlier reports.

## 2. What the archive cannot say about re-drawn first attempts

Until this change, a case whose repair round failed in the infrastructure
was dropped on `--resume`, or replaced by `rerun_nogen_cases.py`, and asked
again from the first prompt. Only a case whose first attempt failed can reach
a repair round, so the second draw went to failed first attempts only, and
first-attempt accuracy reads high by however many of them passed the second
time.

| in the 307 tracked reports (7,635 cases) | count |
|---|---:|
| cases that carry a re-run mark (`rerun_of_infra_failure`) | 218 |
| cases dropped on a resume (`resumed[].dropped_infra_cases`) | 99 |
| cases still unmeasured | 30 |
| ... of those, with a measured first attempt that is now kept | 2 |

The marks record the infra kind and the fingerprint, not the stage the
failure happened at, and a dropped case leaves only a count. So how many of
the 317 had a measured first attempt, and how many of those changed class on
the second draw, cannot be recomputed. 317 of 7,635 is the upper bound on the
cases affected. The resume drops are concentrated: 43 in
`small_ab_hostcut_20261026` and 21 in `small_ab_20261026`. Read the
first-attempt numbers of those two boards with that in mind; the others have
at most 19.

From this change on, such a case keeps its measured attempts and continues
after them, and says so (`continued_after_infra.kept_attempts`,
`rerun_of_infra_failure.kept_attempts` and `infra_stage`).

## 3. Tests

`python3 bench/test_doc_bench_offline.py`: the new cases cover extraction
(info strings, sample output before the program, each runner's own
language), carriage returns, the resume check on temperature, output budget,
extra body and compiler flags, `--resume-allow-mismatch`, continuing a case
after one and after two measured attempts, and the re-run tool refusing a
changed guide or destination, accepting a restored guide, and recording an
allowed mismatch.
