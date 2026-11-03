# The Gemma 4 12B fine-tune at 8 bits (set up 2026-11-03)

The final step of `../gemma4-12b-v2/` at 8 bits, with no guide, on the eight suites, graded by the 1.0 release binary.
It answers the question that run left open: does the model keep its scores in the form a 32 GB Mac would run? It also
decides whether the model is worth publishing: at 16-bit it led the guided base's 26 held-out first attempts by only 4,
and e3 lost 3 going to int8.

## Setup

The recipe of `../e3-int4-1.0-20261101/`; only the model changes.
- **Model:** the merged 16-bit export of `gemma4-12b-v2`, checked by SHA-256 after the copy, imported into Ollama with
  `ollama create -q int8` (its MLX quantization) and copied with a 32K context.
  - The import is 12 GB. It keeps Ollama's own `gemma4` renderer and parser.
  - Checked with `_debug_render_only` before scoring: on `/v1` with `"reasoning_effort": "none"` a message renders as
    `<bos><|turn>user\n...<turn|>\n<|turn>model\n<|channel>thought\n<channel|>`, the one BOS and empty thought channel the
    fine-tune trained on. Native `/api/chat` needs `"think": false` for the same prompt: with no field it turns thinking on.
- **No guide.** The fine-tune is scored as it would be released, on prompts of a few hundred tokens.
- **Grader:** the published `aether-1.0-macos-arm64` release binary, checked against the release's `SHA256SUMS`.
- **Settings:** T=0, seed 42, one repeat, two repair rounds, 8,192 output tokens, thinking off
  (`"reasoning_effort": "none"`).

## Running it

`score.sh` imports the model if Ollama lacks it, stops any other loaded Ollama model, runs the 3-task smoke and then the
eight suites. The smoke fails if an answer is empty, begins with a reasoning trace or costs 1,000 completion tokens or
more, or if a prompt is 3,000 tokens or more. Each suite resumes from its checkpoint; re-run the script to continue. The
command is at the top of `score.sh`. Copy the reports here as `score/tuned__<suite>.json.gz` with
`bench/results/pack_reports.py`.
