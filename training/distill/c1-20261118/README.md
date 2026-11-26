# c1-20261118: the control arm, pairs from tasks the fine-tune passed (written 2026-11-18, before any solve)

`../fv3-20261105/` found that `gemma4-12b-v3` (v2-20261101 plus 298 pairs solved from tasks 8-bit `gemma4-12b-v2`
**failed**) beats `gemma4-12b-v2` by +3.70 points [+1.90, +5.46] on 1,026 fresh tasks. That shows more data of this kind
helps. It does not show that choosing the model's failures is what helps: 298 pairs from any fresh tasks might do the
same. `../../PLAN-failure-targeted-batch.md` called for this control; the owner asked for it on 2026-11-18.

**The control dataset, `v3c-20261118`:** `v2-20261101` plus 298 pairs from tasks the same 8-bit model **passed**, solved
exactly as the targeted pairs were.
- Tasks: `choose_tasks.py` draws 340 of the 1,454 passed tasks of `f1`, `f2` and `f3-20261103` (seeded), so the tasks come from
  the same writing runs, category rotation and frozen Python outputs as the targeted ones.
- Solvers: the guided untrained Qwen3.8-27B, then MiMo with the guide on the 27B's misses, each with two repairs; the 1.0
  medium guide (sha256 `b693a67b`) and the 1.0 release grader, as in `f3-20261103` (`run.sh`).
- Pairs: harvest keeps agreeing programs, `check_pairs` gates them, and a seeded choice keeps exactly 298.
- The dataset is built like `v3-20261104`, with these 298 in place of the targeted 298; nothing else changes.

**Runs:** `gemma4-12b-c1` (seed 3407) and `gemma4-12b-c1-s42` (seed 42), each `gemma4-12b-v3`'s config with only the
dataset changed. Each is answered on the 1,026 tasks of `fv3-20261105` at int8 on the scoring Mac, with no guide and no
repair, exactly as the four existing runs were.

## The reading, fixed before the pairs are solved

`compare.py` computes it, with `../fv3-20261105/`'s four reports.

- **Measure:** first-attempt pass rate on fv3's 1,026 tasks, each task's pass averaged over an arm's two seeds;
  **targeted minus control**, paired bootstrap over tasks (10,000 resamples, seed 42).

| 95% interval | reading | what follows |
|---|---|---|
| above zero, and the seed check passes | **targeting helps** beyond adding the same amount of data | the next batches keep filtering on a fine-tune's failures |
| includes zero | **not shown**: at this resolution the fv3 gain is explained by more data | filtering is optional; the owner decides whether the cheaper unfiltered batches replace it |
| below zero, and the seed check passes | **passed-task data helps more** | stop filtering on failures |
| above or below zero, but the seed check fails | **not separable from seed noise** | no change to filtering; the owner decides on more seeds |

**The seed check (amended 2026-11-24, owner: "Yes, make it decide").** As first written on 2026-11-18 the table had
only its interval: above zero, includes zero, below zero. The interval resamples tasks, not seeds, so a difference
in either direction now also needs both of these, in that direction:
- every targeted run against every control run, by total passes: all four differences point the same way;
- the arm difference is larger in size than either end of the 95% interval of the **seed-only contrast**,
  {targeted seed 3407, control seed 42} minus {targeted seed 42, control seed 3407}. That contrast has one run of each
  arm on each side, so it holds no targeting effect, only what seeds do at this resolution.

The check can only turn a directional reading into "not separable from seed noise", never the reverse. When it was
added, the control pairs were solved and no control run had been scored on the scoring Mac; the repo held no result.

**One scoring session for both arms (amended 2026-11-25, owner: "yes please to your picks", before any control run
was trained or scored).** As first written, the targeted arm was read from fv3's reports of 2026-11-06, answered on
Ollama 0.40.13; on 2026-11-25 the scoring Mac ran 0.40.17. A server change between the arms would line up exactly
with the arm, and the seed check cannot see it. So:
- The two targeted runs (`gemma4-12b-v3` and `gemma4-12b-v3-s42`, int8, as fv3 scored them) are answered again on the
  same 1,026 tasks, with the same grader and settings, in one session with the two control runs. The four passes run
  in the order v3, c1, v3_s42, c1_s42, so slow drift does not line up with an arm.
- The session runs on one server version, written to `session_server_version.txt` beside this README before the first
  pass; it stops if the version changes. `compare.py` refuses to read without that file.
- The deciding targeted reports are `report__filter_v3_c1s.json.gz` and `report__filter_v3_s42_c1s.json.gz` beside
  this README. fv3's 2026-11-06 targeted reports become reported, not deciding: `compare.py` prints how many first
  attempts of each re-answered run reproduce them (same first program, same pass or fail).
- The baseline (v2) reports stay fv3's; "control minus baseline" and "targeted minus baseline" were never deciding,
  and are now also across server versions.
- Seeds stay two a side (owner, same day). The table and the seed check above are unchanged; the seed check now uses
  the re-answered targeted runs.

This adds two passes on the scoring Mac (four in all) and changes no threshold. `score.sh` runs the session. Before
it was armed, `gemma4-12b-v3` was re-imported at int8 on Ollama 0.40.17 under a scratch name: all 677 tensor digests
and the config matched fv3's model, so the targeted arm keeps fv3's int8 models and the import does not line up with
the arm either.

