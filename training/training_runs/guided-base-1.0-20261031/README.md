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
- **Settings:** T=0, seed 42, one repeat, two repair rounds, 32K context, 8,192 output tokens. Thinking off
  through `"extra_body": {"reasoning_effort": "none"}`, as in the earlier runs' destinations. Both Ollama imports
  have the thinking capability with default `true`, and the reports do not record `extra_body`.
- **Harness and tasks:** unchanged since the 1.0 tag.

## Running it

`score.sh` fetches and checks the grader and the guide, then for each model runs a 3-task smoke and the eight suites.
- The smoke fails a model whose answers are empty or begin with a reasoning trace, or whose prompts are under 10,000
  tokens. A guided prompt is about 14K tokens; the 2026-10-22 run once went out without its guide.
- Each suite resumes from its checkpoint; re-run the script to continue.
- The earlier runs took about 25 minutes (MoE) and an hour (27B); this one took less (see Results).

The command is at the top of `score.sh`. The reports land under `~/aether-models/guided-base-1.0-20261031/score/`
on the scoring Mac. Copy them here as `score/<moe|27b>/guided__<suite>.json.gz`.

## First attempt, rejected (2026-10-31)

The first sweep's destinations left out `reasoning_effort`, so both models ran with thinking on. Ollama puts the
reasoning in a separate field, so the content-prefix leak check passed it.
- The tell was the token counts. The smoke answers (38-84 tokens of Aether) cost about 1,700 completion tokens each,
  and v2_pos took a median of 2,490 against 115 in the 2026-10-22 run.
- Six hard-suite answers reasoned to the 8,192-token cap and returned no content; they were scored as no-answer
  failures.
- It was stopped after five of the MoE's eight suites. The reports are kept on the scoring Mac under
  `score-rejected-thinking-on/` and are not results.
- The smoke test now also fails a model whose smoke answers cost 1,000 completion tokens or more.

## Results (2026-10-31)

Both models ran all eight suites with no infrastructure failures, every report on the thinking-off destination and
the 1.0 grader (b968240e…). After the resume the MoE took 12 minutes and the 27B 38. The reports are under `score/`.

| first attempt (after repair) | main (73) | held-out (53) |
|---|---|---|
| MoE + 1.0-beta2 guide (`../base-qwen36-35b-a3b-int8-20261022/`) | 60 (69) | 29 (38) |
| **MoE + 1.0 guide** | **60 (71)** | **33 (43)** |
| 27B + 1.0-beta2 guide (`../guided-qwen38-27b-1.0-beta2/`) | 67 (70) | 47 (52) |
| **27B + 1.0 guide** | **68 (72)** | **48 (53)** |

Task by task, against the 1.0-beta2 runs:
- **MoE.** 26 of the 126 tasks changed status.
  - First attempts: 12 gained, 8 lost. At T=0 a different prompt moves the tasks near the boundary both ways, so the
    +4 is mostly churn.
  - After repair: 10 gained, 3 lost (sign test p ≈ 0.09). This is the firmer gain, where the 1.0 diagnostics act.
- **27B.** 4 tasks changed, all gains.
  - First attempts: `hard2_event_sessionize` and `sealed_toon_effective_config`.
  - After repair: `hard2_event_sessionize`, `hard2_expr_eval` and `algo_edit_distance_script`.
  - It passes 125 of 126 after repair; `cs_sieve_primes` is the one left.
- **Not the grader.** The replay that motivated this run re-graded both old runs' programs on the 1.0 grader without
  moving a pass. The differences are in what the models wrote.
- **One length cap.** One MoE first attempt (`spec_league_table`) reached the 8,192-token cap: a program that ran on
  into a monologue in comments. It was repaired.

Against the published fine-tunes, which see no guide. Their replay on the 1.0 grader moved nothing either, so the
columns compare:

| first attempt (after repair) | main (73) | held-out (53) |
|---|---|---|
| `qwen36-35b-a3b-e3`, no guide | 65 (70) | 39 (47) |
| MoE + 1.0 guide | 60 (71) | 33 (43) |
| `qwen38-27b-v1`, no guide | 71 (72) | 37 (46) |
| 27B + 1.0 guide | 68 (72) | 48 (53) |

- The MoE fine-tune still beats its guided base on first attempts: by 5 on main and 6 on held-out. After repair they
  are within one on main (70 vs 71); the fine-tune keeps 4 on held-out (47 vs 43).
- The 27B fine-tune leads its guided base only on main first attempts (71 vs 68). On held-out the guided base leads
  by 11 (7 after repair), against 10 (6) with the 1.0-beta2 guide.
- These are the bars for the next fine-tunes on the 1.0 data.
