# The MoE fine-tune at 4-bit (set up 2026-11-01)

The released MoE fine-tune `qwen36-35b-a3b-e3` quantized to 4 bits, with no guide, on the eight suites, graded by the
1.0 release binary. It asks whether the fine-tune keeps its scores at a size that fits a 32 GB Mac. The guided Gemma 4
12B (`../guided-gemma4-12b-1.0-20261031/`) is the untrained alternative in that class.

## Setup

- **Model.** e3's merged bf16 export (`../qwen36-35b-a3b-e3/`), imported into Ollama (`ollama create -q <level>`,
  its MLX quantization) four ways. The sizes are the imports' manifests:

  | variant | routed experts (over 90% of the weights) | size |
  |---|---|---|
  | **nvfp4** | 4-bit float, blocks of 16 with 8-bit scales | 21.9 GB |
  | **mxfp4** | 4-bit float, blocks of 32 with power-of-two scales | 20.9 GB |
  | int8 (`q8`) | 8-bit | 39.1 GB |
  | int4 | gate and up projections 4-bit, **down projections left at 8-bit** | 27.4 GB |

  - nvfp4 and mxfp4 are the sizes a 32 GB Mac can hold. int4 would leave it about 5 GB for everything else.
  - int8 is the import the Python check used in October. e3's published Aether scores come from vLLM at bf16, so
    int8 on the same stack as the 4-bit copies separates the serving stack from the cost of 4 bits.
  - Every copy keeps the token embeddings at bf16 (1.0 GB) and the vision tower (0.9 GB), which Aether never uses.
  - Each is copied with a 32K context.
- **No guide.** The fine-tune is scored as it was released, on prompts of a few hundred tokens.
- **Grader:** the published `aether-1.0-macos-arm64` release binary, checked against the release's `SHA256SUMS`.
- **Settings:** T=0, seed 42, one repeat, two repair rounds, 8,192 output tokens, thinking off
  (`"reasoning_effort": "none"`, as in the earlier Ollama runs).

## Running it

`score.sh` fetches and checks the grader, imports a variant when Ollama lacks it, stops any other loaded model, runs
the 3-task smoke and then the eight suites, in the table's order. The smoke fails a variant if an answer is empty, begins with a
reasoning trace or costs 1,000 completion tokens or more, or if a prompt is 3,000 tokens or more (a guide got in).
Each suite resumes from its checkpoint; re-run the script to continue. The command is at the top of `score.sh`. Copy
the reports here as `score/<variant>/tuned__<suite>.json.gz` with `bench/results/pack_reports.py`.

## Results (2026-11-01)

All four copies ran all eight suites with no infrastructure failures, no empty answers and no length caps, on the 1.0
grader (b968240e…). Each took about 8 to 10 minutes. Every first-attempt prompt is byte-identical to the one in e3's
published reports (126 of 126). The reports are under `score/<variant>/`.

| first attempt (after repair) | size | main (73) | held-out (53) |
|---|---|---|---|
| `qwen36-35b-a3b-e3`, bf16 on vLLM (published; replayed on the 1.0 grader) | — | 65 (70) | 39 (47) |
| e3, int8 | 39.1 GB | 65 (70) | 36 (44) |
| e3, int4 (down projections 8-bit) | 27.4 GB | 67 (69) | 33 (45) |
| **e3, nvfp4** | 21.9 GB | **63 (68)** | **31 (39)** |
| **e3, mxfp4** | 20.9 GB | **64 (68)** | **33 (40)** |
| *untrained MoE + 1.0 guide, int8 (`../guided-base-1.0-20261031/`)* | 39 GB | *60 (71)* | *33 (43)* |
| *Gemma 4 12B + 1.0 guide, q8_0 (`../guided-gemma4-12b-1.0-20261031/`)* | 12 GB | *48 (67)* | *26 (41)* |

Task by task, with a two-sided sign test on the tasks that differ:
- **The stack.** int8 on Ollama passes exactly the published model's main tasks, first attempt and after repair. On
  held-out it loses 4 first attempts and gains 1 (p ≈ 0.38), and loses 3 after repair. The bf16 and int8 copies
  differ in both quantization and serving stack, so this run cannot split those.
- **int4 with 8-bit down projections** matches int8: 6 and 7 tasks either way on first attempts, 4 and 4 after
  repair.
- **Full 4-bit costs a few held-out tasks.** Against int8 after repair, nvfp4 loses 10 tasks and gains 3 (p ≈ 0.09),
  and mxfp4 loses 7 and gains 1 (p ≈ 0.07). Most of the losses are on held-out. First attempts churn more and do
  not separate (nvfp4 13 to 6, p ≈ 0.17; mxfp4 10 to 6, p ≈ 0.45). The two formats agree in direction; neither is
  clearly better than the other.
- **On main the fine-tune survives 4 bits.** mxfp4 and nvfp4 are within 2 of int8 on first attempts and after repair.
- **Against the guided models.** At mxfp4 the fine-tune keeps its first-attempt lead on main over the untrained MoE
  with the guide (64 vs 60), ties it on held-out first attempts (33 vs 33), and trails it by 3 on both after repair.
  The guided MoE here is int8 and carries a ~14K-token guide in every prompt; a 4-bit guided copy is not measured.
  Against the guided Gemma 4 12B, the mxfp4 fine-tune leads first attempts by 16 on main and 7 on held-out, and is
  within 1 after repair.
- **For a 32 GB Mac,** mxfp4 at 20.9 GB is the candidate: it scores like nvfp4 in a gigabyte less. Dropping the unused
  vision tower would save 0.9 GB more. On a Mac with less memory, macOS caps how much the GPU may wire down, so a
  21 GB model may need that limit raised; this run on a larger Mac did not test it.
