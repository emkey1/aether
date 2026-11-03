# Plan: a batch aimed at a fine-tune's own failures (drafted 2026-11-02)

The owner approved the pilot and the solver order on 2026-11-03. Everything after the pilot still needs a decision.

**The question:** does training data drawn from tasks a fine-tune gets wrong move its benchmark, where the same amount of
ordinary data did not? d5 added 286 pairs, about 15% more records, and `training_runs/qwen36-35b-a3b-v2/RESULTS.md`
measured no gain. Every earlier batch chose tasks by category or by what the guided base missed, never by what a fine-tune
missed.

## Which model to aim at

**Gemma 4 12B final (`gemma4-12b-v2`).** A run costs about 5.6 hours on host-c (3 h 7 min of training, 2.5 h of scoring)
against about 13.4 for the MoE, so seeds are affordable. It also has room to gain, at 30 (36) on held-out. A result here says
nothing certain about the MoE, which would need its own run if this works.

## The pipeline

1. **Tasks and Python:** `aether_distill.py` as for d5, through T'Ra's large-open lane, with the board-topic filter and the
   frozen expected output from Python. Held-out task text is never used.
2. **Filter (new):** the tuned model answers each frozen task with no guide and no repair, exactly as the board would ask.
   - `filter-cmd` prints the harness command, and `filter` writes `tasks_failed.json` and `tasks_passed.json` with each
     failure's class (coded, return code, wrong output).
   - The model's programs are only read for pass or fail. They never reach a pair. An offline test in
     `training/test_distill_offline.py` checks that.
   - On the 8-bit Mac form (`training_runs/gemma4-12b-v2-q8/`), so no host-c time. Which weights filter hardly matters, since this
     step only picks tasks.
3. **Solve with the guide:** see the decision below.
4. **Harvest and gates:** exact stdout against Python, `aether_specialization_check_pairs.py`, the dataset gates. Each pair's
   provenance gains the filter model and failure class.
5. **Dataset `v3-*`:** `v2-20261101` plus the batch, on the 1.0 grader. The recipe is otherwise identical to `gemma4-12b-v2`:
   one change per run.

## Who solves

**The guided Qwen3.8-27B first and MiMo on what it misses, as in d5.** MiMo wrote all of d5's Python. A pair where
MiMo also writes the Aether is only checked against itself, so a task it misreads the same way twice still passes. That
matters more here, because failed tasks will be enriched for hard and ambiguous statements. In d5 the guided 27B solved 277
of 293 and MiMo's 49 pairs were the exception.

MiMo alone is simpler and may solve more of the hard tasks, but then every pair has that weakness. Either way, pairs carry
solver tags and the write-up counts them.

## Pilot first

**100 fresh tasks,** through the filter and a solve of its failures, stopping before any dataset or training to report. It
replaces the guesses below with three measured numbers: how often Gemma fails fresh tasks, by class; how much time each stage
takes; and what share of the failures the solvers can solve.

**Result, 2026-11-03: two slices, `distill/f1-20261103/` and `distill/f2-20261103/`.** `distill/f1-20261103/pilot_report.py
[--batch DIR]` prints each. The rotation starts with the 14 plain categories, and f1's 13 requests never left them, so f2 took the
JSON, file and map categories and the one plain category f1 missed.

| | plain (f1) | JSON, file, map (f2) | together |
|---|---:|---:|---:|
| frozen tasks | 100 | 146 | 246 |
| **8-bit Gemma failed** | **19 (19%)** | **24 (16%)** | **43 (17%)** |
| wrong output / coded / return code | 11 / 6 / 2 | 17 / 5 / 2 | 28 / 11 / 4 |
| guided 27B solved (first time) | 15 (13) | 22 (16) | 37 (29) |
| MiMo solved of the rest | 4 of 4 | 0 of 2 | 4 of 6 |
| time: tasks / Python / filter / solve, min | 17 / 13 / 25 / 12 | 27 / 21 / 35 / 19 | 44 / 34 / 60 / 31 |

- **The filter does find harder tasks.** The guided 27B solves 29 of the 43 first time (67%), against 85% on d5's unfiltered tasks.
- **Failures cluster.** JSON tasks fail most: 14 of 33 (42%) across the three JSON categories. Map tasks barely fail, 7 of 88, and 5
  of those are inverted indexes, mostly a substring match where the task means a whole word. A rotation naturally yields JSON-heavy pairs.
- **Wrong output is two-thirds of failures,** so the limit below matters. Five of the 28 were read, and they are mixed: three
  logic slips, and two silent JSON API errors, a wrong key or path falling through `toon_get_*_or` to its default so every value is
  zero. The second kind is language knowledge that examples can teach.
- **Provenance is clean** in both slices: every open-lane request was served by MiMo. Grader timeouts: none in the filter or
  solver reports of f2, which ran while the Mac was under heavy unrelated load.

**Scale, from both slices:** matching d5's 286 pairs takes about 1,700 frozen tasks, about 20 hours unattended: 5 of task writing,
4 of Python, 7 of filtering at 8 bits on the Mac and 4 of solving. Duplicates took 16% of what the writer returned in f1, and that
may rise. The owner can set a lower target. The MiMo stages share T'Ra's queue, and the Mac's GPU is taken for about 11 hours.

## Runs

All on host-c with its services stopped, about 5.6 hours each.

| arm | dataset | seeds | status |
|---|---|---|---|
| baseline | `v2-20261101` | 3407 | done (`gemma4-12b-v2`) |
| baseline | `v2-20261101` | 42 | new; also the first measure of held-out seed noise |
| targeted | `v3-*` | 3407, 42 | new |
| control, only if targeted reads as a gain | `v2-20261101` + the same number of pairs from `tasks_passed.json` | 3407, 42 | new |

The control separates "targeted data helps" from "more data helps", and is skipped if there is no gain to explain.

## The reading, fixed before any targeted run is scored

Commit this section's final form and the run configs before training starts.

- **Measure:** first-attempt pass rate on the 53 held-out tasks at T=0, averaged over each arm's seeds. Targeted minus
  baseline, with a paired bootstrap over tasks (10,000 resamples, seed 42), as in `training_runs/qwen36-35b-a3b-e3-t02/compare.py`.
- **A gain needs all three:** the 95% interval is above zero; the difference is larger than the gap between the two baseline
  seeds; main's interval is not wholly below zero.
- **Reported but not deciding:** after repair, each suite, and coded / return code / wrong output.
- **Held-out failures stay unread,** as they have been, so these suites can judge the next batch too.

Earlier notes proposed held-out coded errors as the main measure, because they sit at 10 to 12 in all four fine-tunes. Total
passes replace it: v2 kept coded failures level while doubling wrong output, and Gemma's gap to e3 is not in coded errors at
all (14 against 16).

## Limits, stated now

- **Only a large gain is readable.** e3's T=0.2 interval on these 53 tasks was about 9 points either side, which is about 5
  tasks. A real gain of 2 or 3 tasks would read as no result.
- **Wrong output may not respond.** Those failures are logic slips rather than language errors, and more Aether examples may
  not fix them. The per-class counts will show which classes moved.
- **Repair is a separate question.** Repair fixed only 8 of Gemma's 29 failures. Repair pairs built from its broken programs
  might address that, but the fine-tune's own programs stay out of training data unless the owner decides otherwise, and that
  would be its own run.
