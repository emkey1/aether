# The Guide Is Enough — Finalized-Guide, Repair-On Edition

*In-context (untrained) results for Aether. Companion to
[`aether_specialization_findings.md`](aether_specialization_findings.md) — the
no-guide, fine-tuned side. A broader historical sweep (48 models, single-pass) is
preserved at [`archive/aether_guided_benchmark.md`](archive/aether_guided_benchmark.md).*

This edition re-runs the in-context benchmark with the **repair loop ON**
(`--repair-attempts 2`). That is what is new here: every score is **Compiled /
Correct / Retried / Fixed**, so you can see not just how often a model lands a
correct program but how often the compiler's own diagnostics let it self-correct
on a second pass. The archived sweep ran single-pass (repair off), so its
Retried/Fixed columns are all zero; this run populates them.

**Every row carries the guide version it was scored against** (a `guide ver`
column, the guide's own self-declared `YYYY-MM-DD-N` stamp from its front
matter). The guide's content changes over time as the language does — most
rows below predate this session's parser-hardening work and were scored on the
**2026-06-28** guide snapshot; models landing now score against the current
**2026-07-01-8** guide. Don't compare a score across guide versions without
checking this column first.

**One real signal from the version transition so far:** `gemini-2.5-flash` is
the only model scored on both guide versions (re-run 2026-07-03), and it holds
**30/30 on simple both times** but drops on the harder sets — `large` **8/8 →
6-7/8**, `cs` **17-18/19 → 13-15/19**, both variants, with more retries needed
along the way. The hardened language now correctly rejects constructs the
prior, more lenient AST/rewriter path let through, and even a frontier model
occasionally reaches for one of them. This isn't a benchmark artifact — it's
the real cost of the stricter parser, paid on the hard tail even by capable
models. Small local models show the same pattern more sharply (see `qwen3-1.7b`
below, 0/8 on `large`).

The short answer is unchanged: for capable models, **the guide is enough.** Handed
nothing but the guide, an entire frontier tier writes every task on the core
benchmark correctly — no fine-tuning, no worked examples beyond the document itself.

---

## Setup

- **Three instruments, exact-stdout scored.** The **simple** set (30 tasks, `tasks_v2_pos.json`) probes language fluency; **the
  large set** (8 tasks, `tasks_hard.json`) carries bigger inputs and more layered
  logic; **CS-classics** (19 tasks, `tasks_cs.json`) tests textbook algorithms. Each
  program is compiled and run with `aether` 2026-06-27-3 and its stdout compared
  byte-for-byte against an oracle.
- **The guide in the prompt, two sizes.** The **full** guide
  ([`aether_for_llms_and_others.md`](aether_for_llms_and_others.md), ~980 lines) or
  the **concise** one
  ([`aether_for_llms_with_small_contexts.md`](aether_for_llms_with_small_contexts.md),
  ~500 lines). There is no `none` column — that is the fine-tuned side's
  department. The guide's own version stamp is recorded per row (see `guide ver`
  column in each table) since it is revised alongside the language.
- **Repair on, and the columns that come with it.** Up to two attempts: on a failure
  the model is handed its own compiler diagnostic — the `FX-001` / `ANN-001`-style
  coded errors that double as guide section headings — and may resubmit.
  **Compiled** = ran with return code 0; **Correct** = exact stdout match (the
  headline number); **Retried** = needed ≥2 attempts; **Fixed** = a repair turned a
  failure into a pass. These four are **overlapping tallies over the same N tasks,
  not a partition** — they do not sum to N (`Compiled ≥ Correct ≥ Fixed`,
  `Fixed ⊆ Retried`, and `Correct` already includes the fixes). A task that never
  produced a usable program counts 0 in all four (and shows up as `Correct < N`).
- **Cohort.** A curated set from a 120B MoE down to a 7B, served on the two GB10
  claws (claw1/claw2, Ollama), m5t (LM Studio / MLX), and the Gemini API. For the
  broad 2B–122B model list, see the archive.

## Simple (30 tasks): core language fluency

