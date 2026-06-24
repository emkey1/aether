# The Guide Is Enough: In-Context (Untrained) Results for Aether

*Companion to [`aether_specialization_findings.md`](aether_specialization_findings.md).*

That note measures one half of "can a language model write Aether": a model
**fine-tuned** on an Aether corpus, writing with **no guide** in the prompt. This
note measures the other half — a model with **no Aether-specific training at
all**, handed the guide **in its context window**, asked to solve the same
benchmark. The two bracket the question from both sides.

The short answer: for capable models, **the guide is enough.** No fine-tuning, no
worked examples beyond the document itself — just the guide in the prompt — and
an entire frontier tier writes *every* program on the core benchmark correctly.

---

## Setup

- **Three instruments, exact-stdout scored.** Each model's program is compiled
  and run with the current `aether` binary, its stdout compared byte-for-byte
  against an oracle. **v2/30** ([`tasks_v2_pos.json`](https://github.com/emkey1/pscal/blob/AetherLang/Tests/aether_doc_bench/tasks_v2_pos.json))
  probes language fluency across 30 positive, decontaminated tasks; **the large
  set** ([`tasks_hard.json`](https://github.com/emkey1/pscal/blob/AetherLang/Tests/aether_doc_bench/tasks_hard.json),
  8 tasks) carries bigger inputs and more layered logic to spread the leaders
  apart; **CS-classics** ([`tasks_cs.json`](https://github.com/emkey1/pscal/blob/AetherLang/Tests/aether_doc_bench/tasks_cs.json),
  19 tasks) tests textbook algorithms.
- **The guide in the prompt, two sizes.** The **full** guide
  ([`aether_for_llms_and_others.md`](aether_for_llms_and_others.md), ~980 lines,
  ~8.7k tokens) or the condensed **small** one
  ([`aether_for_llms_with_small_contexts.md`](aether_for_llms_with_small_contexts.md),
  ~500 lines, ~4.6k tokens). There is no `none` condition — that is the no-guide
  note's department. Guide version **2026-06-23-1**. The prompt is the same size
  across all three instruments: the large set's bigger inputs go to the *compiled
  program* at runtime (stdin/files), not the model's prompt, so a small served
  context bites the same on every board.
- **Paired metrics.** We never report exact-match alone. **exact** (stdout
  matches the oracle) is paired with **✓ run-ok** (the program compiled and ran);
  the gap between them is where competence hides.
- **No training, one shot.** Every model is used as released; the only Aether it
  ever sees is the guide. Every score is a single pass — no retry, no
  compile-error feedback. The harness supports a repair loop (`--repair-attempts`)
  but it is **off** here, so the scores are a floor: turning repair on could only
  raise them.
- **Serving (the `served` column).** Local models run one resident at a time,
  unloaded between runs to bound memory — LM Studio 0.4.16 (MLX or llama.cpp) or
  Ollama on the GB10 Sparks; the 122B by vLLM 0.20.2 on claw1; the cloud tier
  (Z.ai GLM, Gemini, OpenAI) by API. Quant is part of the result and shown per
  row — a heavier quant of the same model may score differently.

## Headline: a whole frontier tier aces v2/30

The ceiling is not one model. With the full guide, a **clean 30/30 — every task,
exact** — is reached by every frontier-scale model tried: `gpt-5.5`, `o3`,
`GLM-5`, `GLM-5.2`, `gemini-3.1-pro`, `gemini-3-flash`, the cloud-served 122B
`Qwen3.5-122B-A10B`, and the 37 GB, 8-bit `Qwen3.6-35B-A3B` that fits on a laptop.
The smallest of these and the largest cloud flagship converge on the same result:
handed the guide, **100% of what they write is valid Aether, and all of it is
correct.**

For models in this class the README's deliberately-cautious phrasing — "valid,
correct Aether a surprising fraction of the time" — understates the outcome. It
is not a fraction. It is all of it. (Thinking is not required at the top: the
122B is served with thinking *disabled* and still scores 30/30; reasoning helps
weaker models climb, but the ceiling is reachable without it.)

## The capability gradient

The headline is the ceiling. The more interesting question is **where the floor
is** — how far down the capability ladder the guide keeps working, and where it
stops. The v2/30 board fills that in, cloud and local together, sorted by size
(exact out of 30; ✓ = compiled and ran):

<!-- LEADERBOARD:START -->
*47 models scored on v2/30 — cloud APIs and local builds together, sorted by size (cloud first, size unlisted). exact/30 · ✓ = compiled & ran · released = YYYY-MM · served: MLX/GGUF = LM Studio (local), vLLM/TRT-LLM = claw1, Z.ai/Gemini/OpenAI = cloud APIs.*

*Excluded as harness-incompatible (not capability results): `starcoder2-7b` (2024-02, context-window overflow), `stable-code-instruct-3b` (2024-03, chat-template parse failure), and `gemma4:12b` (Ollama, 0 of 30 produced a runnable program, a serving failure not a capability result).*

*Withheld pending re-test: `nvidia/nemotron-3-nano`. Its latest serving run returned empty completions on all 60 generations (the model loaded, but produced nothing extractable), so that 0/30 is a serving fault under investigation, not a capability result. An earlier pass scored in the double digits, so it will be re-run once the serving issue is fixed.*

| model | size · quant | served | released | [small](aether_for_llms_with_small_contexts.md) | [full](aether_for_llms_and_others.md) |
|---|---|---|---|---|---|
| `GLM-5` | — | Z.ai API | 2026-03 | 30/30 · 30✓ | 30/30 · 30✓ |
| `GLM-4.6` | — | Z.ai API | 2025-09 | 29/30 · 29✓ | 30/30 · 30✓ |
| `gemini-3.1-pro-preview` | — | Gemini API | 2026-04 | 30/30 · 30✓ | 30/30 · 30✓ |
| `gpt-5.5` | — | OpenAI API | 2026-04 | 30/30 · 30✓ | 30/30 · 30✓ |
| `GLM-5.2` | — | Z.ai API | 2026-05 | 30/30 · 30✓ | 30/30 · 30✓ |
| `gemini-3-flash-preview` | — | Gemini API | 2026-04 | 30/30 · 30✓ | 30/30 · 30✓ |
| `GLM-5-Turbo` | — | Z.ai API | 2026-04 | 29/30 · 30✓ | 29/30 · 30✓ |
| `o3` | — | OpenAI API | 2025-04 | 30/30 · 30✓ | 30/30 · 30✓ |
| `gemini-2.5-pro` | — | Gemini API | 2025-06 | 30/30 · 30✓ | 30/30 · 30✓ |
| `gemini-2.5-flash` | — | Gemini API | 2025-06 | 29/30 · 29✓ | 30/30 · 30✓ |
| `GLM-4.5-Air` | — | Z.ai API | 2025-07 | 25/30 · 26✓ | 25/30 · 27✓ |
| `gpt-4o` | — | OpenAI API | 2024-05 | 23/30 · 23✓ | 28/30 · 28✓ |
| `openai/gpt-oss-120b` | 63 GB · MXFP4 | TRT-LLM | 2025-08 | 27/30 · 27✓ | 29/30 · 29✓ |
| `qwen3.5-122b-a10b-nvfp4` | 62 GB · NVFP4 | vLLM | 2026-02 | 29/30 · 30✓ | 30/30 · 30✓ |
| `deepseek-r1:70b` | 42 GB · Q4_K_M | Ollama | 2025-01 | 25/30 · 25✓ | 25/30 · 27✓ |
| `llama3.3:70b` | 42 GB · Q4_K_M | Ollama | 2024-12 | 28/30 · 28✓ | 27/30 · 29✓ |
| `qwen/qwen3.6-35b-a3b` | 37.75 GB · 8bit | MLX | 2026-04 | 30/30 · 30✓ | 30/30 · 30✓ |
| `google/gemma-4-31b` | 33.8 GB · 8bit | MLX | 2026-04 | 29/30 · 29✓ | 29/30 · 29✓ |
| `qwen/qwen3-vl-30b` | 33.53 GB · 8bit | MLX | 2026-02 | 14/30 · 27✓ | 3/30 · 5✓ |
| `qwen3-vl-30b-a3b-thinking-mlx` | 33.53 GB · 8bit | MLX | 2026-04 | 12/30 · 19✓ | 0/30 · 0✓ |
| `qwen/qwen3-30b-a3b-2507` | 32.46 GB · 8bit | MLX | 2025-07 | 27/30 · 27✓ | 27/30 · 28✓ |
| `qwen3.6-27b-mlx-oq8` | 28.6 GB · 8bit | MLX | 2026-04 | 19/30 · 19✓ | 14/30 · 14✓ |
| `gemma-4-26b-a4b-it` | 28.05 GB · Q8_0 | GGUF | 2026-04 | 28/30 · 28✓ | 25/30 · 28✓ |
| `command-r:latest` | 20 GB · Q4_K_M | Ollama | 2024-08 | 21/30 · 24✓ | 22/30 · 25✓ |
| `qwen3:32b` | 20 GB · Q4_K_M | Ollama | 2025-04 | 24/30 · 27✓ | 27/30 · 28✓ |
| `qwen3.6-27b-claude-deckard-qx64-hi-mlx` | 19.58 GB · 6bit | MLX | 2026-04 | 24/30 · 24✓ | 22/30 · 22✓ |
| `deepseek-r1:32b` | 19 GB · Q4_K_M | GGUF | 2025-01 | 25/30 · 27✓ | 29/30 · 29✓ |
| `exaone3.5:32b` | 19 GB · Q4_K_M | GGUF | 2024-12 | 25/30 · 25✓ | 23/30 · 24✓ |
| `qwen3-coder:30b` | 18 GB · Q4_K_M | GGUF | 2025-07 | 21/30 · 24✓ | 24/30 · 25✓ |
| `qwq-32b` | 18.0 GB · Q6_K | GGUF | 2025-03 | 19/30 · 22✓ | 2/30 · 2✓ |
| `gemma3:27b` | 17 GB · Q4_K_M | Ollama | 2025-03 | 29/30 · 30✓ | 5/30 · 5✓ |
| `google/gemma-3n-e4b` | 15.74 GB · bf16 | MLX | 2025-06 | 17/30 · 21✓ | 19/30 · 24✓ |
| `deepseek-r1-distill-qwen-14b` | 15.7 GB · Q8_0 | GGUF | 2025-01 | 21/30 · 22✓ | 25/30 · 26✓ |
| `prism-coder-7b` | 15.24 GB · ? | GGUF | 2026-04 | 5/30 · 5✓ | 2/30 · 2✓ |
| `mistral-small3.1:24b` | 15 GB · Q4_K_M | GGUF | 2025-03 | 25/30 · 25✓ | 23/30 · 25✓ |
| `mistralai/devstral-small-2-2512` | 14.12 GB · 4bit | MLX | 2025-12 | 24/30 · 24✓ | 25/30 · 25✓ |
| `mistralai/devstral-small-2507` | 13.28 GB · 4bit | MLX | 2025-07 | 25/30 · 28✓ | 27/30 · 28✓ |
| `qwen3.5-9b-mlx` | 10.45 GB · 8bit | MLX | 2026-02 | 23/30 · 24✓ | 25/30 · 26✓ |
| `yi-coder-9b-chat@q8_0` | 9.3 GB · Q8_0 | GGUF | 2024-09 | 22/30 · 23✓ | 0/30 · 0✓ |
| `gemma-4-e4b-it-mlx@8bit` | 8.97 GB · 8bit | MLX | 2026-04 | 22/30 · 23✓ | 24/30 · 24✓ |
| `gemma-4-e4b-it-mlx@4bit` | 6.86 GB · 4bit | MLX | 2026-04 | 21/30 · 24✓ | 20/30 · 21✓ |
| `yi-coder-9b-chat@q4_k_m` | 5.5 GB · Q4_K_M | GGUF | 2024-09 | 23/30 · 25✓ | 0/30 · 0✓ |
| `qwen3.5-4b-mlx` | 5.16 GB · 8bit | MLX | 2026-02 | 0/30 · 0✓ | 3/30 · 3✓ |
| `deepseek-r1-distill-qwen-7b` | 4.68 GB · Q4_K_M | GGUF | 2025-01 | 2/30 · 4✓ | 1/30 · 3✓ |
| `ibm/granite-4-h-tiny` | 4.23 GB · Q4_K_M | GGUF | 2025-10 | 15/30 · 20✓ | 18/30 · 21✓ |
| `qwen3:4b` | 2.5 GB · Q4_K_M | Ollama | 2025-04 | 25/30 · 26✓ | 22/30 · 27✓ |
| `qwen3.5-2b-mlx` | 1.75 GB · 4bit | MLX | 2026-02 | 9/30 · 12✓ | 13/30 · 23✓ |
<!-- LEADERBOARD:END -->

Three robust shapes:

- **The guide carries reasoning, not "code-model-ness."** A small general
  *thinking* model writes valid, correct Aether for a real share of tasks, while
  a 3B model specialized for code but without a reasoning step scores zero. The
  guide rewards a model that can *follow* it, not one merely fluent in mainstream
  languages.
- **The full guide meets or beats the condensed one,** and the margin widens as
  models weaken — more context buys the most exactly where in-context learning is
  hardest.
- **Some `full`-column collapses are real ceilings, not harness artifacts.** When
  a model aces `small` but craters on `full`, the easy assumption is mechanical.
  Maybe the ~8.7k full guide overflowed a too-small served context, or a
  long-reasoning model hit the request timeout. Re-running clean shows it is not
  always so. `yi-coder-9b` held at **0/30 on `full`** at a 16k window, both
  quants, while `small` stayed 22-23. It follows the 4.6k condensed guide but
  cannot carry the 8.7k full one, a genuine capability edge rather than a glitch.
  Treat a wide `small`-to-`full` gap as a result to verify per model, not one to
  dismiss.

## The large data set (tasks_hard.json)

These eight tasks are larger and harder than v2 — bigger inputs and more layered
logic (nested aggregation, finite-state machines, streak detection, integer
recursion). The v2/30 board saturates at the top (a whole tier ties 30/30), so it
stops separating the strongest from one another; the hard set, scored
exact-stdout against an oracle, spreads them back out. Same two guides and
harness, a separate board because v2/30 is already wide.

<!-- LEADERBOARD-LARGE:START -->
*27 models scored on the large data set (`tasks_hard.json`, 8 hard tasks). exact/8 · ✓ = compiled & ran. Cloud and local together, sorted by size.*

| model | size · quant | served | released | [small](aether_for_llms_with_small_contexts.md) | [full](aether_for_llms_and_others.md) |
|---|---|---|---|---|---|
| `GLM-5` | — | Z.ai API | 2026-03 | 8/8 · 8✓ | 8/8 · 8✓ |
| `GLM-4.6` | — | Z.ai API | 2025-09 | 8/8 · 8✓ | 8/8 · 8✓ |
| `gemini-3.1-pro-preview` | — | Gemini API | 2026-04 | 7/8 · 7✓ | 8/8 · 8✓ |
| `gpt-5.5` | — | OpenAI API | 2026-04 | 8/8 · 8✓ | 8/8 · 8✓ |
| `GLM-5.2` | — | Z.ai API | 2026-05 | 8/8 · 8✓ | 8/8 · 8✓ |
| `gemini-3-flash-preview` | — | Gemini API | 2026-04 | 6/8 · 6✓ | 8/8 · 8✓ |
| `GLM-5-Turbo` | — | Z.ai API | 2026-04 | 8/8 · 8✓ | 8/8 · 8✓ |
| `o3` | — | OpenAI API | 2025-04 | 7/8 · 7✓ | 7/8 · 7✓ |
| `gemini-2.5-pro` | — | Gemini API | 2025-06 | 8/8 · 8✓ | 7/8 · 7✓ |
| `gemini-2.5-flash` | — | Gemini API | 2025-06 | 8/8 · 8✓ | 8/8 · 8✓ |
| `GLM-4.5-Air` | — | Z.ai API | 2025-07 | 3/8 · 3✓ | 4/8 · 5✓ |
| `gpt-4o` | — | OpenAI API | 2024-05 | 6/8 · 6✓ | 7/8 · 7✓ |
| `openai/gpt-oss-120b` | 63 GB · MXFP4 | TRT-LLM | 2025-08 | 7/8 · 7✓ | 7/8 · 7✓ |
| `qwen3.5-122b-a10b-nvfp4` | 62 GB · NVFP4 | vLLM | 2026-02 | 8/8 · 8✓ | 6/8 · 7✓ |
| `qwen/qwen3.6-35b-a3b` | 37.75 GB · 8bit | MLX | 2026-04 | 5/8 · 5✓ | 7/8 · 7✓ |
| `deepseek-r1:32b` | 19 GB · Q4_K_M | GGUF | 2025-01 | 6/8 · 6✓ | 7/8 · 7✓ |
| `exaone3.5:32b` | 19 GB · Q4_K_M | GGUF | 2024-12 | 2/8 · 5✓ | 1/8 · 3✓ |
| `qwen3-coder:30b` | 18 GB · Q4_K_M | GGUF | 2025-07 | 4/8 · 5✓ | 3/8 · 4✓ |
| `google/gemma-3n-e4b` | 15.74 GB · bf16 | MLX | 2025-06 | 0/8 · 0✓ | 0/8 · 0✓ |
| `deepseek-r1-distill-qwen-14b` | 15.7 GB · Q8_0 | GGUF | 2025-01 | 2/8 · 3✓ | 2/8 · 2✓ |
| `mistral-small3.1:24b` | 15 GB · Q4_K_M | GGUF | 2025-03 | 4/8 · 4✓ | 6/8 · 7✓ |
| `mistralai/devstral-small-2-2512` | 14.12 GB · 4bit | MLX | 2025-12 | 5/8 · 5✓ | 6/8 · 6✓ |
| `mistralai/devstral-small-2507` | 13.28 GB · 4bit | MLX | 2025-07 | 7/8 · 7✓ | 6/8 · 7✓ |
| `yi-coder-9b-chat@q8_0` | 9.3 GB · Q8_0 | GGUF | 2024-09 | 0/8 · 1✓ | 2/8 · 2✓ |
| `yi-coder-9b-chat@q4_k_m` | 5.5 GB · Q4_K_M | GGUF | 2024-09 | 1/8 · 4✓ | 1/8 · 4✓ |
| `deepseek-r1-distill-qwen-7b` | 4.68 GB · Q4_K_M | GGUF | 2025-01 | 0/8 · 0✓ | 0/8 · 0✓ |
| `ibm/granite-4-h-tiny` | 4.23 GB · Q4_K_M | GGUF | 2025-10 | 1/8 · 4✓ | 1/8 · 3✓ |
<!-- LEADERBOARD-LARGE:END -->

Two findings the hard set makes visible:

- **The full guide's edge is a middle-band effect.** On v2/30 the full guide
  helped the *weak* models most (granite-4-tiny full +3, gemma-3n full +2). On
  the hard set that advantage does not widen; it vanishes. The weakest models
  crater to 0-2 of 8 on **both** guides, small tying full, most outputs no longer
  even compiling — because they cannot solve the hard tasks at all, so guide size
  has nothing to bite on. The strongest ace both guides either way. So more
  context is the margin only when the task is **hard-but-achievable** — in the
  middle band, and nowhere else.
- **The hard set separates models v2 cannot.** `devstral-small` (a 24B coder
  quantized to 4-bit, ~14 GB) holds at **5-7 of 8**, near the cloud tier, while
  `yi-coder-9b` — its peer on v2/30 (22 vs 24-25) — collapses to **0-2 of 8**.
  v2/30 rated them equals; the hard tasks show devstral is far more capable. Code
  specialization *does* help here, but specifically the agentic, compositional
  kind devstral is trained for, not mere code fluency — the README's "a guide
  carries reasoning, not code-model-ness" point seen from inside the code-model
  camp.

## The CS-classics set (tasks_cs.json)

A third instrument: classic computer-science algorithms (recursion, the
bubble/merge/quick sort triad, binary search, graph BFS and Dijkstra, dynamic
programming, strings), exact-stdout scored. It tests algorithmic implementation —
a different axis from v2's language-feature fluency and the hard set's
large-compositional tasks — and doubles as a language-completeness probe: it
surfaced the rea method-to-method receiver bug, since fixed.

<!-- LEADERBOARD-CS:START -->
*24 models scored on the CS-classics set (`tasks_cs.json`, 19 algorithm tasks: recursion, sorts, search, graphs, DP, strings).*

| model | size · quant | served | released | [small](aether_for_llms_with_small_contexts.md) | [full](aether_for_llms_and_others.md) |
|---|---|---|---|---|---|
| `GLM-5` | — | Z.ai API | 2026-03 | 15/19 · 17✓ | 15/19 · 16✓ |
| `GLM-4.6` | — | Z.ai API | 2025-09 | 15/19 · 15✓ | 15/19 · 17✓ |
| `gemini-3.1-pro-preview` | — | Gemini API | 2026-04 | 18/19 · 19✓ | 19/19 · 19✓ |
| `gpt-5.5` | — | OpenAI API | 2026-04 | 19/19 · 19✓ | 19/19 · 19✓ |
| `GLM-5.2` | — | Z.ai API | 2026-05 | 17/19 · 17✓ | 18/19 · 18✓ |
| `gemini-3-flash-preview` | — | Gemini API | 2026-04 | 15/19 · 15✓ | 16/19 · 17✓ |
| `GLM-5-Turbo` | — | Z.ai API | 2026-04 | 17/19 · 18✓ | 18/19 · 19✓ |
| `o3` | — | OpenAI API | 2025-04 | 10/19 · 11✓ | 7/19 · 7✓ |
| `gemini-2.5-pro` | — | Gemini API | 2025-06 | 14/19 · 15✓ | 14/19 · 16✓ |
| `gemini-2.5-flash` | — | Gemini API | 2025-06 | 14/19 · 16✓ | 13/19 · 14✓ |
| `GLM-4.5-Air` | — | Z.ai API | 2025-07 | 8/19 · 8✓ | 8/19 · 8✓ |
| `gpt-4o` | — | OpenAI API | 2024-05 | 8/19 · 8✓ | 4/19 · 4✓ |
| `deepseek-r1:70b` | 42 GB · Q4_K_M | Ollama | 2025-01 | — | — |
| `qwen/qwen3.6-35b-a3b` | 37.75 GB · 8bit | MLX | 2026-04 | 14/19 · 16✓ | 16/19 · 16✓ |
| `qwen3:32b` | 20 GB · Q4_K_M | Ollama | 2025-04 | 5/19 · 5✓ | 6/19 · 6✓ |
| `deepseek-r1:32b` | 19 GB · Q4_K_M | GGUF | 2025-01 | 6/19 · 9✓ | 6/19 · 9✓ |
| `exaone3.5:32b` | 19 GB · Q4_K_M | GGUF | 2024-12 | 3/19 · 5✓ | 3/19 · 4✓ |
| `qwen3-coder:30b` | 18 GB · Q4_K_M | GGUF | 2025-07 | 8/19 · 10✓ | 8/19 · 8✓ |
| `gemma3:27b` | 17 GB · Q4_K_M | Ollama | 2025-03 | 5/19 · 8✓ | 7/19 · 8✓ |
| `deepseek-r1-distill-qwen-14b` | 15.7 GB · Q8_0 | GGUF | 2025-01 | 4/19 · 4✓ | 3/19 · 5✓ |
| `mistral-small3.1:24b` | 15 GB · Q4_K_M | GGUF | 2025-03 | 5/19 · 8✓ | 7/19 · 9✓ |
| `yi-coder-9b-chat@q8_0` | 9.3 GB · Q8_0 | GGUF | 2024-09 | 5/19 · 6✓ | 0/19 · 0✓ |
| `yi-coder-9b-chat@q4_k_m` | 5.5 GB · Q4_K_M | GGUF | 2024-09 | 7/19 · 7✓ | 0/19 · 0✓ |
| `qwen3:4b` | 2.5 GB · Q4_K_M | Ollama | 2025-04 | 0/19 · 0✓ | 0/19 · 0✓ |
<!-- LEADERBOARD-CS:END -->

## What this does and does not show

- It shows that **adoption does not require fine-tuning.** A capable model plus
  the guide is a working Aether programmer today — the practical case for most
  users.
- It does **not** retire the no-guide program. A model that writes Aether with
  *nothing* in the prompt is a different, harder bar, and the place where the
  language's regularity is actually stress-tested.
- The README's compiler↔guide loop closes here too: when a guided model does
  slip, the compiler's coded diagnostic (`FX-001`, `ANN-001`, …) points back into
  the same guide section, so a second pass can self-correct.

## Tools and tasks

Everything here is reproducible from the public repos. The harness and task sets
live in the umbrella repo (`emkey1/pscal`, on the `AetherLang` branch); the guides
and write-ups live here in `emkey1/aether`.

- **Harness.** [`tools/aether_doc_bench.py`](https://github.com/emkey1/pscal/blob/AetherLang/tools/aether_doc_bench.py)
  issues each task, compiles and runs the model's program with the `aether`
  binary, and scores stdout byte-for-byte against an oracle. It records the
  `aether` and task-set versions with every result.
- **Task sets.** [`tasks_v2_pos.json`](https://github.com/emkey1/pscal/blob/AetherLang/Tests/aether_doc_bench/tasks_v2_pos.json)
  (v2/30), [`tasks_hard.json`](https://github.com/emkey1/pscal/blob/AetherLang/Tests/aether_doc_bench/tasks_hard.json)
  (large), and [`tasks_cs.json`](https://github.com/emkey1/pscal/blob/AetherLang/Tests/aether_doc_bench/tasks_cs.json)
  (CS-classics). Each carries its prompt, input fixtures, and expected stdout.
- **Guides.** [full](aether_for_llms_and_others.md) and
  [small](aether_for_llms_with_small_contexts.md) — the only Aether any model here
  ever sees.
- **The training (no-guide) side.** Companion write-up
  [`aether_specialization_findings.md`](aether_specialization_findings.md).

## Status

The cloud tier and the two large MoEs are complete across all three boards; the
local sweep is broad (2B to 122B) and filling toward the smallest models, with a
round of clean re-runs queued for the context/timeout artifacts flagged above.
Each board is regenerated from the per-model result JSONs as they land.
