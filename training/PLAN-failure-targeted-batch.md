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
   - On the 8-bit Mac form if that exists by then, otherwise on host-c at 16-bit. Which weights filter hardly matters,
     since this step only picks tasks.
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

**Rough scale, to be replaced by the pilot:** matching d5's 286 pairs, for a direct comparison, takes about 1,350 tasks if
Gemma fails 25% of them and the solvers get 85% of those. At d5's rates that is a few hours of task writing and up to a
day of Python, and at 16-bit on host-c about 17 hours of filtering (about 45 s a task). The owner can set a lower target
after the pilot. All of it runs unattended, and the MiMo stages share T'Ra's queue.

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
