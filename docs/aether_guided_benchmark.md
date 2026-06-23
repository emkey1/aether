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
- **One shot, no repair.** Every score is a *single pass*. The model sees the
  task and the guide once and emits one program, with no retry and no
  compile-error or wrong-output diagnostics fed back to it. The harness does
  support a repair loop (`--repair-attempts N`, which returns failure diagnostics
  for another try), but it is **off** for this sweep, so a miss is a first-attempt
  miss, not a model that converged after several tries. The scores are a floor;
  turning repair on could only raise them.

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
*31 of 47 local models scored so far (the ≥60B set is served separately). exact/30 · ✓ = compiled & ran · released = YYYY-MM · served: MLX/GGUF = LM Studio 0.4.16, vLLM/TRT-LLM = claw1, Gemini API = Google cloud.*

*Excluded as harness-incompatible (not capability results): `starcoder2-7b` (2024-02, context-window overflow) and `stable-code-instruct-3b` (2024-03, chat-template parse failure).*

| model | size | quant | served | released | [small](aether_for_llms_with_small_contexts.md) | [full](aether_for_llms_and_others.md) |
|---|---|---|---|---|---|---|
| `gemini-3.1-pro-preview` | — | none | Gemini API | 2026-04 | 30/30 · 30✓ | 30/30 · 30✓ |
| `GLM-5.2` | — | none | Z.ai API | 2026-05 | 30/30 · 30✓ | 30/30 · 30✓ |
| `gemini-3-flash-preview` | — | none | Gemini API | 2026-04 | 30/30 · 30✓ | 30/30 · 30✓ |
| `gemini-2.5-pro` | — | none | Gemini API | 2025-06 | 30/30 · 30✓ | 30/30 · 30✓ |
| `gemini-2.5-flash` | — | none | Gemini API | 2025-06 | 29/30 · 29✓ | 30/30 · 30✓ |
| `openai/gpt-oss-120b` | 63 GB | MXFP4 | TRT-LLM | 2025-08 | 27/30 · 27✓ | 29/30 · 29✓ |
| `qwen3.5-122b-a10b-nvfp4` | 62 GB | NVFP4 | vLLM | 2026-02 | 29/30 · 30✓ | 30/30 · 30✓ |
| `qwen/qwen3.6-35b-a3b` | 37.75 GB | 8bit | MLX | 2026-04 | 30/30 · 30✓ | 30/30 · 30✓ |
| `nvidia/nemotron-3-nano` | 33.58 GB | 8bit | MLX | 2025-12 | 10/30 · 13✓ | 13/23 · 14✓ (incomplete) |
| `qwen/qwen3-vl-30b` | 33.53 GB | 8bit | MLX | 2026-02 | 14/30 · 27✓ | 3/30 · 5✓ |
| `qwen3-vl-30b-a3b-thinking-mlx` | 33.53 GB | 8bit | MLX | 2026-04 | 12/30 · 19✓ | 0/30 · 0✓ |
| `qwen/qwen3-30b-a3b-2507` | 32.46 GB | 8bit | MLX | 2025-07 | 27/30 · 27✓ | 27/30 · 28✓ |
| `qwen3.6-27b-mlx-oq8` | 28.6 GB | 8bit | MLX | 2026-04 | 19/30 · 19✓ | 14/30 · 14✓ |
| `gemma-4-26b-a4b-it` | 28.05 GB | Q8_0 | GGUF | 2026-04 | 28/30 · 28✓ | 25/30 · 28✓ |
| `GLM-4.6` | ? | — | GGUF | ? | 9/9 · 9✓ (incomplete) | — |
| `zai-org/glm-4.7-flash` | 20.0 GB | 8bit | MLX | 2025-12 | 0/30 · 0✓ | 0/30 · 0✓ |
| `qwen3.6-27b-claude-deckard-qx64-hi-mlx` | 19.58 GB | 6bit | MLX | 2026-04 | 24/30 · 24✓ | 22/30 · 22✓ |
| `qwq-32b` | 18.0 GB | Q6_K | GGUF | 2025-03 | 19/30 · 22✓ | 2/30 · 2✓ |
| `google/gemma-3n-e4b` | 15.74 GB | bf16 | MLX | 2025-06 | 17/30 · 21✓ | 19/30 · 24✓ |
| `deepseek-r1-distill-qwen-14b` | 15.7 GB | Q8_0 | GGUF | 2025-01 | 21/30 · 22✓ | 22/30 · 23✓ |
| `prism-coder-7b` | 15.24 GB | ? | GGUF | 2026-04 | 5/30 · 5✓ | 2/30 · 2✓ |
| `mistralai/devstral-small-2-2512` | 14.12 GB | 4bit | MLX | 2025-12 | 24/30 · 24✓ | 25/30 · 25✓ |
| `mistralai/devstral-small-2507` | 13.28 GB | 4bit | MLX | 2025-07 | 25/30 · 28✓ | 27/30 · 28✓ |
| `qwen3.5-9b-mlx` | 10.45 GB | 8bit | MLX | 2026-02 | 23/30 · 24✓ | 25/30 · 26✓ |
| `yi-coder-9b-chat@q8_0` | 9.3 GB | Q8_0 | GGUF | 2024-09 | 22/30 · 23✓ | 2/30 · 2✓ |
| `gemma-4-e4b-it-mlx@8bit` | 8.97 GB | 8bit | MLX | 2026-04 | 22/30 · 23✓ | 24/30 · 24✓ |
| `gemma-4-e4b-it-mlx@4bit` | 6.86 GB | 4bit | MLX | 2026-04 | 21/30 · 24✓ | 20/30 · 21✓ |
| `yi-coder-9b-chat@q4_k_m` | 5.5 GB | Q4_K_M | GGUF | 2024-09 | 23/30 · 25✓ | 2/30 · 2✓ |
| `qwen3.5-4b-mlx` | 5.16 GB | 8bit | MLX | 2026-02 | 1/1 · 1✓ (incomplete) | — |
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

