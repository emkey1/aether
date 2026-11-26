# Does LoRA on the late layers alone match the all-layer recipe? (Gemma 4 12B, written 2026-11-26, before any training)

Every Gemma 4 12B run so far put LoRA on the family's seven targets in all 48 decoder layers. Two measurements of
2026-11-26 say the early layers' changes do almost nothing:
- Per layer, the cosine between the changes two seeds make to the base is 0.000 in layers 0 to 15 and rises only from
  layer 16 (0.18 to 0.45 in layers 28 to 47). Runs that share a seed and most of their data still disagree in layers
  0 to 10. The early-layer changes look like each run's own noise.
- In `../../distill/fgraft-20261126/`, on 298 records held out of both v3 seeds, the base with only v3's changes to layers 20
  to 47 has loss 0.0456, against v3's 0.0430 and the base's 0.7696: about 99.6% of the drop. With only v3's changes to
  layers 0 to 19, the loss is 0.7878, no better than the base.

Removing trained layers is not the same as training only the others, and eval loss is not pass rate. This study trains
the late layers alone and scores them.

**One change.** Both arms are `gemma4-12b-v3`'s recipe on `v3-20261104` (4-bit base, rank 32, alpha 64, 3 epochs at
1e-4, no early stopping, the final step exported). The `late` arm adds `lora.layers_to_transform` = layers 20 to 47:
the same seven target names in 28 of the 48 layers, 191 of the 328 target modules. The `all` arm is the two existing
v3 runs; nothing is retrained for it.

| arm | runs | seed |
|---|---|---|
| all | `gemma4-12b-v3`, `gemma4-12b-v3-s42` (trained 2026-11-04) | 3407, 42 |
| late | `gemma4-12b-v3-late`, `gemma4-12b-v3-late-s42` | 3407, 42 |

The late runs also name the driver's divergence stop at twice the best eval loss (from the second eval). It changes
nothing unless it fires. The rule of `../../distill/c1-20261118/` applies: a run whose final eval loss is more than
twice its best has diverged, is not scored, and is retrained once under a new tag; if that diverges too, the owner
decides.

## Before the second run: a smoke check

`gemma4-12b-v3-late` trains first on the training host. The trainer stops at setup unless LoRA sits on exactly layers
20 to 47 (its log line `LoRA layer check: 191 LoRA modules on exactly the 28 requested layers`). Before
`gemma4-12b-v3-late-s42` starts, `train/run_metadata.json` must show that line's coverage and a trainable-parameter
count below `gemma4-12b-v3`'s, and the first logged losses must be finite. Otherwise nothing else starts: a layer list
that was silently ignored would make this study measure the all-layer recipe twice.

## The reading, fixed before training

`compare.py` computes it, written before any late run is scored on the pattern of `../../distill/c1-20261118/compare.py`.

- **Measure:** first-attempt pass rate on fv3's 1,026 frozen tasks (`../../distill/fv3-20261105/tasks_frozen.json`), no
  guide and no repair, graded by the 1.0 release binary, each model imported at int8 on the scoring Mac and answered as
  the c1 session answered. Each task's pass is averaged over an arm's two seeds; the difference is **late minus all**,
  with a paired bootstrap over tasks (10,000 resamples, seed 42).
- **One session.** All four runs answer in one scoring session on one Ollama version, written to
  `session_server_version.txt` beside this README before the first pass, in the order v3_late, v3, v3_late_s42, v3_s42.
  The session stops if the version changes.
- **The seed check** of `../../distill/c1-20261118/README.md` applies unchanged to every directional reading: all four
  late-run minus all-run totals point the same way, and the arm difference is larger than either end of the 95%
  interval of the seed-only contrast {v3_late, v3_s42} minus {v3_late_s42, v3}.

Write the 95% interval as [low, high], in points.

| interval | seed check | reading | what follows |
|---|---|---|---|
| low > 0 | passes | **late-only is better** | late-only is the candidate recipe for the next 12B run; the owner decides |
| low > 0 | fails | **no loss shown** (a gain not separable from seed noise) | as the next row |
| includes zero, low at or above -2.0 | | **no loss shown** at a 2-point margin | late-only may replace the all-layer recipe for its lower cost; the owner decides |
| includes zero, low below -2.0 | | **not resolved** | no change |
| high < 0 | fails | **a loss not separable from seed noise** | no change; the owner decides on more seeds |
| high < 0 | passes | **late-only is worse** | keep every layer |

The margin is 2.0 points, about 20 of 1,026 tasks. fv3's two-seed intervals were about 1.8 points either side, so two
equal arms will usually read "no loss shown", and a real loss of 2 points or more will usually not. "No loss shown" does
not mean equal: it means a loss larger than 2 points is ruled out at this resolution.

**Reported, not deciding:**
- **Seed noise.** The number of tasks on which an arm's two seeds differ: 125 for the all-layer seeds in the c1 session,
  recounted in this one. The idea behind the study is that the early layers' changes carry much of the seed-to-seed
  difference. Two seeds per arm cannot show it; a clearly smaller count would justify a three-seed follow-up.
- **Cost:** each run's training time, peak memory and trainable parameters.
- Eval loss on the 298 control pairs with `../../distill/fgraft-20261126/graft_eval.py`, and the trainer's eval history.
- The per-layer cosine between the two late seeds' changes to the base.
- Each late run against each all-layer run with a sign test, and failure classes.

**Limits.** One model, one dataset, and the 1,026 tasks are the pool on which v3 was chosen (as in c1). The cut at layer
20 comes from one eval-loss probe and was not tuned. Rank and learning rate stay the same, so a late run has about 58%
of the all-layer run's trainable parameters: this tests the same recipe on fewer layers, not the same capacity.

**Hosts and cost.** Training on the training host, one run at a time, about 3 to 4 hours each. Scoring on the scoring
Mac after the qwen35-4b-la session: four passes of about 5 hours.
