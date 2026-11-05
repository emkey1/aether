# fs42-20261105: what a second seed does on 1,776 fresh tasks

**Result, 2026-11-05: seed 42 is stronger on fresh first attempts, and half of the 298 pairs' tasks pass at it.** See "Result" below.

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

## Result (2026-11-05)

The pass took 8 h 18 min on the scoring Mac, under heavy unrelated load for much of it. All 1,776 prompts match the published seed's, with no infrastructure failures or empty answers, and one answer hit the length cap (three on the published seed). `overlap.py` prints the tables.

| of 1,776 tasks, first attempt | published seed, int8 | seed 42, int8 | e3, mxfp4 |
|---|---:|---:|---:|
| **failed** | **322 (18.1%)** | **272 (15.3%)** | **230 (13.0%)** |
| coded | 105 | 101 | 51 |
| return code | 23 | 11 | 11 |
| wrong output | 194 | 160 | 168 |

| both seeds failed | only the published seed | only seed 42 | neither |
|---:|---:|---:|---:|
| 172 | 150 | 100 | 1,354 |

| area | tasks | published seed | seed 42 | e3 |
|---|---:|---:|---:|---:|
| core | 780 | 150 (19%) | 131 (17%) | 119 (15%) |
| JSON documents and data files | 335 | 113 (34%) | 79 (24%) | 69 (21%) |
| keyed data | 661 | 59 (9%) | 62 (9%) | 42 (6%) |

**Reading 1: seed 42 is stronger on fresh first attempts.** It fails alone on 100 tasks and the published seed on 150, p=0.0019. The tasks were new to both seeds and played no part in choosing seed 42, so this does not share the weakness of the int8 pass on the 126 tasks. It compares two sets of weights, on first attempts only.

**Reading 2: supports it.** Seed 42 passes 150 of the 298 pairs' tasks (50%), above the third fixed in advance.

- **Half of a seed's failures repeat.** Seed 42 also fails 53% of the published seed's failures, 3.5 times chance, against e3's 38%. Wrong output and coded failures repeat at about the same rate, 56% and 54%, and return codes less, 26%.
- **The stable list is smaller than the batch.** 172 tasks fail at both seeds: 148 have a pair, 24 do not, and e3 also fails 83 of the 172.
- **This does not explain the no-gain result by itself.** `v3-20261104` contained all 148 of those pairs, about 6% of its records, and still showed no gain. The other 150 may have diluted them, but only another training run could test that, and adding fewer pairs from the same set is a weak bet.
- **The gap to e3 narrows, and what remains is coded errors.** The split against e3 goes from 201 to 109 at the published seed to 166 to 124 at seed 42, p=0.016. Coded failures are about twice e3's at both Gemma seeds, 105 and 101 against 51, while seed 42's wrong output is below e3's. The 126 tasks could not show this, at 14 and 23 against 16. e3 is at 4 bits and Gemma at 8.
- **Gemma's worst category was partly one seed's.** Reading a small JSON data file fails 43 of 58 at the published seed, 21 at seed 42 and 12 on e3.

**The two grader timeouts stand.** The README's limit is 20 seconds and the pass ran under load, so `replay_timeouts.py` graded both programs again: three times at the limit and once with 300 seconds. Both timed out every time, so the load did not cause them and no number changes (`replay_timeouts.json`). One is also a failure on both other models, and the other is also a failure on e3.

**Not tested:** seed 42's lead after repair, as this pass has no repair. The published form is unchanged, and whether this changes which seed should be published is the owner's decision. The reading in `../../training_runs/gemma4-12b-v2-s42-q8/` still stands for that test.
