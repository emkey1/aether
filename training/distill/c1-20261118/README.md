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
| above zero | **targeting helps** beyond adding the same amount of data | the next batches keep filtering on a fine-tune's failures |
| includes zero | **not shown**: at this resolution the fv3 gain is explained by more data | filtering is optional; the owner decides whether the cheaper unfiltered batches replace it |
| below zero | **passed-task data helps more** | stop filtering on failures |

**Reported, not deciding:** control minus baseline (v2) with its interval; each seed's differences with a sign test; the
seed gap in each arm; failure class; how many passed-sample tasks the solvers solved, against the targeted batch's 298
of 322.

**Limits.** The control's tasks were easier for the fine-tune, so a solver may solve them more cleanly; the report
gives each arm's solver mix and first-attempt share. One model (Gemma 4 12B) and one pool.
