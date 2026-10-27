# Small-guide A/B 2: the four host sections cut (2026-10-26)

The same design as [`../small_ab_20261026/`](../small_ab_20261026/README.md): one
board with both guides in each report, interleaved per task, so every task is
answered under both guides by the same model with the same seed. The question was
whether cutting the four host sections no small-board task uses (`par`, HTTP,
sockets, tasks and AI; 11,849 → 10,388 o200k) costs first-attempt accuracy.

**Result: close, but not shown non-inferior.** First attempt went from 68.9% to
66.7% (−2.3 points, 95% CI [−9.1, +4.6]). The D37d bound of −3 is not cleared, and
one model at three repeats could not clear it unless the cut were clearly better
(see "Power" below). While the board ran, after an interim read, the owner ruled
that a close result ships, with "close" defined as both point estimates within 3
points of the current guide and no rise in empty first attempts. The final board
met all three conditions, so the cut shipped as small guide 2026-10-26-3.

| | |
|---|---|
| compiler | aether `0d4444e`, language 2026-10-26-2 (label 1.0-beta15dev), Release, sha256 `adbdf17e…a35533` |
| current guide (`small_current`) | `aether_for_llms_with_small_contexts.md` 2026-10-26-1 as of aether `0d4444e`, sha256 `5b286812…985254` (the guide board 1 ran); 11,849 o200k / 12,614 Qwen3.5 tokens |
| cut guide (`small_hostcut`) | stamp 2026-10-26-3, the file committed with this record, sha256 `32cfc0e4…ee0def0`; 10,388 o200k / 11,052 Qwen3.5 tokens |
| model | qwen3.5-9b on OpenRouter, provider pinned (Parasail, bf16), destination `or-qwen35-9b` of `../b0_openrouter_20261010/destinations.openrouter_20261010.json`: T 0.2, output cap 96,000, stop null |
| tasks | the current manifests `bench/tasks_v2_pos.json` (35), `tasks_cs.json` (19), `tasks_hard_v2.json` (14), `tasks_hard_nontoon.json` (5): 73 tasks |
| settings | 3 repeats (seed 42 + r), 2 repair rounds, the no-retry rule (an empty first attempt is final); one report per suite and repeat |
| run | host-e, 2026-10-26 19:02Z to 2026-10-27 02:46Z, three repeat lanes of 6 workers. Provider failures (HTTP 429 rate limits, truncated responses, one request past 3,600 s) left cases unmeasured in 8 of the 12 units. Resume passes re-ran them, the last four units in a second driver run. Every unit holds every case for both guides, none unmeasured (`status.log`) |
| cost | $3.41 of key usage |

`analyze_small_ab.py --harness tools/aether_doc_bench.py --old small_current --new small_hostcut bench/results/small_ab_hostcut_20261026`
reproduces every number below from the packed reports. This copy of the script
leaves out any pair whose first attempt the provider never answered. None is left
in this board, and board 1 had none, so its numbers are unchanged.

## Results

219 paired cases (73 tasks × 3 repeats). FA = first attempt; FX = after repair
under the no-retry rule. Intervals are 95% percentile bootstrap of the paired
difference, resampling task clusters within each suite (4,000 draws).

| | current | cut | cut − current |
|---|---|---|---|
| FA | 151 (68.9%) | 146 (66.7%) | −2.3 pts [−9.1, +4.6] |
| FX | 171 (78.1%) | 168 (76.7%) | −1.4 pts [−6.8, +4.6] |
| no answer within the output budget | 44 | 43 | |
| silent wrong / coded / uncoded error | 7 / 16 / 1 | 4 / 26 / 0 | |
| FA among first attempts that held a program | 151 / 175 (86.3%) | 146 / 176 (83.0%) | |
| completion tokens (all attempts) | 5.23 M | 5.71 M | × 1.09 |
| prompt tokens (all attempts) | 3.28 M | 3.04 M | × 0.93 |

Case by case, the first attempt differs in 51 pairs: 28 pass only under the
current guide, 23 only under the cut one (two-sided sign test p = 0.58).

| suite | cases | FA current | FA cut | FX current | FX cut |
|---|---|---|---|---|---|
| tasks_v2_pos | 105 | 95 | 89 | 99 | 100 |
| tasks_cs | 57 | 35 | 39 | 43 | 42 |
| tasks_hard_v2 | 42 | 19 | 15 | 27 | 21 |
| tasks_hard_nontoon | 15 | 2 | 3 | 2 | 5 |

FA by repeat: current 74.0 / 65.8 / 67.1, cut 65.8 / 65.8 / 68.5.

- **Coded errors.** First-attempt coded errors rose from 16 to 26. ARR-001 (an
  array parameter written in place) went from 1 to 7, on `cs_nqueens`,
  `cs_quick_sort`, `hard2_heap_sort` and `hard2_expr_eval`. The cut removed nothing
  about arrays or parameters, so this board does not explain the shift.
- **`toon_parse_file`.** Every FX-001 on the board, 5 under the current guide and 7
  under the cut one, is the same mistake: `toon_parse_file` called outside `fx`.
  `toon_parse_file_simple` makes it in all three repeats under the cut guide and
  in none under the current one, and repair fixes all three. The TOON section is
  word for word the same in both guides. The cut did remove five examples
  wrapped in `fx { }`, which may matter; this board cannot tell.

## The noise floor

Board 1 ran the current guide on the same 219 cases with the same seeds, provider
and settings, against compiler `849cad0`. The two commits between add min/max
inference and make `fx fn` FX-001; no first attempt on either board writes
`fx fn` or draws TYPE-001 on `min`/`max`. Same
guide, two runs: FA 153 then 151, and 42 first attempts came out differently (22
pass → fail, 20 fail → pass). The provider's sampling is not reproducible case by
case even with a fixed seed. The 51 pairs that differ between the two guides here
are about that size.

## Power

With one model and 219 pairs that differ about a quarter of the time, the
interval is roughly ±7 points wide. Its lower bound clears −3 only when the new
guide measures about 4 points better. A guide that is truly as good as the
current one fails the D37d test at this size most of the time. To show
non-inferiority at −3 for a true difference of zero takes about 15 repeats of
the 73-task board, five times this one. The D37d margin was set for a panel of
4–5 local destinations. The next small-guide steps should run on a local board,
which has no key limit and can afford the repeats.

## Limits

- One model, the only one the small board runs (qwen3.5-9b, the 16K cohort).
- The cut's token saving is real (1,461 o200k, 1,562 Qwen3.5 per prompt) but did
  not show up as fewer tokens spent: the model reasoned 9% longer in total under
  the cut guide, inside the run-to-run noise above.
- The ship decision is the owner's call on a close result, not a D37d pass.
