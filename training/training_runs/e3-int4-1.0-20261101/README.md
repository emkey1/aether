# The MoE fine-tune at 4-bit (set up 2026-11-01)

The released MoE fine-tune `qwen36-35b-a3b-e3` quantized to 4 bits, with no guide, on the eight suites, graded by the
1.0 release binary. It asks whether the fine-tune keeps its scores at a size that fits a 32 GB Mac. The guided Gemma 4
12B (`../guided-gemma4-12b-1.0-20261031/`) is the untrained alternative in that class.

## Setup

- **Model.** e3's merged bf16 export (`../qwen36-35b-a3b-e3/`), imported into Ollama twice:
  - **int4** (`ollama create -q int4`, Ollama's MLX quantization): the question.
  - **int8** (`-q int8`): the import the Python check used in October. e3's published Aether scores come from vLLM at
    bf16, so this run on the same stack as the int4 one separates the serving stack from the cost of 4 bits.
  - Each is copied with a 32K context.
- **No guide.** The fine-tune is scored as it was released, on prompts of a few hundred tokens.
- **Grader:** the published `aether-1.0-macos-arm64` release binary, checked against the release's `SHA256SUMS`.
- **Settings:** T=0, seed 42, one repeat, two repair rounds, 8,192 output tokens, thinking off
  (`"reasoning_effort": "none"`, as in the earlier Ollama runs).

## Running it

`score.sh` fetches and checks the grader, imports a variant when Ollama lacks it, stops any other loaded model, runs
the 3-task smoke and then the eight suites, int4 first. The smoke fails a variant if an answer is empty, begins with a
reasoning trace or costs 1,000 completion tokens or more, or if a prompt is 3,000 tokens or more (a guide got in).
Each suite resumes from its checkpoint; re-run the script to continue. The command is at the top of `score.sh`. Copy
the reports here as `score/<variant>/tuned__<suite>.json.gz` with `bench/results/pack_reports.py`.

## Results

Pending. The bars, first attempt (after repair):

| | main (73) | held-out (53) |
|---|---|---|
| `qwen36-35b-a3b-e3`, bf16 on vLLM (replayed on the 1.0 grader) | 65 (70) | 39 (47) |
| MoE + 1.0 guide, int8, untrained | 60 (71) | 33 (43) |
| Gemma 4 12B + 1.0 guide, q8_0, untrained | 48 (67) | 26 (41) |
