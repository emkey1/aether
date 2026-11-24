# fep-20261118: two epochs or three for the MoE, on fresh tasks

`qwen36-35b-a3b-e3` trained for three epochs because its third pass looked like a held-out gain of six tasks. Two later checks did not support that
(`../../training_runs/qwen36-35b-a3b-e3-t02/`, `../../training_runs/qwen36-35b-a3b-v2/`), but both read 53 held-out tasks, which resolve about 9 points
either way, and those suites had already helped choose e3. A third pass costs about four hours of an eleven-hour run. This batch asks the question
where it can be answered: on 2,802 fresh tasks that no MoE run trained on and that chose nothing about these models.

## What runs

Three existing models answer the frozen tasks of `f1`, `f2`, `f3-20261103` and `fv3-20261105` (100 + 146 + 1,530 + 1,026) with no guide and no repair,
at int8 on the scoring Mac, T=0, seed 42, graded by the 1.0 release binary. Nothing is trained, solved or harvested, and nothing here reaches a dataset.

| arm | run | what it is |
|---|---|---|
| three epochs | `qwen36-35b-a3b-e3` | the published model |
| two epochs | `qwen36-35b-a3b-e2` | the same data, recipe and seed on a schedule that ends at epoch 2 |
| reported only | `qwen36-35b-a3b-e3-ep2` | e3's own adapter at epoch 2, cut mid-schedule |

All three trained on `v1-20261021`, which is older than every task here. `run.sh` is the driver, and it stops if the Ollama server's version changes
between passes.

## The reading, fixed before any pass runs

`compare.py` computes it. The measure is the first-attempt pass rate over all 2,802 tasks, three epochs minus two epochs (`e3` minus `e2`), with a
paired bootstrap over tasks (10,000 resamples, seed 42).

| 95% interval | reading | what follows for the next MoE recipe |
|---|---|---|
| above zero | **the third pass helps** | three epochs stays |
| below zero | **the third pass hurts** | two epochs |
| includes zero | **no readable difference** | two epochs, since the third pass then buys nothing measurable for a third more training time; the upper end says how large a gain is ruled out |

By the fv3 estimate (1,026 tasks resolve about 1.6 points), 2,802 tasks should resolve about 1 point either way.

**Reported, not deciding:** the same difference on each pool (the 1,776 earlier tasks and fv3's 1,026), e3 against its own epoch 2, each pair's
gained and lost tasks with a sign test, and failures by class.

## Limits, stated now

- **One training run per arm.** Seed noise on this model is unmeasured, so this reads these three sets of weights, not the recipe in general. A
  second seed is the test that would.
- **e3 already answered 1,776 of these tasks** at mxfp4 (`../fe3-20261104/`). That pass was read for overlap with Gemma's failures and chose nothing
  between epochs.
- **int8, not bf16,** and first attempts only. On the benchmark e3 scored 65 (70) / 36 (44) at int8 against 65 (70) / 39 (47) at bf16.
- **The tasks are the distillation writer's mix,** not the benchmark's: no sealed TOON or memory-stream tasks, and few long specifications.
- **This does not move the release.** e3 stays published whatever this says.

## Result (2026-11-18)

All three passes are complete: 2,802 cases each, no empty answers, no reasoning leaks, no infrastructure failures, Ollama 0.40.15 throughout.
The reports are `report__filter_{e3,e2,e3_ep2}.json.gz`; `python3 training/distill/fep-20261118/compare.py` reproduces everything below from them.

| first attempt, 2,802 tasks | passed | failed | does not compile / return code / wrong output |
|---|---|---|---|
| three epochs (e3) | 2,485 (88.7%) | 317 | 88 / 10 / 219 |
| two epochs (e2) | 2,477 (88.4%) | 325 | 76 / 18 / 231 |
| e3 at epoch 2 (mid-schedule) | 2,445 (87.3%) | 357 | 97 / 19 / 241 |

**The reading: no readable difference.** e3 minus e2 is +0.29 points [-0.79, +1.36] (126 gained, 118 lost, sign test p=0.65). Both pools agree:
+0.34 [-1.01, +1.69] on f1 to f3 and +0.19 [-1.66, +2.05] on fv3. By the rule fixed above, **the next MoE recipe trains two epochs**. A gain from
the third pass larger than 1.4 points is ruled out.

Reported, not deciding:
- **Both finished schedules sit above e3's own mid-schedule epoch-2 adapter.** e3 by +1.43 [+0.46, +2.43] (p=0.005). e2 by +1.14 [+0.04, +2.28]:
  143 gained, 111 lost, sign test p=0.052, and the lower bound is +0.00 with other bootstrap seeds, so e2's side is borderline, not shown. A
  schedule that runs to its end is one explanation; with one run per arm it is not established as the cause. What does hold: the earlier
  conclusion from 53 held-out tasks (`../../training_runs/qwen36-35b-a3b-e2/`: "the decayed learning rate did not bring the gain, the third pass
  did") is not supported, since on fresh tasks the two-epoch schedule lands where three epochs do. (Wording corrected 2026-11-24: it read "the
  schedule's end matters more than the number of passes" and gave e2's p as 0.05.)
- e3's six-task held-out lead over e2, which decided the release, is not visible on fresh tasks. The release stands, as stated in the limits.
- Seed noise is still unmeasured (one run per arm).
