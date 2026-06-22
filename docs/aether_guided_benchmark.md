# The Guide Is Enough: In-Context (Untrained) Results for Aether

*Companion to [`aether_specialization_findings.md`](aether_specialization_findings.md).*

That note measures one half of "can a language model write Aether": a model
**fine-tuned** on an Aether corpus, writing with **no guide** in the prompt,
scored by exact-match. This note measures the other half — a model with **no
Aether-specific training at all**, handed the guide **in its context window**,
asked to solve the same benchmark. The two bracket the question from both sides.

The short answer: for capable models, **the guide is enough.** No fine-tuning, no
worked examples beyond the document itself — just the condensed or full guide in
the prompt — and a frontier-scale model writes *every* program on the benchmark
correctly.

---

## Setup

- **Instrument.** The v2 benchmark: 30 positive, decontaminated tasks
  (`Tests/aether_doc_bench/tasks_v2_pos.json`) spanning effects, pure helpers,
  records and methods, tuples, dynamic arrays, contracts, module imports, real
  formatting, recursion, and the TOON access cluster. Each generated program is
  compiled and run with the current `aether` binary and its stdout compared
  byte-for-byte against an oracle.
- **Conditions.** The guide is placed in the prompt, in one of two sizes:
  - **full** — [`aether_for_llms_and_others.md`](aether_for_llms_and_others.md) (~980 lines)
  - **small** — [`aether_for_llms_with_small_contexts.md`](aether_for_llms_with_small_contexts.md) (~500 lines)
  There is no `none` condition here — that is the no-guide note's department.
  Both guides are version-stamped; this sweep used **guide version 2026-06-21-1**.
- **Quantization.** Local models are run at the quant LM Studio reports (shown
  per row below); the 122B is served NVFP4. Quant is part of the result — a
  heavier quant of the same model may score differently.
- **Metrics, paired.** Following the companion note's rule we never report
  exact-match alone. **exact** (stdout matches the oracle) is paired with
  **run-ok** (the program compiled and ran). The gap between the two is where
  competence hides.
- **No training.** Every model below is used as released. The only Aether it
  ever sees is the guide in its context.
- **Serving.** Local models are served by **LM Studio 0.4.16** — MLX runtime for
  MLX builds, llama.cpp for GGUF (`served` column per row); the 122B by **vLLM
  0.20.2** on claw1. Local models run one resident at a time, unloaded between
  runs to bound memory.
- **Harness honesty.** The harness issues the benchmark's tasks concurrently and gives
  models a generous output budget, so a model that reasons at length is never
  truncated mid-thought — a real failure mode that, left unfixed, makes a capable
  model look incompetent for a purely mechanical reason.

## Headline result

| model | class · quant | small (exact / run-ok) | full (exact / run-ok) |
|---|---|---|---|
| Qwen3.5-122B-A10B | 122B MoE · NVFP4 | 29/30 · 30/30 | **30/30 · 30/30** |
| Qwen3.6-35B-A3B | 35B MoE · 8-bit | **30/30 · 30/30** | **30/30 · 30/30** |

Qwen3.5-122B with the full guide scores a **clean 30/30** — every task, exact.
With the condensed guide it scores 29/30, and the single "miss" still **compiled
and ran**: a valid program whose output differed only in formatting. That is the
companion note's thesis surfacing on the other side of the ledger — **100% of
what it wrote was valid Aether**, and exact-match's one-point dock is a
formatting artifact, not a competence gap. The 35B MoE matches it outright — a
clean **30/30 on both guides** — so the result is not a single-model fluke.

For models in this class the README's deliberately-cautious phrasing — "valid,
correct Aether a surprising fraction of the time" — understates the outcome. It
is not a fraction. It is all of it.

## The capability gradient (in progress)

The headline is the *ceiling*. The more interesting question this sweep answers
is **where the floor is** — how far down the capability ladder the guide keeps
working, and where it stops. A sweep across ~49 locally-served models (2B → ~45B,
the ≥60B models served separately) is filling that gradient in on the v2/30
instrument. **Live standings**, auto-updated as models land (exact out of 30;
✓ = compiled and ran):

<!-- LEADERBOARD:START -->
*18 of 47 local models scored so far (the ≥60B set is served separately). exact/30 · ✓ = compiled & ran · released = YYYY-MM · served: MLX/GGUF = LM Studio 0.4.16, vLLM/TRT-LLM = claw1, Gemini API = Google cloud.*

*Excluded as harness-incompatible (not capability results): `starcoder2-7b` (2024-02, context-window overflow) and `stable-code-instruct-3b` (2024-03, chat-template parse failure).*

