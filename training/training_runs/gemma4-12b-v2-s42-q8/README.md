# The Gemma 4 12B fine-tune's second seed at 8 bits (set up 2026-11-05)

`../gemma4-12b-v2-s42/`, the same recipe and data as the published fine-tune with a different seed, at 8 bits, with no guide, on the eight suites, graded by the
1.0 release binary. At 16 bits it scored 96 (116) of 126 against the published seed's 97 (105), and 31 (45) against 30 (36) on held-out
(`../gemma4-12b-v3/RESULTS.md`).

**The question:** does that lead after repair survive in the form people run? Few programs stay byte-identical when a model is quantized, so this also serves as a
partial second sample of a lead that was found on the suites that report it. Corrected on 2026-11-05: that was assumed, not checked. 77 of the 126 first programs stayed
identical, so it is a weak second sample.

**Result: it does not keep the lead by this reading, and the published form stays.** See `RESULTS.md`.

## The reading, fixed before scoring

`compare.py` computes it. The comparison is against the published int8 form (`../gemma4-12b-v2-q8/`, 93 (106) of 126), task by task.

- **Measure:** tasks only one of the two passes after repair, over all 126 tasks, with a two-sided sign test.
- **It keeps the lead if both hold:** more tasks in its favour at p < 0.05, and first attempts over all 126 tasks are not lower at p < 0.05.
- **If it does not:** the published form stays, and nothing is offered to replace it.
- **If it does:** whether to publish it is the owner's decision. It is still a comparison of two sets of weights, not of recipes.
- **Reported, not deciding:** main and held-out apart, each suite, coded / return code / wrong output, failures repair fixed, against the published forms of e3 too.

Held-out failures stay unread.

## Setup

The recipe of `../gemma4-12b-v2-q8/`; only the weights change.
- **Model:** the merged 16-bit export of `gemma4-12b-v2-s42`, checked by SHA-256 after the copy, imported into Ollama with `ollama create -q int8` and copied with a
  32K context. Its config, tokenizer and chat template are checked against the published seed's export, and the served prompt with `_debug_render_only` before scoring.
- **Grader:** the published `aether-1.0-macos-arm64` release binary, already on the scoring Mac and checked by its hash.
- **Settings:** T=0, seed 42, one repeat, two repair rounds, 8,192 output tokens, thinking off (`"reasoning_effort": "none"`).

## Running it

The command is at the top of `score.sh`. It reuses the smoke tasks and the generation check of `../gemma4-12b-v2-q8/`. Copy the reports here as
`score/tuned__<suite>.json.gz` with `bench/results/pack_reports.py`.
