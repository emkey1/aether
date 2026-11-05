# fe3-20261104: does e3 fail the tasks Gemma failed?

The owner asked whether the failure-targeted work in `../../PLAN-failure-targeted-batch.md` helps the other fine-tunes. This batch answers the first
part cheaply. e3 at mxfp4 answered the 1,776 frozen tasks of `f1`, `f2` and `f3-20261103` with no guide and no repair, on the 1.0 release binary, as
8-bit Gemma did. Nothing here is solved, harvested or trained on, and e3's programs are only read for pass or fail. `overlap.py` prints the numbers
below from the repo root.

It took 107 minutes on the scoring Mac, with no infrastructure failures, empty answers or length caps.

| of 1,776 tasks | 8-bit Gemma | e3 at mxfp4 |
|---|---:|---:|
| **failed** | **322 (18%)** | **230 (13%)** |
| coded | 105 | 51 |
| return code | 23 | 11 |
| wrong output | 194 | 168 |

| both failed | only Gemma | only e3 | neither |
|---:|---:|---:|---:|
| 121 | 201 | 109 | 1,345 |

| area | tasks | Gemma failed | e3 failed | both |
|---|---:|---:|---:|---:|
| core | 780 | 150 (19%) | 119 (15%) | 61 |
| JSON documents and data files | 335 | 113 (34%) | 69 (21%) | 48 |
| keyed data | 661 | 59 (9%) | 42 (6%) | 12 |

## Reading

- **The two models share hard tasks, but only partly.** 121 tasks failed on both, about 2.9 times the 42 expected if failures were unrelated. Even so,
  e3 fails only 38% of what Gemma fails.
- **Most of the 298 pairs are not aimed at e3.** It failed 98 of those tasks (33%). The other 200 are tasks it already passes, which makes them
  closer to the ordinary data of d5 than to targeted data.
- **e3's failures split three ways:** 98 have a pair, 23 are tasks both solvers already missed, and 109 are tasks Gemma passed, so no solver has tried
  them.
- **e3 is the stronger model on these tasks.** The tasks where exactly one model failed split 201 to 109 in e3's favour (sign test p about 2e-7), which
  the 126 benchmark tasks could not separate. Weights differ: this is e3 at 4 bits against Gemma at 8. These are first attempts only, and the task mix
  is not the benchmark's.
- **Its lead is mostly in language errors.** Coded and return-code failures are about half Gemma's, 62 against 128, while wrong output is close, 168
  against 194. Wrong output is 73% of e3's failures, and that is the class the plan warns more examples may not fix.
- **Gemma's worst category is not shared.** Reading a small JSON data file fails 43 of 58 on Gemma and 12 of 58 on e3. Added on 2026-11-05: a second Gemma seed fails 21 of the 58
  (`../fs42-20261105/`), so part of this gap was one seed's.

## What this means for a MoE run

A MoE run on `v3-20261104` would be a weak test, since two-thirds of the new pairs are not aimed at it. A real test needs e3's 109 unsolved failures
solved, which would give about 200 pairs on its own failures at the solvers' earlier rate. Those runs take about 13 hours each and three are needed:
a second baseline seed and two targeted seeds. Whether to go on depends on the verdict of `../../training_runs/gemma4-12b-v3/`.

Added on 2026-11-05: that verdict is no gain, so this does not go on.
