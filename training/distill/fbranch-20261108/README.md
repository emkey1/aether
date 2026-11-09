# fbranch-20261108: two late branches of v3, averaged, read on 1,026 fresh tasks

Averaging and TIES-merging the two seeds of `gemma4-12b-v2` both failed (`../favg-20261105/`, `../fties-20261106/`): runs that diverge from the first step learn changes that barely line up (cosine 0.23). Runs that share most of their training and split late usually stay compatible. This tests that, on the owner's word, after `../fpolish-20261108/`.

- **Trunk:** the published `gemma4-12b-v3` merged weights.
- **Branches:** `gemma4-12b-v3-branch-s3407` and `gemma4-12b-v3-branch-s42`, each a fresh LoRA trained for one more epoch of `v3-20261104` at lr 5e-5, differing only in seed.
- **Merge:** `average_weights.py`, the mean of the two branches, with the trunk as base for the cosine between their changes.
- **Read:** all three at int8 on the 1,026 fresh tasks of `../fv3-20261105/`, no guide, no repair. None of them trained on those tasks.

## The reading, fixed before training

The average against the better branch (fewer failures): tasks only one of the two fails on first attempt, exact two-sided sign test.

| result | reading |
|---|---|
| fewer failures than the better branch, p < 0.05 | **late-branch averaging helps**; publishing anything is the owner's decision |
| p >= 0.05 | **no better than a branch** |
| more failures than the better branch, p < 0.05 | **worse than a branch** |

**Reported, not deciding:** each branch against `gemma4-12b-v3` itself (does an extra epoch help on its own), the average against v3, the cosine between the branches' changes, and failure classes.

## Limits, stated now

- **One branch length and learning rate,** fixed now.
- **First attempts only,** at int8, on one pair of branches.

## Result (2026-11-09)

Both branches trained in about 80 minutes each on the training host; the three passes ran side by side on the scoring Mac with no infrastructure failures, and every smoke passed.

| 1,026 fresh tasks, first attempt | failed | coded / return code / wrong output |
|---|---:|---|
| v3 (the trunk, published) | 134 | 43 / 5 / 86 |
| branch, seed 3407 | 132 | 44 / 6 / 82 |
| branch, seed 42 | 140 | 48 / 4 / 88 |
| **average of the branches** | **148** | **45 / 5 / 98** |

**The reading: no better than a branch.** Against the better branch, seed 3407, the average fails alone 45 tasks and the branch 29, p=0.08, so the direction is against averaging.

- **An extra epoch did nothing measurable:** 45 to 47 against v3 for seed 3407 (p=0.92), 64 to 58 for seed 42 (p=0.65).
- **This was not the late branch it meant to be.** Each branch trained a fresh LoRA, and a fresh LoRA starts from random matrices set by its seed, so the two learned in unrelated directions from their first step: the cosine between their changes to the trunk is 0.002 (`average_stats.json`), against 0.23 between the two full seeds. A true late branch resumes one adapter from a shared checkpoint and changes only the data order. That was not tested.

Nothing follows for publishing.