**A diverged control run, and the pass order (amended 2026-11-25, owner: "Retrain, swap order", before any control
run was scored).** `gemma4-12b-c1` (seed 3407) diverged at step 54 of 978. Its training loss went from 0.22 to 10.3
(gradient norm 830) and never recovered. Eval loss was 0.14 at step 50, 6.59 at step 75 and 4.13 at the end. The
trainer keeps the final model (`load_best_model_at_end` is false), so its export is the diverged model. Every other
12B run of this recipe ends within 1.2 times its best eval loss: v2 1.09, v2_s42 1.07, v3 1.16, v3_s42 1.11, and c1_s42
1.14 (best 0.0874, last 0.0993). So:
- **The rule, fixed now:** a run whose final eval loss is more than twice its best has diverged. Its export is not
  scored, and it is retrained once with the same config under a new tag. If the retrain diverges too, the arm has no
  run at that seed, and nothing is read until the owner decides.
- **`gemma4-12b-c1-r2`** is that retrain: `gemma4-12b-c1`'s config with only the tag changed
  (`training/config/runs/gemma4-12b-c1-r2.json`). It is the control arm's seed-3407 run, tag `c1` in `compare.py`.
  `score.sh` applies the rule to its eval history, written beside the export when training ends, before importing it.
- **The pass order becomes v3, c1_s42, v3_s42, c1.** The session was stopped after the v3 pass, which is kept, and
  before the diverged model answered any task. The retrain takes about two hours. The arms still alternate, so slow
  drift still does not line up with an arm.

The table, the seed check and the one-session rule are unchanged.

## Result (2026-11-26)

One session on Ollama 0.40.17 (`session_server_version.txt`), in the order v3, c1_s42, v3_s42, c1. The c1 pass is
`gemma4-12b-c1-r2`, whose final eval loss was 1.11 times its best (40 evals, best 0.0848, last 0.0938), so the rule
above let it in. Each pass answered all 1,026 tasks with no infrastructure failure, empty answer or leaked thinking.
`compare.py` reads the four packed reports beside this README and prints the same reading.

| run | arm, seed | first attempt passed | failed: coded / return code / wrong output |
|---|---|---|---|
| `v3_c1s` | targeted, 3407 | 886 | 45 / 7 / 88 |
| `v3_s42_c1s` | targeted, 42 | 879 | 58 / 3 / 86 |
| `c1` (`gemma4-12b-c1-r2`) | control, 3407 | 866 | 42 / 13 / 105 |
| `c1_s42` | control, 42 | 870 | 57 / 2 / 97 |

**The reading: not shown.** Targeted minus control is **+1.41 points [-0.24, +3.07]**. At this resolution the fv3 gain is
explained by more data, and a targeting effect above 3.07 points is ruled out. What follows, from the table: filtering
on a fine-tune's failures is optional, and the owner decides whether the cheaper unfiltered batches replace it. The seed
check guards only directional readings; for the record, all four targeted-run minus control-run totals are positive
(+20, +16, +13, +9) and the seed-only contrast is +0.54 points [-1.02, +2.10], so it would have failed on its second part.

Reported, not deciding:
- Control minus baseline +1.71 points [+0.10, +3.31]; targeted minus baseline +3.12 [+1.32, +4.87]. Both compare with
  fv3's baseline reports of 2026-11-06 (Ollama 0.40.13), so they cross server versions.
- Per seed: v3 minus c1 +1.95 [-0.39, +4.19], 83 gained and 63 lost (sign test p=0.116); v3_s42 minus c1_s42 +0.88
  [-1.36, +3.12], 71 and 62 (p=0.49).
- Seed gaps: baseline -2.44 [-4.87, +0.00], targeted +0.68 [-1.46, +2.83], control -0.39 [-2.73, +1.95].
- The targeted runs re-answered here against their answers of 2026-11-06: v3 gave the same first program on 904 of
  1,026 tasks and the same pass or fail on 1,014; v3_s42 on 768 and 990. Each passes 6 fewer now (886 against 892, 879
  against 885).

**Reported, not deciding:** control minus baseline (v2) with its interval; each seed's differences with a sign test; the
seed gap in each arm; failure class; how many passed-sample tasks the solvers solved, against the targeted batch's 298
of 322.

**Why the seed check (review of 2026-11-24).** The interval above resamples tasks,
not seeds, so it describes these four sets of weights, not the recipe. On fv3's own reports seed noise alone reaches
the decision boundary: the two baseline seeds differ by +2.44 points [+0.00, +4.87], and a contrast that holds no
recipe difference at all, {v3, v2_s42} minus {v3_s42, v2}, is +1.56 [-0.10, +3.17]. `compare.py` prints that seed-only contrast for these arms and all four targeted-run minus control-run
differences. The same day the owner made them part of the reading: the seed check above.

**Limits.** The control's tasks were easier for the fine-tune, so a solver may solve them more cleanly; the report
gives each arm's solver mix and first-attempt share. One model (Gemma 4 12B) and one pool. The targeted and baseline
reports are the ones of 2026-11-06 (Ollama 0.40.13), and `run.sh` pins no server version: the control runs answer on
whatever the scoring Mac has then. `../fv3-20261105/` found that only 79 of 134 and 85 of 163 re-asked programs came
back the same at T=0, so record the server version with the control reports, and if it differs, re-answer one targeted
run first and report how much of it reproduces. (Superseded on 2026-11-25 by the one-session rule above: both
targeted runs are re-answered beside the control runs.) The 1,026 tasks are also the pool on which the targeted recipe
read +3.70 and on which the published 12B was chosen, so "targeting helps" here would say the effect is not seed noise
or volume, not how large it is (added 2026-11-25).