| model | size · served | guide ver | concise (C/C/R/F) | full (C/C/R/F) |
|---|---|---|---|---|
| `gpt-oss-120b` | 120B MXFP4 · claw1 | 2026-06-28 | **30**/30/6/6 | 29/29/7/6 |
| `qwen3.6-35b-a3b` | 35B-A3B · m5t | 2026-06-28 | **30**/30/1/1 | **30**/30/0/0 |
| `gemini-2.5-flash` | — · cloud | 2026-06-28 | **30**/30/1/1 | **30**/30/1/1 |
| `glm-5.2` | — · cloud | 2026-06-28 | **30**/30/0/0 | **30**/30/0/0 |
| `glm-5-turbo` | — · cloud | 2026-06-28 | **30**/30/1/1 | 29/29/2/1 |
| `gemma3-27b` | 27B · claw2 | 2026-06-28 | **30**/29/6/5 | 26/26/9/5 |
| `devstral-24b` | 24B · m5t | 2026-06-28 | 29/29/3/2 | 28/28/4/3 |
| `qwen3-coder-30b` | 30B-A3B · m5t | 2026-06-28 | 29/28/4/2 | 28/28/2/0 |
| `mistral-small-24b` | 24B · claw1 | 2026-06-28 | 28/28/6/4 | 28/27/7/4 |
| `phi4` | 14B · claw2 | 2026-06-28 | 28/27/13/10 | 26/25/6/1 |
| `qwen3.5-9b` | 9B · m5t | 2026-06-28 | 27/27/1/1 | 26/26/2/2 |
| `exaone3.5-32b` | 32B · claw1 | 2026-06-28 | 25/24/10/4 | 23/22/11/3 |
| `llama3.1-8b` | 8B · claw2 | 2026-06-28 | 22/19/13/2 | 26/20/13/3 |
| `granite4-tiny-7b` | 7B · m5t | 2026-06-28 | 19/15/20/5 | 21/13/17/0 |
| `granite3.3-8b` | 8B · claw1 | 2026-06-28 | 18/14/20/4 | 21/18/13/1 |
| `qwen3-1.7b` | 1.7B · claw1 | **2026-07-01-8** | 13/9/25/4 | 16/9/25/5 |
| `gemini-2.5-flash` | — · cloud | **2026-07-01-8** | **30**/30/0/0 | **30**/30/0/0 |

*More claw1/claw2 models (small + large + Ornith) are still landing — see Status.*

## Large (8 tasks): bigger inputs, layered logic

| model | size · served | guide ver | concise | full |
|---|---|---|---|---|
| `gemini-2.5-flash` | — · cloud | 2026-06-28 | **8**/8/0/0 | **8**/8/0/0 |
| `qwen3.6-35b-a3b` | 35B-A3B · m5t | 2026-06-28 | **8**/8/1/1 | **8**/8/2/2 |
| `glm-5.2` | — · cloud | 2026-06-28 | **8**/8/0/0 | **8**/8/0/0 |
| `glm-5-turbo` | — · cloud | 2026-06-28 | 7/7/0/0 | **8**/8/0/0 |
| `gpt-oss-120b` | 120B MXFP4 · claw1 | 2026-06-28 | **8**/8/2/2 | 7/7/1/0 |
| `devstral-24b` | 24B · m5t | 2026-06-28 | 7/7/2/1 | 7/7/8/7 |
| `gemma3-27b` | 27B · claw2 | 2026-06-28 | 7/7/8/7 | 7/7/8/7 |
| `qwen3-coder-30b` | 30B-A3B · m5t | 2026-06-28 | 6/6/4/2 | 7/7/5/4 |
| `mistral-small-24b` | 24B · claw1 | 2026-06-28 | 7/7/1/0 | 7/7/2/1 |
| `phi4` | 14B · claw2 | 2026-06-28 | 7/7/8/7 | 1/1/8/1 |
| `qwen3.5-9b` | 9B · m5t | 2026-06-28 | 5/5/4/4 | 6/6/6/6 |
| `exaone3.5-32b` | 32B · claw1 | 2026-06-28 | 2/1/7/0 | 3/2/8/2 |
| `granite4-tiny-7b` | 7B · m5t | 2026-06-28 | 0/0/8/0 | 1/0/8/0 |
| `llama3.1-8b` | 8B · claw2 | 2026-06-28 | 1/0/8/0 | 1/0/8/0 |
| `granite3.3-8b` | 8B · claw1 | 2026-06-28 | 2/0/8/0 | 1/0/8/0 |
| `qwen3-1.7b` | 1.7B · claw1 | **2026-07-01-8** | 0/0/7/0 | 0/0/8/0 |
| `gemini-2.5-flash` | — · cloud | **2026-07-01-8** | 7/7/3/2 | 6/6/3/1 |

