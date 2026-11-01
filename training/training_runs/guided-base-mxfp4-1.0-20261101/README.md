# The guided MoE base at mxfp4 (set up 2026-11-01)

The untrained Qwen3.6-35B-A3B quantized to mxfp4, with the 1.0 medium guide in every prompt, on the eight suites, graded
by the 1.0 release binary. `../e3-int4-1.0-20261101/` scored the fine-tune at mxfp4 but could compare it only with the
guided base at int8. This run closes that gap: on a 32 GB Mac, is the fine-tune with no guide or the base with the guide
the better model at about 21 GB?

## Setup

The recipe of `../guided-base-1.0-20261031/`; only the quantization changes.
- **Model:** the base's bf16 export imported with `ollama create -q mxfp4`, the same import as the e3 copy, then copied
  with a 32K context.
- **Guide:** `docs/aether_for_llms_medium_contexts.md` at tag `1.0`, passed as the custom document `card`.
- **Grader:** the published `aether-1.0-macos-arm64` release binary, checked against the release's `SHA256SUMS`.
- **Settings:** T=0, seed 42, one repeat, two repair rounds, 32K context, 8,192 output tokens, thinking off
  (`"reasoning_effort": "none"`).

## Running it

`score.sh` fetches and checks the grader and the guide, imports the model when Ollama lacks it, stops any other loaded
model, runs the 3-task smoke and then the eight suites. The command is at the top of `score.sh`. Copy the reports here
as `score/mxfp4/guided__<suite>.json.gz` with `bench/results/pack_reports.py`.

## Results (2026-11-01)

The eight suites ran with no infrastructure failures, no empty answers and no length caps, on the 1.0 grader
(b968240e…), in 11 minutes. The model is 20.9 GB. Every first-attempt prompt is byte-identical to the int8 guided run's
(126 of 126). The reports are under `score/mxfp4/`.

| first attempt (after repair) | size | main (73) | held-out (53) |
|---|---|---|---|
| untrained MoE + 1.0 guide, int8 (`../guided-base-1.0-20261031/`) | 39 GB | 60 (71) | 33 (43) |
| **untrained MoE + 1.0 guide, mxfp4** | **20.9 GB** | **61 (67)** | **26 (40)** |
| e3 fine-tune, no guide, int8 (`../e3-int4-1.0-20261101/`) | 39 GB | 65 (70) | 36 (44) |
| **e3 fine-tune, no guide, mxfp4** | **20.9 GB** | **64 (68)** | **33 (40)** |

Task by task, with a two-sided sign test on the tasks that differ:
- **At the same size, the fine-tune leads on first attempts and ties after repair.** Against the guided base at mxfp4,
  e3 alone passes 23 first attempts and the guided base alone 13 (p ≈ 0.13): 9 to 6 on main, 14 to 7 on held-out. After
  repair they are level: 13 to 12 overall (p = 1.0), 68 and 67 on main, 40 and 40 on held-out.
  - The earlier comparison, against the guided base at int8, understated the fine-tune by giving the base 8 bits.
- **The guide also costs about 14K tokens in every prompt**, which the fine-tune does not pay.
- **4 bits cost the guided base about what they cost the fine-tune.** After repair the base goes from 71 and 43 to 67 and
  40; e3 goes from 70 and 44 to 68 and 40. The base's largest first-attempt drop is on held-out (33 to 26; tasks 14 to
  7, p ≈ 0.19), not separable from noise in this one run.
- **For a 32 GB Mac,** e3 at mxfp4 is the better choice of the two measured: no worse after repair, ahead on first
  attempts, with a short prompt.
