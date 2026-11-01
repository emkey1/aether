# The guided MoE base at mxfp4 (set up 2026-11-01)

The untrained Qwen3.6-35B-A3B quantized to mxfp4, with the 1.0 medium guide in every prompt, on the eight suites, graded
by the 1.0 release binary. `../e3-int4-1.0-20261101/` scored the fine-tune at mxfp4 but could compare it only with the
guided base at int8. This run closes that gap: on a 32 GB Mac, is the fine-tune with no guide or the base with the guide
the better 21 GB model?

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

## Results

Pending. The bars, first attempt (after repair):

| | main (73) | held-out (53) |
|---|---|---|
| untrained MoE + 1.0 guide, int8 | 60 (71) | 33 (43) |
| e3 fine-tune, no guide, mxfp4 | 64 (68) | 33 (40) |
