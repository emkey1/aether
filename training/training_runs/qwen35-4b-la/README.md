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

## The reading, fixed before training

`compare.py` computes it.

- **Measure:** first-attempt pass rate on the 2,802 fresh tasks of `f1`, `f2`, `f3-20261103` and `fv3-20261105`, with no
  guide and no repair, graded by the 1.0 release binary. `v2-20261101` was built before any of these batches, so neither
  arm has seen them. Each task's pass is averaged over the arm's two seeds; the difference is `la` minus `base`, with a
  paired bootstrap over tasks (10,000 resamples, seed 42). The models answer at bf16 through Ollama on the scoring Mac
  (`tools/score_ollama_model.sh`'s import, then the same filter pass as `../../distill/fep-20261118/`).

| 95% interval | reading | what follows |
|---|---|---|
| above zero, and larger than the gap between the two `base` seeds | **a gain** | the `qwen3_5` family's targets add the five names, from the next run on |
| above zero, but within the seed gap | **not separable from seed noise** | no change; the owner decides on more seeds |
| includes zero | **no readable difference** | no change; the upper end says how large a gain is ruled out |
| below zero | **a loss** | no change |

**Note added 2026-11-24 (review of 2026-11-24).** The interval resamples tasks, not seeds, and the guard in the first
two rows compares it with the gap between the two `base` seeds' totals. `../gemma4-12b-v3/RESULTS.md` found that guard
weak: two seeds whose totals differed by one task differed on 15 of 53 first attempts. `compare.py` therefore also
prints, as reported and not deciding, the seed-only contrast {la-s3407, base-s42} minus {la-s42, base-s3407}, which
holds no `la` effect, and all four `la`-run minus `base`-run differences. The table above is unchanged; whether "a
gain" must also clear those is the owner's call.

**Reported, not deciding:** each seed's difference with a sign test; the gap between seeds in each arm; failure class
(coded, return code, wrong output); the benchmark's main and held-out suites at T=0 from the driver's scoring (held-out
failures stay unread); trainable parameters, training time and final eval loss for each run.

**Limits.** One model size and one dataset. A result on the 4B says nothing certain about the 27B or the MoE, where
three in four layers are linear attention too; a gain here would justify a one-run check there, not a change by itself.
