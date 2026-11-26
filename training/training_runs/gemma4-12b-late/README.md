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

**The session moves to the training host (amended 2026-11-26, owner: "Yes, move it", before any late run was
scored).** As first written, the four passes ran on the scoring Mac after the qwen35-4b-la session. That session holds
the Mac until about 07:00Z on 2026-11-27, the Mac runs one Ollama job at a time, and in the c1 session a 12B pass took
4.3 to 6.2 hours there, about 20 seconds a task. The training host is free once training ends, and it can answer many
requests at once. So:
- All four runs, the two all-layer v3 runs included, answer in one session on the training host. Each is served at
  bf16 from its merged export by one vLLM image pinned by id (`30a38a1d74a1`, the image the driver's host scoring
  uses), with the driver's serving flags (32,768-token context, eager mode), 16 requests at a time in every pass, and
  graded by the 1.0 release built for Linux (sha256 `92341510…`, language 2026-10-30-1). The order stays v3_late, v3,
  v3_late_s42, v3_s42. `score_host.sh` runs it. It writes the image, the grader and the worker count to
  `session_server_version.txt` before the first pass and stops if any of them changes. The request timeout is 3,600
  seconds (900 on the Mac), because a single request is slower there; it changes no answer.
- The measure, the table, the margin, the seed check and the divergence rule are unchanged, and `compare.py` reads
  the same four reports. `score.sh`, the Mac session, stays as written and is not run.
- What is lost: the pass counts are bf16 under vLLM with batched requests, not int8 under Ollama, so they do not
  compare in absolute terms with fv3's or c1's (886 and 879 for the all-layer runs in the c1 session). The all-layer
  runs' reproduction of their c1-session answers now crosses server, precision and batching, and `compare.py` reports
  it as that. At temperature 0, batched requests need not give the same answer twice; all four runs answer under the
  same condition.
- Before this amendment, a throughput test: `gemma4-12b-v3` (not a late run) on 128 seeded fv3 tasks, 16 at a time,
  its answers not read for the reading, took 3.43 seconds a task (439 seconds for 128, about 109 completion tokens
  a second in all, no empty or leaked answer, none stopped at the token limit): about six times the Mac's rate.
- The cost item of "Reported, not deciding" gains what training measured: `gemma4-12b-v3-late` trained in 12,830
  seconds and `gemma4-12b-c1-r2` (all layers, the same host, the same 978 steps) in 12,800. The late recipe saves
  trainable parameters (76.5 million against 131.1 million), not training time; that is all the table's "for its
  lower cost" can mean.

**Both sessions run; the Mac decides (amended again 2026-11-26, owner: "Go ahead and run the Mac tests later, I'm
just interested in earlier preliminary results").** This was written while the training host's first pass (v3_late)
was running, before any late answer had been read. So:
- The deciding session is the Mac session as first written: `score.sh`, after the qwen35-4b-la session, int8 on one
  Ollama version, reports `report__filter_<tag>.json.gz` and `session_server_version.txt`. The table and everything
  above it apply to it unchanged.
- The training host's session of the amendment above (`score_host.sh`) runs as a preliminary look, reported, not
  deciding. Its reports are kept as `report__host_<tag>.json.gz` with `session_host.txt`; `python3 compare.py --host`
  reads them and labels its reading preliminary. Whatever it shows changes nothing in the Mac session.
- The throughput test and the training-cost note above stand.

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
Mac after the qwen35-4b-la session: four passes of about 5 hours. (Amended twice on 2026-11-26, above: this Mac
session decides, and the training host runs a preliminary one first.)
