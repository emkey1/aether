# Card `2026-10-20-2` against `2026-11-19-1`, one bundle, 2026-11-19

The card changes only at a release boundary, and each change runs the old and
the new card once on the same bundle (`docs/aether_guide_changelog.md`). Card
`2026-11-19-1` is prose cuts only, made to bring the card back under its
2,000-token ceiling; this is its run.

**Reading: no readable difference.** The run is too small, and too many cases
ended without a program, to show an effect of either sign.

## Setup

| | |
|---|---|
| Cards | old `2026-10-20-2` (sha256 `d00ce86deb28…`, the file at `3ae28f8~1`), new `2026-11-19-1` (sha256 `b52fd8d53a62…`) |
| Bundle | `tasks_v2_pos` (35), `tasks_cs` (19), `tasks_traps` (24): 78 tasks, both cards in one report per suite, interleaved per task |
| Model | Qwen3.5 9B, MLX 8-bit, served locally on the scoring Mac through the queue; temperature 0.2, 24,000 output tokens, no stop marker |
| Harness | one repeat, seed base 42, two repair attempts; compiler `2026-11-18-1` (sha256 `3623b436d939…`) |

The queue host lost power during `tasks_v2_pos`. The 39 cases that failed in
the infrastructure (20 old, 19 new) were re-run with
`bench/rerun_nogen_cases.py --report … --apply` against the same two card
files, and are marked `rerun_of_infra_failure` in the report. `tasks_cs` and
`tasks_traps` ran entirely after the outage.

## Result

| suite | card | right first time | right after repair | no program on the first attempt |
|---|---|---:|---:|---:|
| v2_pos (35) | old | 21 | 21 | 10 |
| | new | 21 | 22 | 9 |
| cs (19) | old | 7 | 9 | 9 |
| | new | 7 | 8 | 8 |
| traps (24) | old | 7 | 9 | 12 |
| | new | 5 | 10 | 12 |
| **all (78)** | **old** | **35** | **39** | **31** |
| | **new** | **33** | **40** | **29** |

- Paired by task, first attempt: 9 tasks are right under the old card only and
  7 under the new card only. After repair: 9 and 10.
- On the 35 tasks where both cards produced a program on the first attempt,
  each card is right on 28.
- "No program" means the model spent its whole output budget reasoning. It is
  a model outcome, counted as a failure, and it happens on about 38% of first
  attempts under both cards.

Recompute the table with `python3 bench/results/card_ab_20261119/tabulate.py`.

## Limits

One model, one repeat, 78 tasks: a difference of two tasks is noise (the
runbook's rule on single-run deltas). This local board is comparable only with
itself, not with the hosted boards of the same model. The reports are packed
with `bench/results/pack_reports.py`.