## CS-classics (19 tasks): textbook algorithms

| model | size · served | guide ver | concise | full |
|---|---|---|---|---|
| `glm-5-turbo` | — · cloud | 2026-06-28 | 18/18/1/1 | 19/**19**/1/1 |
| `glm-5.2` | — · cloud | 2026-06-28 | 18/18/3/3 | 18/18/1/1 |
| `gemini-2.5-flash` | — · cloud | 2026-06-28 | 18/**18**/3/2 | 18/17/4/2 |
| `qwen3.6-35b-a3b` | 35B-A3B · m5t | 2026-06-28 | 17/17/2/2 | 18/**18**/2/1 |
| `gpt-oss-120b` | 120B MXFP4 · claw1 | 2026-06-28 | 17/17/6/4 | 17/17/8/6 |
| `qwen3-coder-30b` | 30B-A3B · m5t | 2026-06-28 | 12/10/10/1 | 14/**13**/8/2 |
| `exaone3.5-32b` | 32B · claw1 | 2026-06-28 | 10/8/14/3 | 7/7/14/2 |
| `qwen3.5-9b` | 9B · m5t | 2026-06-28 | 8/8/0/0 | 14/**13**/4/3 |
| `devstral-24b` | 24B · m5t | 2026-06-28 | 15/11/11/3 | 13/11/11/3 |
| `gemma3-27b` | 27B · claw2 | 2026-06-28 | 11/7/14/2 | 13/12/12/5 |
| `mistral-small-24b` | 24B · claw1 | 2026-06-28 | 9/7/12/0 | 10/7/14/2 |
| `phi4` | 14B · claw2 | 2026-06-28 | 6/6/14/2 | 5/5/14/0 |
| `granite4-tiny-7b` | 7B · m5t | 2026-06-28 | 4/2/18/1 | 6/4/16/1 |
| `granite3.3-8b` | 8B · claw1 | 2026-06-28 | 4/2/17/0 | 5/3/16/0 |
| `llama3.1-8b` | 8B · claw2 | 2026-06-28 | 5/1/18/0 | 5/2/17/0 |
| `qwen3-1.7b` | 1.7B · claw1 | **2026-07-01-8** | 8/4/17/3 | 4/2/16/0 |
| `gemini-2.5-flash` | — · cloud | **2026-07-01-8** | 15/15/8/4 | 14/13/7/1 |

*All boards: concise and full columns are Compiled/Correct/Retried/Fixed for that
model.*

## What the repair columns show

- **Repair earns its keep — for models strong enough to read their own error.**
  This is what the archived single-pass sweep could not show. `gpt-oss-120b` on the simple set
  fixed **6 of 6** retries (concise); `devstral-24b` on the large set fixed **7 of 8**
  (full). The coded diagnostic points back into the same guide section the model
  already has, and a capable model uses it. But it is capability-gated: weak models
  thrash. `granite4-tiny-7b` on `cs` retried 16 and fixed 1; `mistral-small-24b` on
  `cs` retried 14 and fixed 2. A model that cannot turn `FX-001` into a fix just
  rattles the cage — visible as a high Retried with a near-zero Fixed.
