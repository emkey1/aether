# fties-20261106: TIES-merging two seeds' weights, read on 1,776 fresh tasks

`../favg-20261105/` averaged the two seeds of `gemma4-12b-v2` and lost badly: their changes to the base have a cosine of only 0.23, so averaging halved most of what each had learned. [TIES-merging](https://arxiv.org/abs/2306.01708) (Yadav et al., 2023) removes that specific failure: it keeps each parameter's change at full size instead of halving it. This batch tests it with the same tasks, the same int8 build and the same reading. No training is involved. It runs after the seed 42 swap and `../fv3-20261105/`, on the owner's word.

`ties_merge.py` builds the export, with settings fixed now and never tuned on these tasks:
- **Trim:** for each tensor, keep each parent's 20% largest changes from the base, by magnitude, and zero the rest.
- **Elect:** each parameter's sign is the sign of the sum of the two trimmed changes.
- **Merge:** the mean of the trimmed changes that agree with that sign, added back to the base at full scale.

Only the 328 tensors the LoRA trains differ. Everything else is copied from the base, which equals both parents there.

## The reading, fixed before the merge is built

The same as `../favg-20261105/`. The comparison is against seed 42: tasks only one of the two fails, with an exact two-sided sign test.

| result | reading |
|---|---|
| fewer failures than seed 42, p < 0.05 | **better than both parents**; publishing anything is the owner's decision |
| p >= 0.05 | **no better than seed 42** |
| more failures than seed 42, p < 0.05 | **worse than seed 42** |

It stops early after 246 tasks if it has at least twice the worse parent's failures. Reported: the same tables as `../favg-20261105/`, and the average's result beside them.

## Limits, stated now

- **First attempts only,** at int8, on one pair of seeds.
- **These are fixed TIES defaults,** not a search. A miss here does not rule out other settings, and searching them on these tasks would need a separate task set to confirm the winner.
