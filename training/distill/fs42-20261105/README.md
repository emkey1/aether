# fs42-20261105: what a second seed does on 1,776 fresh tasks

The 126 benchmark tasks cannot tell two seeds apart without spending the suites that judge them, and int8 was a weak second sample of them: 77 of 126 first programs
stayed byte-identical to the 16-bit form (`../../training_runs/gemma4-12b-v2-s42-q8/RESULTS.md`). This batch asks the same model on many more tasks. Seed 42 of
`gemma4-12b-v2` at int8 answers the 1,776 frozen tasks of `f1`, `f2` and `f3-20261103`, with no guide and no repair, on the 1.0 release binary, exactly as the published
seed did. Neither seed trained on these tasks, because both used dataset `v2-20261101`, and the pairs made from them entered only at `v3-20261104`.

Nothing here is solved, harvested or trained on, and the programs are only read for pass or fail. `overlap.py` prints the numbers from the repo root.

## The reading, fixed before the pass

**1. Is either seed stronger on fresh first attempts?** Tasks only one seed fails, with an exact two-sided sign test. One seed is stronger if p < 0.05. Otherwise this
cannot separate them. This compares these two sets of weights on first attempts. It does not test seed 42's lead after repair, because this pass has no repair.

**2. Were the 298 pairs partly aimed at one seed's noise?** The pairs came from the published seed's failures, and a new training run rerolls which tasks fail. The
measure is the share of the 298 pairs' tasks that seed 42 passes.

| seed 42 passes | reading |
|---|---|
| at least a third | supports it: a material part of the batch aimed at tasks a fresh run passes anyway |
| less than a fifth | against it: the batch aimed mostly at stable failures |
| between | unclear |

A third is the benchmark's rate: 10 of the published seed's 29 first-attempt failures pass at seed 42. The list is selected on one seed's failures, so some of this
share is regression to the mean, which is the effect in question. Even a clear yes would not show why `../../training_runs/gemma4-12b-v3/` found no gain. Only another
training run could, and that needs the owner's word.

**Reported, not deciding:** overlap against chance and against e3's 38% of the same failures, each failure class, each area and category, and how many tasks fail at both
seeds and have no pair yet. Tasks that fail at both seeds are the list a later batch could aim at.

## Limits, stated now

- **First attempts only,** at int8, with the task mix of these batches rather than the benchmark's.
- **Two seeds cannot give a task's failure rate.** A task that passes at one seed may still fail most of the time.
