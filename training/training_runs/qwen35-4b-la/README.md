# Does LoRA on the linear-attention layers help? (Qwen3.5-4B, written 2026-11-18, before any training)

Every `qwen3_5` run so far (the 27B, the 4B, the MoE) put LoRA on the family's dense targets, `q_proj k_proj v_proj o_proj
gate_proj up_proj down_proj`. Those names exist only in the full-attention layers and the MLPs. The gated delta-net
(linear-attention) layers name their projections `in_proj_qkv`, `in_proj_z`, `in_proj_a`, `in_proj_b` and `out_proj`, so
their attention was never adapted: 24 of 32 layers in the 4B, 30 of 40 in the MoE. The trainer's coverage report showed
this on 2026-10-22. Nobody has tested adapting them.

**One change.** Both arms train Qwen3.5-4B in bf16 on `v2-20261101` with the current recipe (3 epochs, no early stopping,
adapters kept at steps 300 and 575). The `la` arm adds the five linear-attention names to `lora.target_modules`; the
`base` arm keeps the family's list. The vision tower's projections are named `attn.qkv` and `attn.proj`, so neither list
reaches it. Two seeds per arm:

| arm | runs | seed |
|---|---|---|
| base | `qwen35-4b-la-base-s3407`, `qwen35-4b-la-base-s42` | 3407, 42 |
| la | `qwen35-4b-la-la-s3407`, `qwen35-4b-la-la-s42` | 3407, 42 |

The 4B trained in 22 minutes in `qwen35-4b-v1`; with 3 full epochs a run is about an hour, plus about an hour of scoring
on the host.

## Before the first full run: a smoke check

`qwen35-4b-la-la-s3407` runs first, and the other three wait until its `train/run_metadata.json` shows `lora_coverage`
with LoRA on the 24 linear-attention layers and a larger trainable-parameter count than a `base` run, and its first
logged losses are finite. If not, the run is stopped and nothing else starts: a target list that silently adapts
nothing would make this study measure two identical arms.

**Passed, 2026-11-25.** The run finished all three epochs before the check was read (the wait watched the wrong
directory); nothing else had started. Its adapter puts LoRA on all five linear-attention projections in each of the 24
linear-attention layers, on the 8 full-attention layers and on all 32 MLPs, and on nothing in the vision tower:
64,929,792 trainable parameters, against 42,467,328 for `qwen35-4b-v1`, the last run on the family's list (rank 32,
alpha 64 in both). Its first logged losses were 0.61, 0.37, 0.40, 0.41 and 0.32, and eval loss fell from 0.196 at step
25 to 0.104 at step 575. The other three then trained one at a time on the same host. `score.sh` runs the session
below.

## The reading, fixed before training

`compare.py` computes it.

- **Measure:** first-attempt pass rate on the 2,802 fresh tasks of `f1`, `f2`, `f3-20261103` and `fv3-20261105`, with no
  guide and no repair, graded by the 1.0 release binary. `v2-20261101` was built before any of these batches, so neither
  arm has seen them. Each task's pass is averaged over the arm's two seeds; the difference is `la` minus `base`, with a
  paired bootstrap over tasks (10,000 resamples, seed 42). The models answer at bf16 through Ollama on the scoring Mac
  (`tools/score_ollama_model.sh`'s import, then the same filter pass as `../../distill/fep-20261118/`).

| 95% interval | reading | what follows |
|---|---|---|
| above zero, larger than the gap between the two `base` seeds, and the seed check passes | **a gain** | the five names are a candidate; no target changes until the three-seed reading below (amended 2026-11-25) |
| above zero, but within the seed gap or failing the seed check | **not separable from seed noise** | no change; the owner decides on more seeds |
| includes zero | **no readable difference** | no change; the upper end says how large a gain is ruled out |
| below zero | **a loss** | no change |

**The seed check (amended 2026-11-24, owner: "Yes, make it decide").** As first written on 2026-11-18 the first two
rows had only the gap between the two `base` seeds' totals as their guard. The interval resamples tasks, not seeds,
and `../gemma4-12b-v3/RESULTS.md` found that guard weak: two seeds whose totals differed by one task differed on 15 of
53 first attempts. The old guard stays, and "a gain" now also needs both of these:
- every `la` run passes more tasks than every `base` run: all four differences are above zero;
- the difference is larger than either end of the 95% interval of the **seed-only contrast**, {la-s3407, base-s42}
  minus {la-s42, base-s3407}. That contrast has one run of each arm on each side, so it holds no `la` effect, only
  what seeds do at this resolution.

The check can only turn "a gain" into "not separable from seed noise", never the reverse. The other two rows are
unchanged: neither leads to a change. When it was added the repo held no result for these runs; the training host
was not checked.

**What a gain changes (amended 2026-11-25, owner: "yes please to your picks", before any run was trained).** As first
written, "a gain" changed the `qwen3_5` family's targets from the next run on. That conflicts with two rules on record:
`../../config/README.md` asks three seeds for a hyperparameter conclusion, and this README's own limits say a 4B gain
earns a one-run check on the larger models, not a change. On 2026-11-25 no run had started: the training hosts held no
run directory for it. So:
- **The four runs are answered in one session** on the scoring Mac, on one server version written to
  `session_server_version.txt` beside this README before the first pass, in the order la-s3407, base-s3407, la-s42,
  base-s42; the session stops if the version changes, and `compare.py` refuses to read without the file.
- **"A gain" by the table above makes the names a candidate and changes no target.**
- **The 4B's targets change only on a three-seed reading.** A third run in each arm, seed 7, is trained, and all six runs
  are answered again in one new session on one server version. Fixed now, before any run: that reading is "a gain"
  only if (1) the 95% interval of `la` minus `base`, each task's pass averaged over the arm's three seeds (paired
  bootstrap over tasks, 10,000 resamples, seed 42), is above zero; (2) all nine `la`-run minus `base`-run totals are
  above zero; and (3) the arm difference is larger than the largest gap between two seeds' totals inside either arm.
  Anything else is "not separable from seed noise" and no target changes.
- **The 27B and the MoE** then get one run each with the five names added, answered in one session with a base run of
  the same model. That run may show only "not worse than the base run by more than the 4B's largest between-seed gap";
  it cannot show a gain, and no target changes for those models on it.

This changes what follows from the first row, never what the four runs read.

**Reported, not deciding:** each seed's difference with a sign test; the gap between seeds in each arm; failure class
(coded, return code, wrong output); the benchmark's main and held-out suites at T=0 from the driver's scoring (held-out
failures stay unread); trainable parameters, training time and final eval loss for each run.

**Limits.** One model size and one dataset. A result on the 4B says nothing certain about the 27B or the MoE, where
three in four layers are linear attention too; a gain here would justify a one-run check there, not a change by itself.

## Result (2026-11-27)

One session on Ollama 0.40.17 (`session_server_version.txt`), in the order la-s3407, base-s3407, la-s42, base-s42,
from 12:01Z on 2026-11-26 to 04:06Z on 2026-11-27. Each pass answered all 2,802 tasks with no infrastructure failure,
empty answer or leaked thinking. `compare.py` reads the four packed reports beside this README and prints the same
reading.

| run | arm, seed | first attempt passed | failed: coded / return code / wrong output | stopped at the length cap |
|---|---|---|---|---|
| `qwen35-4b-la-la-s3407` | la, 3407 | 2,069 | 180 / 46 / 507 | 5 |
| `qwen35-4b-la-base-s3407` | base, 3407 | 2,077 | 180 / 46 / 499 | 5 |
| `qwen35-4b-la-la-s42` | la, 42 | 2,100 | 197 / 33 / 472 | 6 |
| `qwen35-4b-la-base-s42` | base, 42 | 2,050 | 202 / 41 / 509 | 3 |

**The reading: no readable difference.** `la` minus `base` is **+0.75 points [-0.30, +1.78]**; the two `base` seeds
differ by 0.96. What follows, from the table: no change, and a gain above 1.78 points is ruled out at this resolution.
The three-seed reading (seed 7) and the 27B and MoE runs follow only from "a gain", so none of them runs. The seed
check guards only "a gain"; for the record, the four `la`-run minus `base`-run totals are -8, +19, +23 and +50, and the
seed-only contrast is -1.03 points [-2.09, +0.00], so it would have failed on its first part.

Reported, not deciding:
- Per seed: la-s3407 minus base-s3407 -0.29 points [-1.82, +1.28], 244 gained and 252 lost (sign test p=0.753);
  la-s42 minus base-s42 +1.78 [+0.36, +3.21], 236 and 186 (p=0.017). Across seeds: la-s3407 minus base-s42 +0.68
  [-0.89, +2.21]; la-s42 minus base-s3407 +0.82 [-0.75, +2.32].
- Seed gaps, seed 3407 minus seed 42: `base` +0.96 [-0.57, +2.53], `la` -1.11 [-2.68, +0.50].
- The benchmark from the driver's scoring (no guide, T=0, seed 42, one repeat, two repair rounds, the 1.0 release
  built for Linux), first attempt (after repair); held-out failures stay unread:

  | run | main (73) | held-out (53) |
  |---|---|---|
  | `qwen35-4b-la-la-s3407` | 64 (65) | 19 (25) |
  | `qwen35-4b-la-base-s3407` | 61 (63) | 19 (19) |
  | `qwen35-4b-la-la-s42` | 63 (66) | 16 (20) |
  | `qwen35-4b-la-base-s42` | 61 (62) | 18 (21) |

- Cost, from each run's record: `base` 42,467,328 trainable parameters in 128 LoRA modules, `la` 64,929,792 in 248.
  Training time on the same training host: 6,327 and 3,827 seconds for `base` (s3407, s42), 4,000 and 4,295 for `la`;
  this record does not say why base-s3407 took longer. Final eval loss against the best (34 evals; the same seed holds
  out the same 60 records): `base` 0.1175 / 0.1022 and 0.1294 / 0.1151, `la` 0.1163 / 0.1020 and 0.1300 / 0.1133, all
  within 1.15 times the best.
