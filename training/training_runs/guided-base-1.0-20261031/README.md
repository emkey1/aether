# Guided base on Aether 1.0 (set up 2026-10-31)

The guided comparison for the next fine-tunes: the untrained Qwen3.6-35B-A3B and Qwen3.8-27B with the 1.0 medium
guide in every prompt, on the eight suites, graded by the 1.0 release binary.

## Why this run

On 2026-10-31 a zero-GPU replay on the 1.0 compiler (2026-10-30-1) re-graded everything the published fine-tunes
and their guided baselines wrote:
- `qwen36-35b-a3b-e3` and `qwen38-27b-v1`: 252 first and 63 repair attempts, no class moved;
- `../base-qwen36-35b-a3b-int8-20261022` and `../guided-qwen38-27b-1.0-beta2`: 378 first and 330 repair attempts, no
  pass moved.

The fine-tunes see no guide and their task prompts are unchanged, so a live re-run would regenerate the same first
attempts. The 1.0 diagnostics change the repair feedback of only 2 of the MoE's 44 failing attempts and 9 of the
27B's 36. The guided baselines are different: their guide has moved from 1.0-beta2 (2026-10-20-8) to 1.0
(2026-10-28-1, guide pass G1 and the fixes since). Their published numbers no longer say what an untrained model
does with today's guide.

## Setup

One change from the two earlier guided runs: the guide (and the grader that goes with it).
- **Models:** the same Ollama imports on the scoring Mac, one at a time:
  - `aether-qwen36-35b-a3b-base-q8-32k` (int8, `ollama create -q int8`), as in `../base-qwen36-35b-a3b-int8-20261022`;
  - `aether-qwen38-27b-base-32k` (bf16), as in `../guided-qwen38-27b-1.0-beta2`.
- **Guide:** `docs/aether_for_llms_medium_contexts.md` at tag `1.0` (sha256 b693a67bc16cba2f…), passed as the custom
  document `card` like the earlier runs.
- **Grader:** the published `aether-1.0-macos-arm64` release binary, checked against the release's `SHA256SUMS`.
- **Settings:** T=0, seed 42, one repeat, two repair rounds, thinking off, 32K context, 8,192 output tokens.
- **Harness and tasks:** unchanged since the 1.0 tag.

## Running it

`score.sh` fetches and checks the grader and the guide, then for each model runs a 3-task smoke and the eight suites.
- The smoke fails a model whose answers are empty or begin with a reasoning trace, or whose prompts are under 10,000
  tokens. A guided prompt is about 14K tokens; the 2026-10-22 run once went out without its guide.
- Each suite resumes from its checkpoint; re-run the script to continue.
- The earlier runs took about 25 minutes (MoE) and an hour (27B).

The command is at the top of `score.sh`. The reports land under `~/aether-models/guided-base-1.0-20261031/score/`
on the scoring Mac. Copy them here as `score/<moe|27b>/guided__<suite>.json.gz`.

## Results

Pending.
