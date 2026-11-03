# gemma4-12b-v2: Gemma 4 12B fine-tuned for 32 GB machines (started 2026-11-02)

Gemma 4 12B fine-tuned with the MoE's recipe on the MoE's 1.0 data. It asks whether a 12B model, which fits a 32 GB
machine with room to spare at 8-bit (about 12 GB), can match the 35B MoE squeezed into 20.9 GB. The config is
`training/config/runs/gemma4-12b-v2.json`.

- **Base:** `google/gemma-4-12B-it` at revision `43c9d014`, Apache 2.0, checked against the hub's SHA-256. 11.96B
  parameters, BF16, trained at 4-bit and served as the 16-bit merge.
- **Dataset:** `v2-20261101`, language 2026-10-30-1, the same data as `../qwen36-35b-a3b-v2/`.
- **Recipe:** 3 epochs, no early stopping, the last step exported, LoRA r=32 and alpha=64, lr 1e-4. Steps 300 and 575 are
  kept for epoch twins, because `../qwen36-35b-a3b-e3-t02/` found no measured basis for 3 epochs over 2.
- **Grader:** the 1.0 release source, the same one as v2.
- **Bars,** first attempt (after repair), main / held-out:
  - guided Gemma 4 12B at q8_0: 48 (67) / 26 (41);
  - e3 at mxfp4 (20.9 GB), the current 32 GB candidate: 64 (68) / 33 (40);
  - e3: 65 (70) / 39 (47).

## What this model needed

`training/config/families.json` (family `gemma4_unified`) has the evidence.

- **A new trainer image,** `training/config/images/Dockerfile.gemma4`: the Qwen image's transformers has no Gemma 4.
- **`FastModel`,** Unsloth's multimodal loader, which returns a processor. Training uses its text tokenizer.
- **Training text rendered as served.** With thinking off, the generation prompt ends in an empty thought channel that a
  finished turn lacks. The trainer checks the first row against the served prompt, token for token, before the first step.
- **The `<bos>` stays.** The template emits it and nothing else adds one, so Unsloth's recipe of stripping it would leave
  none here.
- **The base's non-weight files in the merged export.** The merge changes only the seven targeted weight types, but the
  trainer's transformers rewrites `config.json` in a form the serving image cannot read.

`../gemma4-12b-smoke/` proved this path with 5 steps. It is not a result.

## Not yet known

- Whether the merged export converts to a Mac format. Nothing has been tried yet for this architecture.
- The vision and audio parts come along untouched, as they do in the base. About 50M parameters, so not worth stripping.

## Log

- 2026-11-02 12:24Z: training started on host-c, with scoring armed to follow (`score_when_ready.sh`), then the step-575
  twin (`../gemma4-12b-v2-ep2/`). The host's image and video generation services and its resident llama-server stay
  stopped until both are scored.
- 2026-11-02 21:14Z: both scored, `RESULTS.md`. Final 67 (69) / 30 (36), step 575 64 (68) / 27 (31). The host's services
  are running again.
- 2026-11-03: the final step at 8 bits scored 65 (70) / 28 (36) at 12 GB, `../gemma4-12b-v2-q8/RESULTS.md`.
