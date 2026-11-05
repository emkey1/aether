# fv3-20261105: re-testing the targeted pairs on new fresh tasks

`../../training_runs/gemma4-12b-v3/` found no gain from the 298 targeted pairs, but its reading rests on 53 held-out tasks, which only resolve about 11 points either way. The 1,776 tasks of `f1`, `f2` and `f3-20261103` cannot settle it, because `v3-20261104` trained on pairs made from them. This batch writes about 1,000 new tasks that neither arm has seen, and has all four existing runs answer them at int8, with no guide and no repair, on the 1.0 release binary. No training is involved.

| arm | run | seed |
|---|---|---|
| baseline | `gemma4-12b-v2` (the published int8 form) | 3407 |
| baseline | `gemma4-12b-v2-s42` | 42 |
| targeted | `gemma4-12b-v3` | 3407 |
| targeted | `gemma4-12b-v3-s42` | 42 |

Tasks and Python come through T'Ra's large-open lane as in `f3-20261103`, with the same category rotation. The dataset builder never takes `training/distill/`. Earlier batches' tasks count as duplicates, so these do not repeat the ones behind the pairs. The programs are read for pass or fail and for which sorting calls they use, and nothing here reaches a dataset.

## The reading, fixed before the tasks are written

`compare.py` computes it. The measure is the first-attempt pass rate, averaged over each arm's two seeds, targeted minus baseline, with a paired bootstrap over tasks (10,000 resamples, seed 42).

| 95% interval | reading | what follows |
|---|---|---|
| above zero | **a gain** | the owner decides on the control arm in `../../PLAN-failure-targeted-batch.md`, and on a batch filtered on two seeds |
| below zero | **a loss** | no retraining on these pairs |
| includes zero | **no readable difference** | no retraining on these pairs, and the upper end says how large a gain is ruled out |

By my estimate, about 1,000 tasks resolve about 1.6 points either way, against 11 on the 53 held-out tasks. This reading replaces nothing: the verdict in `../../training_runs/gemma4-12b-v3/RESULTS.md` stands for its own test.

**Reported, not deciding:**
- Each seed's difference, with a sign test, and the gap between the seeds in each arm.
- Failure class and area for each run.
- **Does a taught idiom transfer?** How many programs in each arm use `sorted_by` or `sorted_by_desc` and how many of those pass, and how many pass `sorted` an extra argument. v2's data uses these builtins in at most 9 of 2,278 instruction records, and the 298 pairs add 14 more uses (`../coverage-20261105/`).

## Limits, stated now

- **First attempts only,** at int8, and the published seed's weights are not chosen for this test: seed 42's baseline is known to be stronger on the earlier tasks, and both are averaged.
- **The rotation is not the benchmark's mix.**
- **All four runs share one Mac and one Ollama server.** The driver stops if the server's version changes between passes.