These eight tasks are larger and harder than the v2 set, built to pull more
signal out of a model's real capability, particularly for the larger models.
The v2/30 board above saturates at the top (several models tie at a perfect
30/30), so it stops separating the strongest from one another. These tasks
carry bigger inputs and more layered logic (nested aggregation, finite-state
machines, streak detection, integer recursion), scored exact-stdout against an
oracle, which spreads the leaders back out. Same two guides and harness; a
separate table because the v2/30 board is already wide.

<!-- LEADERBOARD-LARGE:START -->
*15 models scored on the large data set (`tasks_hard.json`, 8 hard tasks). exact/8 · ✓ = compiled & ran. Cloud + claw1 first; locals to follow.*

| model | size | quant | served | released | [small](aether_for_llms_with_small_contexts.md) | [full](aether_for_llms_and_others.md) |
|---|---|---|---|---|---|---|
| `gemini-3.1-pro-preview` | — | none | Gemini API | 2026-04 | 7/8 · 7✓ | 8/8 · 8✓ |
| `GLM-5.2` | — | none | Z.ai API | 2026-05 | 8/8 · 8✓ | 8/8 · 8✓ |
| `gemini-3-flash-preview` | — | none | Gemini API | 2026-04 | 6/8 · 6✓ | 8/8 · 8✓ |
| `gemini-2.5-pro` | — | none | Gemini API | 2025-06 | 8/8 · 8✓ | 7/8 · 7✓ |
| `gemini-2.5-flash` | — | none | Gemini API | 2025-06 | 8/8 · 8✓ | 8/8 · 8✓ |
| `openai/gpt-oss-120b` | 63 GB | MXFP4 | TRT-LLM | 2025-08 | 7/8 · 7✓ | 7/8 · 7✓ |
| `qwen3.5-122b-a10b-nvfp4` | 62 GB | NVFP4 | vLLM | 2026-02 | 8/8 · 8✓ | 6/8 · 7✓ |
| `google/gemma-3n-e4b` | 15.74 GB | bf16 | MLX | 2025-06 | 0/8 · 0✓ | 0/8 · 0✓ |
| `deepseek-r1-distill-qwen-14b` | 15.7 GB | Q8_0 | GGUF | 2025-01 | 2/8 · 3✓ | 2/8 · 2✓ |
| `mistralai/devstral-small-2-2512` | 14.12 GB | 4bit | MLX | 2025-12 | 5/8 · 5✓ | 6/8 · 6✓ |
| `mistralai/devstral-small-2507` | 13.28 GB | 4bit | MLX | 2025-07 | 7/8 · 7✓ | 6/8 · 7✓ |
| `yi-coder-9b-chat@q8_0` | 9.3 GB | Q8_0 | GGUF | 2024-09 | 0/8 · 1✓ | 2/8 · 2✓ |
| `yi-coder-9b-chat@q4_k_m` | 5.5 GB | Q4_K_M | GGUF | 2024-09 | 1/8 · 4✓ | 1/8 · 4✓ |
| `deepseek-r1-distill-qwen-7b` | 4.68 GB | Q4_K_M | GGUF | 2025-01 | 0/8 · 0✓ | 0/8 · 0✓ |
| `ibm/granite-4-h-tiny` | 4.23 GB | Q4_K_M | GGUF | 2025-10 | 1/8 · 4✓ | 1/8 · 3✓ |
<!-- LEADERBOARD-LARGE:END -->