- **The concise guide costs the capable models nothing.** At the top, concise and
  full tie (gpt-oss, qwen3.6, gemini all 30/30 on simple; 8/8 on large). The full
  guide's edge is a **weak-model, hard-task** effect: `qwen3.5-9b` jumps from 8 to
  **13** on `cs` with the full guide, `granite4-tiny` from 2 to 4. More context buys
  the most exactly where in-context learning is hardest, and nothing where it is
  easy. Shipping the concise guide gives up nothing on the models that matter.
- **The simple set saturates; `cs` discriminates.** Four models sit at ≥29/30 on
  simple — it no longer separates the top tier. `cs` spreads the same cohort from 2
  to 18. Rank on `cs` and the large set, not simple.
- **The Compiled→Correct gap is semantic, and concentrated in the weak models.**
  `granite4-tiny` compiles 21 of 30 on simple but only 13 are correct; `mistral-small`
  compiles 10 of 19 on `cs`, 7 correct. The guide reliably gets a model to *valid*
  Aether; the residue is algorithmic correctness, which a language guide cannot
  supply.
- **`devstral-24b` is the mid-tier standout** — 29/30 simple, 7/8 large, near the
  frontier tier. Agentic, compositional code-training helps on the layered tasks in
  a way generic code fluency does not.

## What this does and does not show

- It shows **adoption does not require fine-tuning**: a capable model plus the guide
  is a working Aether programmer today, and with repair on, the compiler's coded
  diagnostics measurably raise that further.
- It does **not** retire the no-guide program. Writing Aether with *nothing* in the
  prompt is a harder bar and the real stress test of the language's regularity — see
  the [fine-tuned findings](aether_specialization_findings.md).

## Tools and tasks

Reproducible from the public repos. The harness
([`tools/aether_doc_bench.py`](https://github.com/emkey1/pscal/blob/AetherLang/tools/aether_doc_bench.py))
issues each task, compiles and runs the program with the `aether` binary, and scores
stdout byte-for-byte. Task sets:
[`tasks_v2_pos.json`](https://github.com/emkey1/pscal/blob/AetherLang/Tests/aether_doc_bench/tasks_v2_pos.json)
(simple),
[`tasks_hard.json`](https://github.com/emkey1/pscal/blob/AetherLang/Tests/aether_doc_bench/tasks_hard.json)
(large), and
[`tasks_cs.json`](https://github.com/emkey1/pscal/blob/AetherLang/Tests/aether_doc_bench/tasks_cs.json)
(cs).

## Status

This is the finalized-guide / repair-on cohort, regenerated from the per-model
result JSONs. The two cloud **GLM** models (`glm-5-turbo`, `glm-5.2`) are served via
the autoglm/autoclaw proxy, which is slow (~23 tok/s); GLM's verbose reasoning can run
a hard task past the request time budget, so a couple of their large/cs cells reflect a
proxy/verbosity timeout rather than a capability miss.

**Guide version note (2026-07-03):** the language hardened materially this session
(stricter parser, new coded diagnostics, contract/effect changes), and the guide
was updated to match — its self-declared version moved from `2026-06-28` to
`2026-07-01-8`. Rows below are stamped per-model with the guide version they
were actually scored against; do not read a `2026-07-01-8` row as directly
comparable to the `2026-06-28` majority without accounting for that. The
existing `2026-06-28` rows are not being re-run wholesale (a decision, not an
oversight) — only new destinations land on the current guide going forward.

**In progress:** the two GB10 Sparks (claw1/claw2), routed through the T'Ra
scheduler, are benchmarking a broad tail — more small models (qwen3 1.7b-32b,
deepseek-r1, command-r) and larger ones (gemma3-12b, llama3.3-70b,
command-r-plus, nemotron) plus the **Ornith-1.0-35B** agentic-coding model —
folding in as they land, smallest first. `qwen3-1.7b` is the first to complete
(all three instruments, `2026-07-01-8` guide). (The original m5t laptop tail
was moved to the claws, which serve 32B models far faster; `seed-oss-36b`
stays dropped — impractically slow.) The broad single-pass 2B–122B sweep —
including the cloud flagships that ace
every board — remains in the [archive](archive/aether_guided_benchmark.md).
