# fv3-20261105: re-testing the targeted pairs on new fresh tasks

**Result, 2026-11-06: a gain.** Targeted minus baseline +3.70 points, 95% interval [+1.90, +5.46]. See "Result" below.

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
- **Does a taught idiom transfer?** How many programs in each arm use `sorted_by` or `sorted_by_desc` and how many of those pass, and how many pass `sorted` an extra argument. v2's data uses these builtins in at most 9 of 2,058 Aether instruction records, and the 298 pairs add 14 more uses (`../coverage-20261105/`).

## Limits, stated now

- **First attempts only,** at int8. Seed 42's baseline is known to be stronger on the earlier tasks, and both baseline seeds count equally in the average.
- **The rotation is not the benchmark's mix.**
- **All four runs share one Mac and one Ollama server.** The driver stops if the server's version changes between passes.

## Result (2026-11-06)

1,056 tasks were written and 1,026 survived freezing. The four passes ran on the scoring Mac at int8 on Ollama 0.40.13, on the 1.0 grader, with no infrastructure failures or empty answers, and all prompts match. On the owner's word the last pass, seed 42's targeted run, ran beside the others instead of after them, which was about 1.3 times faster. `compare.py` prints the tables.

| first attempt, 1,026 tasks | passed | failed | coded / return code / wrong output |
|---|---:|---:|---|
| baseline, seed 3407 | 838 | 188 | 49 / 9 / 130 |
| baseline, seed 42 | 863 | 163 | 66 / 7 / 90 |
| targeted, seed 3407 | 892 | 134 | 43 / 5 / 86 |
| targeted, seed 42 | 885 | 141 | 48 / 4 / 89 |

**The reading: a gain.** Targeted minus baseline is +3.70 points, with a 95% interval of [+1.90, +5.46], about 38 tasks of 1,026.

- **Both seeds point the same way.** At seed 3407 the targeted run gains 110 tasks and loses 56 (p=3e-5), and at seed 42 it gains 89 and loses 67 (p=0.09). Both targeted runs fail fewer tasks than either baseline.
- **Wrong output fell most:** 130 and 90 at baseline, 86 and 89 targeted. The plan expected this class to respond least.
- **The taught idiom transferred, a little.** `sorted_by` appears in 6 and 1 baseline programs and in 12 and 5 targeted ones. Seed 42's 14 calls of `sorted` with an extra argument fall to 2.
- **Every area improves,** core most: 94 and 75 failures at baseline, 60 and 69 targeted.

**Why the 53 held-out tasks did not show it.** This interval is about six times narrower. The held-out estimate of +5.7 points is consistent with this +3.7, and its interval of about 11 points either way could not exclude zero.

**What it does not show:**
- **That targeting is what helps.** These tasks come from the same writer and category rotation as the 1,776 behind the pairs, so they sit close to the pairs' distribution, closer than the benchmark does. More pairs of this style might help as much whether or not they target failures. That is the question the control arm in `../../PLAN-failure-targeted-batch.md` answers: v2 plus the same number of pairs from tasks the fine-tune passed.
- **Repair.** First attempts only. On the benchmark after repair the targeted runs were behind, 38.5 against 40.5 on held-out.
- **The benchmark's tasks.** This set is not their mix.

The verdict of `../../training_runs/gemma4-12b-v3/RESULTS.md` stands for its own test. What follows is the owner's decision: the control arm, and a batch filtered on two seeds.

## Repair test (set up 2026-11-07, before any of it ran)

The owner chose v3 (seed 3407) to replace the published model on these first attempts, but on the benchmark at int8 its repair is weak: 95 (102) against seed 42's 96 (114), and repair fixed 7 of its 31 failures (`../../training_runs/gemma4-12b-v3-q8/`). Before the swap is merged, each candidate re-answers its own failed tasks here with two repair rounds, as the benchmark does: v3's 134 and seed 42's baseline's 163. A task counts as passed after repair if its first attempt passed in the run above, or if the repair run passes it. `repair.py` computes the reading.

- **The reading:** tasks only one of the two passes after repair, over all 1,026, with an exact two-sided sign test. **v3 is better after repair** with more tasks in its favour at p < 0.05, **seed 42 is better after repair** with the reverse, and otherwise **no difference after repair**. Which to publish stays the owner's decision.
- **Reported:** how many re-asked first attempts reproduce the original at T=0, and each candidate's failures fixed by repair.

**Repair result (2026-11-07): no difference after repair.** v3 passes 947 of 1,026 after repair and seed 42's baseline 929. v3 alone passes 51 tasks and seed 42 alone 33, p=0.063, so the direction favours v3 but misses the bar. Repair fixed 55 of v3's 134 failures and 66 of seed 42's 163. On these tasks v3's repair is not the weakness the 126 benchmark tasks suggested (2 to 14 there).

Only 79 and 85 of the re-asked first programs matched the original run's byte for byte, at T=0, on the same Mac and model. Several models shared the Mac for these runs, and the earlier ones too, so generation is not fully reproducible from run to run here. That adds noise to single-pass comparisons, not bias.