**Preliminary: the full-guide advantage is a middle-band effect.** A first pass
at the weaker models on this hard set lands a result neither obvious direction
predicted. On v2/30, the full guide's extra context helped *exactly* the weak
models (granite-4-tiny was full +3, gemma-3n full +2, deepseek-r1-14b full +1).
On the hard set that advantage does not widen; it vanishes. All three crater to
near-zero (0 to 2 of 8) on **both** guides, small tying full, and most outputs
no longer even compile. The reason is a floor: these models cannot solve the
hard tasks at all, so guide size has nothing to bite on. Put that next to the
top of the board, where the strongest models ace both sets either way (small ≈
full at the ceiling), and the shape is clear: the full guide's edge lives only
in the **middle band** — on tasks hard enough that the extra context is the
margin, yet still within the model's reach. More context helps precisely when
the task is hard-but-achievable, and nowhere else. (Preliminary: three weak
models so far; the rest of the local sweep will fill this in, and the claim
firms up or breaks with them.)

**The hard set also separates models that v2 cannot.** The first mid-tier
coders to land make the point sharply: `devstral-small` (a 24B coder quantized
to 4-bit, ~14GB) holds at **5-7 of 8**, near the cloud tier, while `yi-coder-9b`,
which scored about the same on v2/30 (22 vs 24-25), collapses to **0-2 of 8**.
v2/30 rated them peers; the hard tasks show devstral is far more capable. Code
specialization *does* help here, but specifically the agentic, compositional
kind devstral is trained for, not mere code fluency; it is the README's "a
guide carries reasoning, not code-model-ness" point seen from inside the
code-model camp.

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

## Tools and tasks

Everything here is reproducible from the public repos. The harness and task
sets live in the umbrella repo (`emkey1/pscal`, on the `AetherLang` branch); the
guides and write-ups live here in `emkey1/aether`.

- **Harness.** [`tools/aether_doc_bench.py`](https://github.com/emkey1/pscal/blob/AetherLang/tools/aether_doc_bench.py) issues each task, compiles and runs the model's program with the `aether` binary, and scores stdout byte-for-byte against an oracle.
- **Task sets (the examples).** [`tasks_v2_pos.json`](https://github.com/emkey1/pscal/blob/AetherLang/Tests/aether_doc_bench/tasks_v2_pos.json) is the v2/30 board; [`tasks_hard.json`](https://github.com/emkey1/pscal/blob/AetherLang/Tests/aether_doc_bench/tasks_hard.json) is the large set. Each task carries its prompt, input fixtures, and expected stdout.
- **Guides.** [full](aether_for_llms_and_others.md) and [small](aether_for_llms_with_small_contexts.md) — the only Aether any model here ever sees.
- **The training (no-guide) side.** Companion write-up [`aether_specialization_findings.md`](aether_specialization_findings.md); its corpus and example generators live under `Tests/aether_specialization/` in the umbrella repo.

## Status

Sweep in progress on v2/30. The two large MoEs are in (above); the local sweep is
working down from ~35B toward 2B. This note's gradient table is completed from the
per-model result JSONs as they accumulate.
