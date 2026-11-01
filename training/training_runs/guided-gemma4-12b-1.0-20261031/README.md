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

## Results (2026-11-01)

All eight suites ran with no infrastructure failures and no empty answers, every report on this destination and the
1.0 grader (b968240e…). The sweep took 2 h 41 min. The reports are under `score/g4/`. The guided runs of
`../guided-base-1.0-20261031/` are the comparison:

| guided, 1.0 guide, first attempt (after repair) | main (73) | held-out (53) |
|---|---|---|
| Qwen3.8-27B, bf16 | 68 (72) | 48 (53) |
| Qwen3.6-35B-A3B, int8 | 60 (71) | 33 (43) |
| **Gemma 4 12B, q8_0** | **48 (67)** | **26 (41)** |

- **Repair carries it.** After repair Gemma is 4 short of the MoE on main and 2 on held-out. On first attempts it is 12
  and 7 short. After repair, task by task, Gemma alone passes 1 task on main and 9 on held-out; the MoE alone passes
  5 and 11.
- **One habit is 12 of its 52 first-attempt failures.** On tasks that read JSON it writes the `if !has_toon() { ... }`
  guard at the top of the file, above `fn main`, so the program fails ENTRY-001 (top-level statements beside main).
  The guide shows the guard only inside main. Nine of the twelve passed on the first repair. It accounts for 9 of the
  11 first-attempt failures in `hard_v2` (3 of 14 passed).
- **The rest.** 19 other coded errors (SYN-001 the most common, 8) and 21 silent or uncoded failures: wrong output that
  compiled. The guided 27B had 10 first-attempt failures in all, the MoE 33.
- **Against the 27B.** Gemma passes 4 tasks first time that the 27B does not (`cs_edit_distance`, `algo_bignum_add`,
  `algo_merge_k_sorted`, `tuple_recursive_digits`), and after repair 1 (`cs_sieve_primes`, the 27B's only miss). The
  27B passes all 18 tasks Gemma still fails after repair.
- **Three tasks hit the 8,192-token cap** (four attempts). Both repairs of `cs_fizzbuzz` reasoned in comments ("the
  logic above still has the multiple-if issue") and then rewrote the same program until the cap. The first attempts
  of `spec_nearest_rank_percentile` (repaired) and `sealed_mstream_toon_body` (still failing) ran on in comments the
  same way.
- **Prompt size.** Gemma's tokenizer counts the guided prompt at 15,100–15,400 tokens, about 11% more than the
  harness estimate, well inside the 32K context.
