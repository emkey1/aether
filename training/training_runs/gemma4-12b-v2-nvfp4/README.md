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
the eight suites with the 16-bit scoring's flags. Copy the reports here as `score/tuned__<suite>.json.gz` with
`bench/results/pack_reports.py`.

## Speed

The first eleven algorithm-suite prompts, no guide, the median of the ten answers after the first, timed from first token to last:

| | `--enforce-eager` | CUDA graph | model size in memory |
|---|---:|---:|---:|
| **NVFP4** | **23.5 tok/s** | **24.4 tok/s** | **8.29 GiB** |
| 16-bit | 7.9 tok/s | 8.0 tok/s | about 22 GiB |

NVFP4 is 3 times as fast, about the ratio of the two sizes. That suggests memory bandwidth sets the speed of this dense model here,
though this measurement does not isolate it. The eager flag makes no real difference to either.
