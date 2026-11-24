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
run first and report how much of it reproduces.