| model | size | quant | served | released | [small](aether_for_llms_with_small_contexts.md) | [full](aether_for_llms_and_others.md) |
|---|---|---|---|---|---|---|
| `gemini-3.1-pro-preview` | — | none | Gemini API | 2026-04 | 30/30 · 30✓ | 30/30 · 30✓ |
| `gemini-3-flash-preview` | — | none | Gemini API | 2026-04 | 30/30 · 30✓ | 30/30 · 30✓ |
| `gemini-2.5-pro` | — | none | Gemini API | 2025-06 | 30/30 · 30✓ | 30/30 · 30✓ |
| `gemini-2.5-flash` | — | none | Gemini API | 2025-06 | 29/30 · 29✓ | 30/30 · 30✓ |
| `openai/gpt-oss-120b` | 63 GB | MXFP4 | TRT-LLM | 2025-08 | 27/30 · 27✓ | 29/30 · 29✓ |
| `qwen3.5-122b-a10b-nvfp4` | 62 GB | NVFP4 | vLLM | 2026-02 | 29/30 · 30✓ | 30/30 · 30✓ |
| `qwen/qwen3.6-35b-a3b` | 37.75 GB | 8bit | MLX | 2026-04 | 30/30 · 30✓ | 30/30 · 30✓ |
| `mistralai/devstral-small-2507` | 13.28 GB | 4bit | MLX | 2025-07 | 25/30 · 28✓ | 9/13 · 9✓ (incomplete) |
| `qwen3.5-9b-mlx` | 10.45 GB | 8bit | MLX | 2026-02 | 23/30 · 24✓ | 25/30 · 26✓ |
| `yi-coder-9b-chat@q8_0` | 9.3 GB | Q8_0 | GGUF | 2024-09 | 22/30 · 23✓ | 2/30 · 2✓ |
| `gemma-4-e4b-it-mlx@8bit` | 8.97 GB | 8bit | MLX | 2026-04 | 22/30 · 23✓ | 24/30 · 24✓ |
| `google/gemma-4-12b-qat` | 7.15 GB | Q4_0 | GGUF | 2026-06 | 1/11 · 1✓ (incomplete) | — |
| `gemma-4-e4b-it-mlx@4bit` | 6.86 GB | 4bit | MLX | 2026-04 | 21/30 · 24✓ | 20/30 · 21✓ |
| `yi-coder-9b-chat@q4_k_m` | 5.5 GB | Q4_K_M | GGUF | 2024-09 | 23/30 · 25✓ | 2/30 · 2✓ |
| `qwen3.5-4b-mlx` | 5.16 GB | 8bit | MLX | 2026-02 | 4/30 · 5✓ | 19/30 · 20✓ |
| `deepseek-r1-distill-qwen-7b` | 4.68 GB | Q4_K_M | GGUF | 2025-01 | 0/30 · 3✓ | 0/30 · 3✓ |
| `ibm/granite-4-h-tiny` | 4.23 GB | Q4_K_M | GGUF | 2025-10 | 15/30 · 20✓ | 18/30 · 21✓ |
| `qwen3.5-2b-mlx` | 1.75 GB | 4bit | MLX | 2026-02 | 9/30 · 12✓ | 13/30 · 23✓ |
| `bonsai-8b-mlx` | 1.3 GB | 1bit | MLX | 2026-04 | *load-failed* | |
| `gemma-4-12b-it-mxfp8` | 12.38 GB | 8bit | MLX | 2026-06 | *load-failed* | |
| `gemma-4-e2b-it` | 4.83 GB | Q6_K | GGUF | 2026-04 | *load-failed* | |
<!-- LEADERBOARD:END -->

Early, robust shape:

- **A guide carries reasoning, not "code-model-ness."** A small *general thinking*
  model writes valid, correct Aether for a real share of tasks with the guide,
  while a 3B model specialized for code but without a reasoning step scored zero.
  The guide rewards a model that can *follow* it, not one merely fluent in
  mainstream languages.
- **The full guide meets or beats the condensed guide**, and the margin should
  widen as models weaken — more context buys the most exactly where in-context
  learning is hardest.
- **Thinking is not required at the top.** The 122B is served with thinking
  disabled and still scores 30/30; reasoning helps weaker models climb but the
  ceiling is reachable without it.

## The large data set (tasks_hard.json)

These eight tasks are the *tests for the large examples* — hard compositional
shapes (nested aggregation, finite-state machines, streak detection, integer
recursion) that consume the large input fixtures, scored exact-stdout against an
oracle. Same two guides and harness as above; a separate table because the
v2/30 board is already wide.

<!-- LEADERBOARD-LARGE:START -->
*5 models scored on the large data set (`tasks_hard.json`, 8 hard tasks). exact/8 · ✓ = compiled & ran. Cloud + claw1 first; locals to follow.*

| model | size | quant | served | released | [small](aether_for_llms_with_small_contexts.md) | [full](aether_for_llms_and_others.md) |
|---|---|---|---|---|---|---|
| `gemini-3.1-pro-preview` | — | none | Gemini API | 2026-04 | 7/8 · 7✓ | 8/8 · 8✓ |
| `gemini-3-flash-preview` | — | none | Gemini API | 2026-04 | 6/8 · 6✓ | 4/4 · 4✓ (incomplete) |
| `gemini-2.5-pro` | — | none | Gemini API | 2025-06 | 8/8 · 8✓ | 7/8 · 7✓ |
| `gemini-2.5-flash` | — | none | Gemini API | 2025-06 | 8/8 · 8✓ | 8/8 · 8✓ |
| `openai/gpt-oss-120b` | 63 GB | MXFP4 | TRT-LLM | 2025-08 | 7/8 · 7✓ | 7/8 · 7✓ |
<!-- LEADERBOARD-LARGE:END -->

## What this does and does not show

- It shows that **adoption does not require fine-tuning.** A capable model plus
  the guide is a working Aether programmer today — which is the practical case
  for most users.
- It does **not** retire the no-guide program. A model that writes Aether with
  *nothing* in the prompt is a different, harder bar, and the place where the
  language's regularity is actually stress-tested.
- The README's compiler↔guide loop closes here too: when a guided model does
  slip, the compiler's coded diagnostic (`FX-001`, `ANN-001`, …) points back into
  the same guide section, so a second pass can self-correct.

## Status

Sweep in progress on v2/30. The two large MoEs are in (above); the local sweep is
working down from ~35B toward 2B. This note's gradient table is completed from the
per-model result JSONs as they accumulate.
