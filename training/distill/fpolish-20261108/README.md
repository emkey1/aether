# fpolish-20261108: one training pass on top of the TIES merge, read on 1,026 fresh tasks

The TIES merge of `gemma4-12b-v2`'s two seeds failed 457 of 1,776 fresh tasks, worse than either parent (`../fties-20261106/`). The owner asked whether a refining pass could help. Run `gemma4-12b-v2-ties-polish` trains a fresh LoRA on the merged weights for one epoch of `v3-20261104`, at half the usual learning rate (5e-5), seed 3407, with everything else as `gemma4-12b-v3`. It is merged, built at int8 like the others, and answers the 1,026 fresh tasks of `../fv3-20261105/` with no guide and no repair. Neither it nor v3 trained on those tasks.

## The reading, fixed before training

The comparison is against `gemma4-12b-v3` (seed 3407) at int8, the published model, trained on the same data from the untouched base: tasks only one of the two fails on first attempt, exact two-sided sign test.

| result | reading |
|---|---|
| fewer failures than v3, p < 0.05 | **better than a plain run**: training from the merge helps, and publishing anything is the owner's decision |
| p >= 0.05 | **no better than a plain run** |
| more failures than v3, p < 0.05 | **worse than a plain run** |

**Reported, not deciding:** the same against both v2 seeds and v3's second seed, each failure class, and how much of the merge's damage one pass repaired.

## Limits, stated now

- **One epoch at one learning rate,** chosen now, not tuned. A miss does not rule out other settings.
- **The data differs from the merge's parents:** they trained on `v2-20261101`, this pass on `v3-20261104`, which already gains on these tasks. The comparison with v3 holds the data fixed.
- **First attempts only,** at int8.

## Result (2026-11-08)

Training took about 80 minutes on the training host; the pass on the scoring Mac had no infrastructure failures, the prompt matched, and the smoke passed. `compare.py` prints the tables.

| 1,026 fresh tasks, first attempt | failed | coded / return code / wrong output |
|---|---:|---|
| v3, seed 3407 (published) | 134 | 43 / 5 / 86 |
| v3, seed 42 | 141 | 48 / 4 / 89 |
| v2, seed 42 | 163 | 66 / 7 / 90 |
| **polished TIES merge** | **179** | **56 / 13 / 110** |
| v2, seed 3407 | 188 | 49 / 9 / 130 |

**The reading: worse than a plain run.** The polish fails alone on 108 tasks and v3 on 63, p=0.0007.

- **The pass repaired most of the merge's damage,** back to the level of the merge's v2 parents: 75 to 84 against v2 seed 3407 (p=0.53) and 77 to 61 against seed 42 (p=0.2). The TIES merge itself failed 26% of the earlier fresh tasks; this fails 17%.
- **It did not reach a plain run on the same data.** Starting from the merge instead of the base cost about 45 tasks against v3.

Nothing follows for publishing.
