# qwen36-35b-a3b-e3-t02: results (2026-11-02)

**By the reading fixed beforehand, e3's T=0 held-out gain of 6 tasks is not supported, and the 3-epoch recipe has no
measured basis over 2 epochs.** Epoch 3 minus epoch 2 on held-out first attempts is +6.4 points, about 3.4 tasks, with a
95% interval of -2.3 to +15.5. The interval includes 0, though most of it is above 0. e3 stays the release model, as the
README said it would whichever way this came out.

`compare.py` prints everything here. Each adapter ran 630 cases (126 tasks, seeds 42 to 46) on grader 2026-10-30-1, with
no infra failures, empty generations or length caps. All 126 prompts match the T=0 reports.

## The comparison

Mean per-task pass rate, with the same rate as tasks in brackets. The difference is epoch 3 minus epoch 2 in points, with
its 95% paired-bootstrap interval over tasks.

| | epoch 3 | epoch 2 | difference |
|---|---|---|---|
| **held-out (53), first attempt: the question** | **68.3% (36.2)** | **61.9% (32.8)** | **+6.4 [-2.3, +15.5]** |
| held-out, after repair | 84.2% (44.6) | 75.1% (39.8) | +9.1 [+2.3, +16.6] |
| main (73), first attempt | 89.6% (65.4) | 93.4% (68.2) | -3.8 [-8.5, +0.3] |
| main, after repair | 95.3% (69.6) | 95.6% (69.8) | -0.3 [-2.2, +1.9] |
| wrong output, held-out, first attempt | 7.2% (3.8) | 14.0% (7.4) | -6.8 [-13.6, -1.1] |
| wrong output, held-out, after repair | 6.4% (3.4) | 11.7% (6.2) | -5.3 [-11.3, -0.0] |
| wrong output, main, first attempt | 2.7% (2.0) | 1.1% (0.8) | +1.6 [-0.5, +4.7] |
| wrong output, main, after repair | 1.9% (1.4) | 0.8% (0.6) | +1.1 [-1.1, +3.6] |

The other rows do not change the reading. They do suggest what to test next: the third pass may trade a little main for
fewer wrong answers on held-out, and held-out after repair is the one pass-rate interval that clears 0. These are eight
intervals with no correction for looking at eight, on suites that helped choose e3, so none of them is a finding yet.

| suite | epoch 3 | epoch 2 |
|---|---|---|
| tasks_v2_pos (35) | 33.8 (34.8) | 34.8 (35.0) |
| tasks_hard_v2 (14) | 11.8 (13.6) | 13.2 (14.0) |
| tasks_hard_nontoon (5) | 2.8 (4.2) | 3.8 (4.2) |
| tasks_cs (19) | 17.0 (17.0) | 16.4 (16.6) |
| tasks_frontier (15) | 9.0 (13.6) | 8.8 (12.4) |
| tasks_frontier_algo (14) | 9.2 (10.8) | 8.8 (9.8) |
| tasks_frontier_spec (12) | 8.4 (9.6) | 7.0 (8.2) |
| tasks_sealed_toon (12) | 9.6 (10.6) | 8.2 (9.4) |

## What this says about the T=0 scores

They hold up well. At T=0.2 the scores land close to the single T=0 sample:

| | T=0 | T=0.2, 5 repeats |
|---|---|---|
| e3 | 65 (70) / 39 (47) | 65.4 (69.6) / 36.2 (44.6) |
| e3, epoch 2 | 68 (70) / 33 (42) | 68.2 (69.8) / 32.8 (39.8) |

- e3's 39 was the one flattering first-attempt number, by about 3 tasks. The README's warning that it was slightly
  flattering was right.
- Most tasks do not depend on the sample: 108 and 106 of the 126 are unanimous across 5 repeats. The T=0 result matches
  the T=0.2 majority on 119 and 123.

## Sampling is not what moves the results

The tasks that move between the adapters move consistently across repeats, out of 5 and in both directions:

- **Held-out, better at epoch 3:** `algo_sliding_window_max` +5, `algo_knapsack_items` +5, `spec_histogram_bins` +4,
  `sealed_toon_meter_validation` +4, `spec_csv_quoting` +3, `par_shard_tally` +3.
- **Held-out, worse at epoch 3:** `algo_record_pipeline` -5, `text_slice_tokenize` -3.
- **Main, better at epoch 3:** `cs_quick_sort` +3.
- **Main, worse at epoch 3:** `mstream_roundtrip` -5, `hard2_heap_sort` -4, `hard2_event_sessionize` -3,
  `hard_expense_outliers` -3.

So a third pass really does change what these weights do on individual tasks, which is why a few flips can swing a score.
Whether a retrain would flip the same tasks is what this test cannot say: it is one training run, and T=0.2 only samples
from weights already trained.

## Next

- e3 stays the release model. Held-out does not pick between these adapters.
- A second seed is still the missing test, and it is now the larger unknown: sampling noise is measured and small, and
  seed noise has never been measured on held-out.
- New recipes keep the epoch 2 adapter and score it as well, rather than assuming the third pass helps. The Gemma 4 12B
  run does this.
