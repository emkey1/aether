# Guided Gemma 4 12B on Aether 1.0 (set up 2026-10-31)

The untrained Gemma 4 12B with the 1.0 medium guide in every prompt, on the eight suites, graded by the 1.0 release
binary. It asks whether a 12B model writes Aether well enough with the guide to be worth fine-tuning for machines with
32 GB of memory, where the 27B and the int8 MoE do not fit comfortably.

## Setup

The recipe of `../guided-base-1.0-20261031`; only the model changes.
- **Model:** `gemma4:12b-it-q8_0` from the Ollama library (11.9B parameters, 8-bit, Apache-2.0), copied to
  `aether-gemma4-12b-q8-32k` with a 32K context.
  - The 8-bit copy is the comparison with the earlier guided runs (bf16 27B, int8 MoE), and what a 32 GB machine
    runs comfortably.
  - The library's `qat` tag (Google's quantization-aware 4-bit) is a candidate for a second run.
- **Guide:** `docs/aether_for_llms_medium_contexts.md` at tag `1.0`, passed as the custom document `card`.
- **Grader:** the published `aether-1.0-macos-arm64` release binary, checked against the release's `SHA256SUMS`.
- **Settings:** T=0, seed 42, one repeat, two repair rounds, 32K context, 8,192 output tokens.
- **Thinking off.** Gemma 4 has the thinking capability. On Ollama's OpenAI endpoint `"reasoning_effort": "none"`
  turns it off (a one-line answer: 18 completion tokens, no reasoning); `"think": false` did not (157 tokens, 370
  characters of reasoning).

## Running it

`score.sh` fetches and checks the grader and the guide, makes the 32K copy, runs the 3-task smoke and then the eight
suites. The smoke fails the model if an answer is empty, begins with a reasoning trace or costs 1,000 completion
tokens or more, or if a prompt is under 10,000 tokens. Each suite resumes from its checkpoint; re-run the script to
continue. The command is at the top of `score.sh`. Copy the reports here as `score/g4/guided__<suite>.json.gz` with
`bench/results/pack_reports.py`.

## Results

Pending. The anchors, first attempt (after repair):

| guided, 1.0 guide | main (73) | held-out (53) |
|---|---|---|
| Qwen3.8-27B, bf16 | 68 (72) | 48 (53) |
| Qwen3.6-35B-A3B, int8 | 60 (71) | 33 (43) |
