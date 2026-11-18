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
