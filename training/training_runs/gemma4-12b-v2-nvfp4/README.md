# The Gemma 4 12B fine-tune at NVFP4 on the training host (set up 2026-11-03)

The owner suggested NVFP4 might be quicker on NVIDIA hardware that supports it. A Mac cannot show that, so this run tests it on
host-c, on the final step of `../gemma4-12b-v2/`.

## Setup

- **Build:** `quantize.py`, NVIDIA's ModelOpt 0.43 in the trainer image. W4A4: weights and inputs at 4 bits, the form the FP4
  kernels run, with input scales calibrated on 64 of the fine-tune's own training records. Vision, audio, the embeddings and
  `lm_head` stay at 16-bit. 8.25 GB, against 23.9 GB for the 16-bit merge.
- **Two fixes the export needed for vLLM,** both in `quantize.py`:
  - ModelOpt writes transformers' in-memory names, and 10 vision tensors differ from the checkpoint's.
  - The exclude list names `model.embed_vision*`, but vLLM matches its own prefixes, which have no `model.`, so it treated
    the vision layers as quantized.
- **Serving:** the vLLM image the 16-bit scoring used (0.25.1). It picks `FlashInferCutlassNvFp4LinearKernel` on this GPU, a native
  kernel rather than its emulation. The format is marked experimental in its log.
- **The host's other services stayed up,** idle. The model fits beside them at a memory fraction of 0.35.

## Running it

`speed.sh` measures NVFP4 and the 16-bit merge, each with and without `--enforce-eager`, with `measure_speed.py`. `score.sh` runs
the eight suites with the 16-bit scoring's flags. `recipes.sh` then builds and scores two more careful recipes the owner asked for after the
plain build lost accuracy: attention left at 16-bit (`mlp_only`), and smoothing before quantizing (`awq_lite`). Copy the reports here as `score/tuned__<suite>.json.gz`, or
`score_<recipe>/` for a recipe, with `bench/results/pack_reports.py`.

## Speed

The first eleven algorithm-suite prompts, no guide, the median of the ten answers after the first, timed from first token to last:

| | `--enforce-eager` | CUDA graph | model size in memory |
|---|---:|---:|---:|
| **NVFP4** | **23.5 tok/s** | **24.4 tok/s** | **8.29 GiB** |
| 16-bit | 7.9 tok/s | 8.0 tok/s | about 22 GiB |

NVFP4 is 3 times as fast, about the ratio of the two sizes. That suggests memory bandwidth sets the speed of this dense model here,
though this measurement does not isolate it. The eager flag makes no real difference to either. `mlp_only`, measured with the scoring
flags, generates at 9.0 tok/s, so leaving attention at 16-bit gives back nearly all of the gain.

## Scores

No guide, first attempt (after repair). All eight suites ran with no infrastructure failures or empty answers, one length cap, and
all 126 prompts byte-identical to the 16-bit run's. The host was powered off for 54 minutes during `frontier_spec`, for reasons
unrelated to this run, and the suite resumed from its checkpoint.

| | size | main (73) | held-out (53) | all (126) |
|---|---:|---|---|---|
| **NVFP4, plain recipe** | **8.25 GB** | **53 (60)** | **16 (23)** | **69 (83)** |
| **NVFP4, `mlp_only`** | **11.7 GB** | **55 (65)** | **23 (29)** | **78 (94)** |
| 16-bit (`../gemma4-12b-v2/`) | 24 GB | 67 (69) | 30 (36) | 97 (105) |
| int8 (`../gemma4-12b-v2-q8/`) | 12 GB | 65 (70) | 28 (36) | 93 (106) |
| untrained + guide, q8_0 | 12 GB | 48 (67) | 26 (41) | 74 (108) |

- **The plain recipe costs far more than int8.** Against int8, 7 tasks to 31 on first attempts (p<0.001) and 5 to 28 after repair
  (p<0.001). After repair it also falls clearly behind the guided base, 10 to 35.
- **The new failures are mostly language errors:** 34 coded on first attempts, against 14 at 16-bit, with wrong output at 21 against 8.
  An example from the smoke task is `fxlprintln`, a garbled identifier in an otherwise right program. So the noise reaches
  individual tokens.
- **No build fault found:** fused layers share scales, no scale is zero or infinite, and the K-for-V layers copy their scales too.
  Published W4A4 tests on Llama and Qwen models of 8B and up keep 94.7 to 99.8% of the 16-bit score with plain rounding
  (arXiv 2509.23202), and this keeps 71%, so it says little about NVFP4 in general.
- **`mlp_only` gives back the speed but not the scores.** It is about int8's size and loses to it, 7 tasks to 22 on first attempts
  (p=0.008) and 6 to 18 after repair (p=0.02). The plain recipe is not separable from it, 12 to 21 (p=0.16) and 13 to 24 (p=0.10), so
  quantizing the MLP layers alone costs most of the loss. Only 22 first programs match the 16-bit run's, against 17 for the plain
  recipe and 78 for int8. Its failures are 32 coded (18 of them SYN-001), 4 return code and 12 wrong output on first attempts. The
  suites ran with no infrastructure failures or empty answers, two length caps on one task, and all 126 prompts byte-identical.
- **`awq_lite` builds but cannot be served here, so it has no score.** ModelOpt writes `quant_algo: NVFP4_AWQ` and 96
  `pre_quant_scale` tensors, on `mlp.down_proj` and `self_attn.o_proj` in each layer. vLLM 0.25.1 rejects that algorithm. Relabelling
  the build as `NVFP4` might load it, but it would drop scales the weights were rescaled to go with, and so measure something else.
