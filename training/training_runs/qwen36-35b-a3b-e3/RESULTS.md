# First MoE: Qwen3.6-35B-A3B, and how long to train it (2026-10-21 to 2026-10-23)

Qwen3.6-35B-A3B (Apache-2.0; 35B parameters, ~3B active per token; 256 routed experts, 8 per token, one shared
expert; `qwen3_5_moe`) trained with `qwen38-27b-v1`'s recipe and data (`v1-20261021`). The 27B and this model differ
only in the base model and, in the later runs, the stopping rule.

## How it trains on this hardware

- **bf16 loading runs out of memory** on the 121 GB unified-memory host. The loader holds a CPU copy (52 GB) beside
  the GPU copy. So the runs load in **4-bit mode**, as the July MoE runs did.
  - bitsandbytes cannot quantize the fused expert weights (`mlp.experts.gate_up_proj` / `down_proj` are
    `nn.Parameter`s), so the 256 routed experts stay bf16 anyway. Only attention and the shared expert are 4-bit.
- **Unsloth puts LoRA on the routed experts by itself**, even with the family's dense target list ("Enabling LoRA on
  MoE parameters"). The trainer's new coverage report shows:
  - routed experts in all 40 layers, the shared expert, and the 10 full-attention layers;
  - 1.86B trainable parameters (5%).
- **The 30 linear-attention layers get no LoRA**: their projections have other names. The same holds for every
  `qwen3_5` run (27B, 4B). A candidate one-change experiment.
- **Merging a kept checkpoint** (`merge_checkpoint.sh`, the trainer's own export path) reproduced the trainer's
  export byte for byte (26 of 26 shards) on the smoke run.
- **Scoring.** The Aether suites ran on host-c with vLLM at bf16. The Python check ran on the scoring Mac, on an
  **int8** Ollama import (`ollama create -q int8`, 39 GB).
  - The harness sandboxes model-written Python with sandbox-exec (macOS only).
  - The bf16 model is too large to sit beside the Mac's other resident model.
  - Parity on the smoke model, tasks_v2_pos: int8 21 (25) against bf16 22 (26).

## Runs

| run | stopping | training |
|---|---|---|
| `qwen36-35b-a3b-v1` | eval-loss early stopping (patience 3); stopped at epoch 0.76, best checkpoint epoch 0.64 (eval loss 0.0940) | 3 h 18 min |
| `qwen36-35b-a3b-e3` | none: the full 3-epoch schedule, last step exported; adapters kept at steps 250 and 500 | 11 h 05 min |
| `qwen36-35b-a3b-e3-ep1` / `-ep2` | scoring-only twins: e3's step-250 (epoch 0.996) and step-500 (epoch 1.992) adapters, merged | — |

Grader 2026-10-21-8 (`1.0-beta4dev`). The replay of every earlier model on it moved nothing, so the 27B and 4B
columns (scored on 2026-10-20-12) compare directly.

## Results: no guide, first attempt (after repair)

| | main (73) | held-out (53) | Python (126) |
|---|---|---|---|
| untrained MoE, no guide | 0 (0) | 0 (0) | 117 (122) |
| untrained MoE + 1.0-beta2 guide | 60 (69) | 29 (38) | — |
| `qwen36-35b-a3b-v1` (early-stopped, epoch 0.76) | 62 (68) | 30 (37) | 78 (103) |
| e3 at epoch 1 | 54 (65) | 20 (34) | 52 (82) |
| e3 at epoch 2 | **68 (70)** | 33 (42) | 48 (91) |
| e3 at epoch 3 (final) | 65 (70) | **39 (47)** | 44 (82) |
| *`qwen38-27b-v1` (published)* | *71 (72)* | *37 (46)* | *67 (116)* |

The untrained-MoE rows are int8 on the scoring Mac (`../base-qwen36-35b-a3b-int8-20261022/`). For guided runs the
reports name the guide `card`; it is the 1.0-beta2 medium guide, about 13.5K tokens in every prompt.

Per suite, first attempt (after repair):

| suite | v1 (0.76) | e3 epoch 1 | e3 epoch 2 | e3 epoch 3 |
|---|---|---|---|---|
| tasks_v2_pos (35) | 34 (35) | 31 (34) | 35 (35) | 33 (35) |
| tasks_hard_v2 (14) | 9 (12) | 4 (10) | 13 (14) | 13 (14) |
| tasks_hard_nontoon (5) | 3 (4) | 2 (4) | 4 (5) | 2 (4) |
| tasks_cs (19) | 16 (17) | 17 (17) | 16 (16) | 17 (17) |
| tasks_frontier (15) | 10 (12) | 7 (11) | 8 (13) | 10 (14) |
| tasks_frontier_algo (14) | 8 (11) | 4 (8) | 9 (10) | 10 (11) |
| tasks_frontier_spec (12) | 7 (8) | 4 (6) | 8 (9) | 9 (11) |
| tasks_sealed_toon (12) | 5 (6) | 5 (9) | 8 (10) | 10 (11) |

## Reading

- **Eval loss was a misleading stopping signal for this model.**
  - On the 60-record split it reached its best at epoch 0.64. It stayed flat around 0.090-0.095 to epoch 2, then rose
    to 0.104-0.106 in the third pass (training loss 0.003).
  - The benchmark kept improving: held-out 30 at epoch 0.76, 39 at epoch 3.
  - The earlier lesson (eval loss flattens on common tokens before rare rules are learned) holds strongly here.
  - Over-fitting by loss is not over-fitting by greedy pass rate: a more confident model pays in loss for its few
    wrong tokens while its first choices improve.
- **The epoch-1 dip is a learning-rate effect, not a knowledge gap.**
  - Epoch 1 was cut at about two-thirds of the peak learning rate. Its misses are slips: a `has_toon()` guard whose
    closing brace is dropped, mangled escapes in string literals.
  - Repair recovers most of them: held-out 20 to 34.
- **Epoch 3 against epoch 2.**
  - Epoch 2 is the better first-attempt model on the main suites (68 vs 65).
  - After repair the two are equal (70 vs 70).
  - Epoch 3 is clearly better on held-out (39 vs 33; 47 vs 42 after repair).
  - Python is lost by epoch 1. The first attempts erode only slightly after that (52, 48, 44), and after repair epoch
    2 keeps the most (91).
- **Against the published 27B**, the epoch-3 MoE trails on the main suites (65 vs 71) and edges it on held-out (39 vs
  37; within noise), with about 3B active parameters per token.
- **Against its own guided base**, every checkpoint from epoch 2 on beats it with no guide on both measures.
- **The release rule fixed before scoring** ("the latest checkpoint that does not lose on the main suites") picks
  epoch 2: epoch 3 is 3 first attempts lower on main.
  - The difference is near the noise floor. Held-out, which favours epoch 3, may not choose.
  - **Nothing is released yet.** Epoch 2 was cut mid-schedule, so the next run separates "one more pass" from "a fully
    decayed learning rate" (`../qwen36-35b-a3b-e2/`, rule in its config).

## Files

- `../qwen36-35b-a3b-smoke-a/`: the 20-step smoke test (tasks_v2_pos 22 (26) after 20 steps).
- `../qwen36-35b-a3b-v1/`, this directory, `../qwen36-35b-a3b-e3-ep1/`, `../qwen36-35b-a3b-e3-ep2/`:
  - `score/tuned__*` are the Aether suites on host-c;
  - `score/python__*` are the Python check on the scoring Mac (int8).
- `../base-qwen36-35b-a3b-int8-20261022/`: the untrained model, no guide, guided and Python, int8 on the scoring Mac.
