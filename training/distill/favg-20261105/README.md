# favg-20261105: averaging two seeds' weights, read on 1,776 fresh tasks

**Result, 2026-11-06: worse than seed 42, and worse than either parent.** See "Result" below.

Two seeds of `gemma4-12b-v2` disagree on many tasks (`../fs42-20261105/`). Choosing one throws the other away, and running both costs twice the memory. This batch tests the third way, which adds nothing at inference: average the two merged 16-bit exports tensor by tensor, build int8 the same way as each parent, and let it answer the same 1,776 frozen tasks with no guide and no repair, on the 1.0 release binary. No training is involved. Neither parent trained on these tasks.

The published work is mixed. [Model soups](https://proceedings.mlr.press/v162/wortsman22a.html) gained by averaging fine-tuned models that differed in hyperparameters. [AdapterSoup](https://ar5iv.labs.arxiv.org/html/2302.07027) reports, without numbers, that averaging bottleneck adapters from different random seeds performed poorly. Averaging merged weights averages each LoRA's whole change to the base, which should avoid the mismatch between two LoRAs' factor matrices, but that has not been tested here.

Nothing here is solved, harvested or trained on, and the programs are only read for pass or fail. `average_weights.py` builds the export, and `overlap.py` prints the numbers from the repo root.

## The reading, fixed before the pass

The comparison is against seed 42, the stronger parent on these tasks: 272 failures against the published seed's 322. The measure is tasks only one of the two fails, with an exact two-sided sign test.

| result | reading | what follows |
|---|---|---|
| fewer failures than seed 42, p < 0.05 | **better than both parents** | a candidate to publish, which is the owner's decision; the eight suites at int8 come first, on the owner's word |
| p >= 0.05 | **no better than seed 42** | averaging is not worth the extra step here, and seed 42 stays the alternative to the published form |
| more failures than seed 42, p < 0.05 | **worse than seed 42** | as above, and averaging these two seeds costs something |

**Stop early if it is broken.** After the first 246 tasks, the pass stops if the average has at least twice the failures of the worse parent on those tasks, which would save about 7 hours. `early_check.py` applies this, and a stopped pass reads as worse than seed 42.

**Reported, not deciding:**
- Of the 250 tasks the parents disagree on, how many the average passes. About half is what picking one parent at random would give, and more suggests it keeps the better of each.
- Of the 172 tasks both parents fail, how many it rescues, and of the 1,354 both pass, how many it breaks.
- Each failure class, area and category, and the comparison with the published seed and e3.
- From `average_stats.json`: how many tensors differ between the parents, and the cosine between each parent's change to the base.

## Limits, stated now

- **First attempts only,** at int8, with these batches' task mix. Repair is not tested.
- **One pair of seeds.** A win here says nothing certain about other seeds or about the MoE, which would need a second seed trained first, about 13 hours.
- **Seed 42 was shown stronger on these same tasks,** so its score may be slightly flattering, which makes this comparison a little harder to win.

## Result (2026-11-06)

The pass took 9 h 58 min on the scoring Mac, with no infrastructure failures or empty answers, and all 1,776 prompts match. The early check passed at 247 tasks, with 60 failures against the parents' 43 and 39, below the stop at twice the worse parent.

`average_stats.json`: the parents differ in 328 of 677 tensors, exactly the layers the LoRA trains, and the cosine between their changes to the base is 0.23. Averaging keeps the shared part and halves the rest, so the average's change is about 78% as large as either parent's.

| of 1,776 tasks, first attempt | published seed | seed 42 | **average** | e3, mxfp4 |
|---|---:|---:|---:|---:|
| **failed** | 322 (18.1%) | 272 (15.3%) | **443 (24.9%)** | 230 (13.0%) |
| coded | 105 | 101 | **209** | 51 |
| return code | 23 | 11 | **36** | 11 |
| wrong output | 194 | 160 | **198** | 168 |
| length caps | 3 | 1 | **24** | 0 |

**The reading: worse than seed 42.** The average fails alone on 258 tasks and seed 42 on 87, p=8.5e-21. Against the published seed it is 233 to 112.

- **It loses language knowledge.** Coded failures double, and `SCOPE-001`, a name that is not in scope, goes from 30 and 19 to 70. It breaks 184 of the 1,354 tasks both parents pass and rescues 36 of the 172 both fail.
- **It keeps neither parent's better half.** On the 250 tasks the parents disagree on, it passes 127, about what picking one at random would give.
- **Each seed's private habits faded, as predicted, but that is no gain.** `sorted` with too many arguments goes from 15 at seed 42 to 3, and the unclosed argument list from 12 at the published seed to 3. `Map[Int, ...]`, which both have, sits between them at 6. Division by zero, the published seed's, rises to 9.
- **Every area is worse,** JSON documents and data files most: 44% against 34% and 24%.

This fits [AdapterSoup](https://ar5iv.labs.arxiv.org/html/2302.07027)'s report that averaging adapters from different random seeds performs poorly. Two seeds of this recipe learn their Aether in directions that mostly do not line up, and halving each loses it. Averaging is not worth pursuing for these fine-tunes, and nothing follows from this batch.
