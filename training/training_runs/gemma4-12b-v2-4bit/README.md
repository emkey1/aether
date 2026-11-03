# The Gemma 4 12B fine-tune at 4 bits on a Mac (set up 2026-11-03)

The final step of `../gemma4-12b-v2/` at nvfp4 and mxfp4, with no guide, on the eight suites, graded by the 1.0 release
binary. It asks how far the model can shrink before its scores go: on e3, 4 bits cost a few held-out tasks. A Mac has no FP4
hardware, so this run gives scores, sizes and memory, not the speed FP4 might gain on Blackwell. `../gemma4-12b-v2-nvfp4/`
tests that.

## Setup

The recipe of `../gemma4-12b-v2-q8/`; only the quantization changes.
- **Model:** the merged 16-bit export of `gemma4-12b-v2`, imported into Ollama with `ollama create -q <format>`, each
  copied with a 32K context.
- **Settings:** T=0, seed 42, one repeat, two repair rounds, 8,192 output tokens, thinking off
  (`"reasoning_effort": "none"`). Grader: the 1.0 release binary, checked by hash.
- **Memory:** Ollama's resident size is logged after every suite, because Gemma's int8 copy grew from 15 GB to 21 GB over a pass.
  e3 at mxfp4 then repeats its own pass to get the same reading; its scores stay in `../e3-int4-1.0-20261101/`.

## Running it

The command is at the top of `score.sh`. Each suite resumes from its checkpoint; re-run the script to continue. Copy the
reports here as `score/<format>/tuned__<suite>.json.gz` with `bench/results/pack_reports.py`.
