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

**Follow-up re-run (2026-07-04-2, post tuple-return structural fix):** re-ran
`gemini-2.5-flash` again after the tuple-return lowering was reworked to a
reentrant record-by-value return (see CHANGELOG). Scores are flat overall
(`simple` 30/30 both guides; `large` 13/16 total, same as 07-01-8, just one
task flipping each direction; `cs` net +2 on the full guide, 13→15/19).
Checked every task whose pass/fail flipped between 07-01-8 and 07-04-2 against
its generated source: **none use tuple-return syntax**, so this run neither
confirms nor refutes the tuple fix — it's ordinary run-to-run model/API
variance (temperature 0.2, not deterministic) riding on top of an otherwise
unchanged compiler surface for these tasks. The task manifests have no
tuple-return case today; a dedicated task would be needed to exercise that
path directly.

The short answer is unchanged: for capable models, **the guide is enough.** Handed
nothing but the guide, an entire frontier tier writes every task on the core
benchmark correctly — no fine-tuning, no worked examples beyond the document itself.

---

## cs-aug4-matched guided board (current)

**Corpus/suite:** No corpus (in-context, untrained) — same 8-model cohort as
[the `cs-aug4` fine-tuned board](aether_specialization_findings.md#cs-aug4-current-primary-board):
`deepseek6.7b`, `qwen3-8b-nothink`, `qwen35-9b`, `qwen25-14b`, `mistral24b`,
`qwen36-27b`, `qwen3-coder30b-a3b`, `qwen36-35b-a3b`, plus two cloud reference models
(`gemini-2.5-flash`, `gemini-2.5-flash-lite`). Task suite `tasks_v2_pos.json` /
`tasks_hard.json` v`2026-07-15-1` (simple **35**, large **9**) and
`tasks_cs.json` v`2026-06-23-1` (cs, 19) — the current manifest, **not
comparable cell-by-cell** to the 30/8/19 tables below. Guide `2026-07-15-2`
([full](aether_for_llms_and_others.md)/[concise](aether_for_llms_with_small_contexts.md)),
`aether` `2026-07-09-1` (commit `9a44e67`) for the local models; the cloud
pair ran against the same pinned local binary for consistency. `--docs
full,small --repair-attempts 2`.

**Seed pinning:** the 8 local models are seed-pinned (`seed: 42`, vLLM/
llama.cpp) — see [[harness-no-seed-pinning]]. **The two Gemini rows are not**
(their destinations config predates the fix and the Gemini API's own `seed`
parameter is best-effort, not guaranteed deterministic like the local
OpenAI-compatible servers) — read any close Gemini-vs-local delta as
directional, not controlled.

**Two serving-side issues surfaced and were fixed before scoring:**
- `deepseek6.7b-base`'s full-guide variant is **omitted, not zero** — this
  model has a hard `max_position_embeddings: 16384` ceiling and the full
  guide alone is ~18,405 tokens; only the concise/small guide is runnable
  (see [[tokenizer-serving-gotcha]]).
- `qwen35-9b-base`'s large-suite, full-guide run initially came back 1/9
  compiled — **not** a model-capability ceiling: `Qwen3.5-9B` supports
  262144 tokens per its own config, but the shared vLLM serving container was
  launched with a fixed `--max-model-len 32768`, and this model's tokenizer
  happened to encode the large-suite-plus-full-guide prompt at ≥16,769
  tokens — combined with the harness's fixed `max_output_tokens: 16000`
  request, that's ≥32,769, just over the server's ceiling. Every other model
  on this board stayed under it for the same guide text (their tokenizers
  are simply more compact on this document). Bumped `--max-model-len` to
  40960 and re-ran just that one suite; real scores below. Left as a
  cautionary note for future models on this harness — a passing run for the
  rest of the cohort does not guarantee a new model's tokenizer will fit the
  same fixed ceiling.

### Simple (35 tasks)

| model | small guide | full guide |
|---|---|---|
| `gemini-2.5-flash` | 35/**35**/0/0 | 35/**35**/1/1 |
| `qwen36-27b` (no-think) | 35/**35**/0/0 | 35/**35**/0/0 |
| `qwen3-8b-nothink` | 32/32/4/2 | 34/34/2/1 |
| `qwen36-35b-a3b` (no-think) | 35/**35**/1/1 | 34/34/1/0 |
| `mistral24b` | 31/30/10/5 | 34/33/4/2 |
| `qwen35-9b` (no-think) | 32/31/6/2 | 33/33/4/2 |
| `gemini-2.5-flash-lite` | 33/32/5/2 | 33/32/4/1 |
| `qwen3-coder30b-a3b` | 29/27/11/3 | 32/32/3/1 |
| `qwen25-14b` | 31/31/4/0 | 33/31/4/0 |
| `deepseek6.7b`† | 30/26/9/0 | — |

### Large (9 tasks)

| model | small guide | full guide |
|---|---|---|
| `gemini-2.5-flash` | 8/8/2/1 | 8/8/3/2 |
| `qwen36-27b` (no-think) | 9/**9**/0/0 | 9/**9**/0/0 |
| `qwen3-8b-nothink` | 9/**9**/2/2 | 9/**9**/4/4 |
| `qwen36-35b-a3b` (no-think) | 9/8/6/5 | 6/6/0/0 |
| `mistral24b` | 9/**9**/1/1 | 9/**9**/0/0 |
| `qwen35-9b` (no-think) | 9/8/8/7 | 9/5/9/5 |
| `qwen35-9b-base` (think)§ | 9/6/6/1/1 | 9/5/5/2/2 |
| `gemini-2.5-flash-lite` | 9/**9**/2/2 | 9/8/9/8 |
| `qwen3-coder30b-a3b` | 5/5/4/0 | 8/8/6/5 |
| `qwen25-14b` | 7/6/9/6 | 8/8/3/2 |
| `deepseek6.7b`†‡ | 1/0/9/0 | — |

§ `qwen35-9b-base` (think) is a **scoped, not directly comparable** follow-up
run, not part of the controlled cs-aug4-matched sweep above: same seed
(`42`), same guide (`2026-07-15-2`), same `--max-model-len 40960` bump, same
`tasks_hard.json` (`2026-07-15-1`) and `--repair-attempts 2`, but against
`aether 2026-07-19-1` (vs the board's `2026-07-09-1`) and with thinking
enabled instead of the model's default no-think mode. Cell format here is
`N/Compiled/Correct/Retried/Fixed` (5 numbers) since a meaningful fraction
of tasks never produced a program at all within the 1800s-per-attempt cap —
worth spelling out separately from `Correct` rather than folding into it.
**The finding:** thinking mode is far more likely to *time out generating
entirely* than to generate and be wrong — every task that finished compiled
correctly (`Correct == Compiled` in both columns, a 100% hit rate on
whatever it actually produced), but only 5–6 of 9 tasks finished within 30
minutes per attempt at all (vs no-think's 9/9 always compiling). The same
three tasks (`hard_expense_outliers`, `hard_account_ledger`,
`hard_sensor_streak`) timed out under **both** guide sizes, suggesting a
runaway-reasoning failure mode on those specific prompts rather than a
context-budget problem. Net effect on this suite: thinking trades away
completion rate for a cleaner "correct or absent" split, and comes out
*behind* no-think on raw task count (5–6/9 vs 5–8/9) because the timeouts
dominate. Full run: `/home/claw/guided_sweep_base/qwen35-9b-base-think_large.json`
on claw2.

### CS-classics (19 tasks)

| model | small guide | full guide |
|---|---|---|
| `gemini-2.5-flash` | 17/17/6/4 | 18/18/5/4 |
| `qwen36-27b` (no-think) | 18/17/5/3 | 18/18/0/0 |
| `qwen3-8b-nothink` | 13/13/10/4 | 13/13/7/2 |
| `qwen36-35b-a3b` (no-think) | 16/15/10/6 | 15/14/4/2 |
| `mistral24b` | 10/6/13/0 | 13/10/10/1 |
| `qwen35-9b` (no-think) | 12/9/11/1 | 11/8/11/0 |
| `gemini-2.5-flash-lite` | 14/12/9/2 | 15/14/7/2 |
| `qwen3-coder30b-a3b` | 12/11/10/2 | 15/13/10/4 |
| `qwen25-14b` | 12/10/12/3 | 11/8/11/0 |
| `deepseek6.7b`† | 8/5/14/0 | — |

*Cells are Compiled/Correct/Retried/Fixed (overlapping tallies, not a
partition), same convention as [the fine-tuned board](aether_specialization_findings.md#cs-aug4-current-primary-board):
Compiled = ran rc 0; Correct = exact stdout (bold = perfect score); Retried =
needed ≥2 attempts; Fixed = a repair turned a failure into a pass.
†`deepseek6.7b-base`'s full guide is omitted (context ceiling, see above), not
a zero. ‡`deepseek6.7b-base`'s large-suite small-guide run genuinely nearly
zeroed (1/9 compiled) — this suite's inputs are the largest of the three even
on the concise guide, and this model's 16384-token ceiling bites there too;
a real finding, not an infra artifact like the `qwen35-9b` case above.*

**Reading this against the fine-tuned board:** `qwen3-8b-nothink`'s base
model is the standout again here — it ties or beats every other local model
on `large` and is competitive on `cs`, mirroring its unexplained strength on
the no-guide board. `qwen35-9b`, added specifically to test whether a newer
Qwen generation closes that gap, does not: it sits mid-pack here too,
consistent with its no-guide result. `qwen36-27b` is the strongest local
model on this board by a clear margin, matching expectations for a 27B
dense model over 8-14B peers. Full-vs-small guide shows the same
weak-model/hard-task pattern as the archived cohort below (e.g. `mistral24b`
gains 4 `cs`-correct from the extra context; `qwen36-27b` needs none of it).

---

## Setup

- **Three instruments, exact-stdout scored.** The **simple** set (30 tasks, `tasks_v2_pos.json`) probes language fluency; **the
  large set** (8 tasks, `tasks_hard.json`) carries bigger inputs and more layered
  logic; **CS-classics** (19 tasks, `tasks_cs.json`) tests textbook algorithms. Each
  program is compiled and run with the `aether` build recorded in the row's
  `aether ver` column, and its stdout compared byte-for-byte against an oracle.
- **Two independent runs per model, not two stages of one run.** "concise guide"
  and "full guide" are two *separate* experiments — same model, same tasks, but a
  different guide document in the prompt: the **full** guide
  ([`aether_for_llms_and_others.md`](aether_for_llms_and_others.md), ~980 lines) or
  the **concise** one
  ([`aether_for_llms_with_small_contexts.md`](aether_for_llms_with_small_contexts.md),
  ~500 lines). There is no `none` column — that is the fine-tuned side's
  department. The guide's own version stamp is recorded per row (see `guide ver`
  column in each table) since it is revised alongside the language.
- **`guide ver` and `aether ver` are two different things, and they drift.**
  `guide ver` is the guide document's own self-declared `YYYY-MM-DD-N` front-matter
  stamp; `aether ver` is the actual compiler build (`aether --version`) that
  compiled and ran that row's programs. They are usually updated together, but not
  guaranteed to be — read `aether ver`, not `guide ver`, when you need to know
  exactly what compiler behavior produced a score. (A build-versioning bug once
  made several rows here report a meaningless local build timestamp instead of
  the real language version — see the note below the tables. Decoded against
  git history, every currently published row's `aether ver` does in fact match
  its `guide ver` exactly; the bug was cosmetic for this doc, not a sign of
  actual undetected drift.)
- **Repair on for both columns — there is no separate "first pass" number.**
  Every score, in *both* the concise-guide and full-guide columns, already
  includes up to two repair attempts: on a failure the model is handed its own
  compiler diagnostic — the `FX-001` / `ANN-001`-style coded errors that double
  as guide section headings — and may resubmit. So a model can score *lower*
  with the full guide than the concise one on the same task set; that isn't a
  contradiction, it just means the longer guide didn't help (or slightly hurt)
  that model on that instrument — see "What the repair columns show" below.
  **Compiled** = ran with return code 0; **Correct** = exact stdout match (the
  headline number, shown as `Correct/N` in the tables); **Retried** = needed ≥2
  attempts; **Fixed** = a repair turned a failure into a pass (shown inline as
  `(N retried, M fixed)` only when nonzero). These are **overlapping tallies
  over the same N tasks, not a partition** — they do not sum to N
  (`Compiled ≥ Correct ≥ Fixed`, `Fixed ⊆ Retried`, and `Correct` already
  includes the fixes). A task that never produced a usable program counts 0 in
  all four (and shows up as `Correct < N`).
- **Cohort.** A curated set from a 120B MoE down to a 7B, served on the two GB10
  claws (claw1/claw2, Ollama), m5t (LM Studio / MLX), and the Gemini API. For the
  broad 2B–122B model list, see the archive.

## Simple (30 tasks): core language fluency

| model | size · served | guide ver | aether ver | concise guide | full guide |
|---|---|---|---|---|---|
| `ornith-1.0-35b-nvfp4` | 35B-MoE · claw1 | **2026-07-04-2** | 2026-07-04-2 | **30/30** (1 retried, 1 fixed) | **30/30** |
| `gpt-oss-120b` | 120B MXFP4 · claw1 | 2026-06-28 | 2026-06-27-3 | **30/30** (6 retried, 6 fixed) | 29/30 (7 retried, 6 fixed) |
| `qwen3.6-35b-a3b` | 35B-A3B · m5t | 2026-06-28 | 2026-06-27-3 | **30/30** (1 retried, 1 fixed) | **30/30** |
| `gemini-2.5-flash` | — · cloud | 2026-06-28 | 2026-06-27-3 | **30/30** (1 retried, 1 fixed) | **30/30** (1 retried, 1 fixed) |
| `gemini-2.5-flash` | — · cloud | **2026-07-01-8** | 2026-07-01-8 | **30/30** | **30/30** |
| `gemini-2.5-flash` | — · cloud | **2026-07-04-2** | 2026-07-04-2 | **30/30** (1 retried, 1 fixed) | **30/30** |
| `glm-5.2` | — · cloud | 2026-06-28 | 2026-06-27-3 | **30/30** | **30/30** |
| `glm-5-turbo` | — · cloud | 2026-06-28 | 2026-06-27-3 | **30/30** (1 retried, 1 fixed) | 29/30 (2 retried, 1 fixed) |
| `glm-5-turbo` | — · cloud | **2026-07-01-8** | 2026-07-01-8 | **30/30** | **30/30** (1 retried, 1 fixed) |
| `gemma3-27b` | 27B · claw2 | 2026-06-28 | 2026-06-27-3 | 29/30 (6 retried, 5 fixed) | 26/30 (9 retried, 5 fixed) |
| `devstral-24b` | 24B · m5t | 2026-06-28 | 2026-06-27-3 | 29/30 (3 retried, 2 fixed) | 28/30 (4 retried, 3 fixed) |
| `qwen3-4b` | 4B · claw2 | **2026-07-01-8** | 2026-07-01-8 | 29/30 (5 retried, 4 fixed) | 21/30 (4 retried, 2 fixed) |
| `qwen3-coder-30b` | 30B-A3B · m5t | 2026-06-28 | 2026-06-27-3 | 28/30 (4 retried, 2 fixed) | 28/30 (2 retried, 0 fixed) |
| `mistral-small-24b` | 24B · claw1 | 2026-06-28 | 2026-06-27-3 | 28/30 (6 retried, 4 fixed) | 27/30 (7 retried, 4 fixed) |
| `phi4` | 14B · claw2 | 2026-06-28 | 2026-06-27-3 | 27/30 (13 retried, 10 fixed) | 25/30 (6 retried, 1 fixed) |
| `qwen3.5-9b` | 9B · m5t | 2026-06-28 | 2026-06-27-3 | 27/30 (1 retried, 1 fixed) | 26/30 (2 retried, 2 fixed) |
| `exaone3.5-32b` | 32B · claw1 | 2026-06-28 | 2026-06-27-3 | 24/30 (10 retried, 4 fixed) | 22/30 (11 retried, 3 fixed) |
| `llama3.1-8b` | 8B · claw2 | 2026-06-28 | 2026-06-27-3 | 19/30 (13 retried, 2 fixed) | 20/30 (13 retried, 3 fixed) |
| `granite3.3-8b` | 8B · claw1 | 2026-06-28 | 2026-06-27-3 | 14/30 (20 retried, 4 fixed) | 18/30 (13 retried, 1 fixed) |
| `granite4-tiny-7b` | 7B · m5t | 2026-06-28 | 2026-06-27-3 | 15/30 (20 retried, 5 fixed) | 13/30 (17 retried, 0 fixed) |
| `qwen3-1.7b` | 1.7B · claw1 | **2026-07-01-8** | 2026-07-01-8 | 9/30 (25 retried, 4 fixed) | 9/30 (25 retried, 5 fixed) |

*Correct/N per guide, bold = perfect score. "(N retried, M fixed)" shown only
when the repair loop was actually invoked — both columns already include
retries; there is no separate "first pass" number here (see Setup). Rows for
the same model are grouped together, oldest guide version first.*

*More claw1/claw2 models (small + large + Ornith) are still landing — see Status.*

<details>
<summary>Compiled detail — Simple</summary>

| model | guide ver | aether ver | concise guide compiled | full guide compiled |
|---|---|---|---|---|
| `gpt-oss-120b` | 2026-06-28 | 2026-06-27-3 | 30/30 | 29/30 |
| `qwen3.6-35b-a3b` | 2026-06-28 | 2026-06-27-3 | 30/30 | 30/30 |
| `gemini-2.5-flash` | 2026-06-28 | 2026-06-27-3 | 30/30 | 30/30 |
| `gemini-2.5-flash` | **2026-07-01-8** | 2026-07-01-8 | 30/30 | 30/30 |
| `gemini-2.5-flash` | **2026-07-04-2** | 2026-07-04-2 | 30/30 | 30/30 |
| `glm-5.2` | 2026-06-28 | 2026-06-27-3 | 30/30 | 30/30 |
| `glm-5-turbo` | 2026-06-28 | 2026-06-27-3 | 30/30 | 29/30 |
| `glm-5-turbo` | **2026-07-01-8** | 2026-07-01-8 | 30/30 | 30/30 |
| `gemma3-27b` | 2026-06-28 | 2026-06-27-3 | 30/30 | 26/30 |
| `devstral-24b` | 2026-06-28 | 2026-06-27-3 | 29/30 | 28/30 |
| `qwen3-4b` | **2026-07-01-8** | 2026-07-01-8 | 29/30 | 22/30 |
| `qwen3-coder-30b` | 2026-06-28 | 2026-06-27-3 | 29/30 | 28/30 |
| `mistral-small-24b` | 2026-06-28 | 2026-06-27-3 | 28/30 | 28/30 |
| `phi4` | 2026-06-28 | 2026-06-27-3 | 28/30 | 26/30 |
| `qwen3.5-9b` | 2026-06-28 | 2026-06-27-3 | 27/30 | 26/30 |
| `exaone3.5-32b` | 2026-06-28 | 2026-06-27-3 | 25/30 | 23/30 |
| `llama3.1-8b` | 2026-06-28 | 2026-06-27-3 | 22/30 | 26/30 |
| `granite3.3-8b` | 2026-06-28 | 2026-06-27-3 | 18/30 | 21/30 |
| `granite4-tiny-7b` | 2026-06-28 | 2026-06-27-3 | 19/30 | 21/30 |
| `qwen3-1.7b` | **2026-07-01-8** | 2026-07-01-8 | 13/30 | 16/30 |

</details>

## Large (8 tasks): bigger inputs, layered logic

| model | size · served | guide ver | aether ver | concise guide | full guide |
|---|---|---|---|---|---|
| `ornith-1.0-35b-nvfp4` | 35B-MoE · claw1 | **2026-07-04-2** | 2026-07-04-2 | 7/8 (3 retried, 3 fixed) | **8/8** (1 retried, 1 fixed) |
| `gemini-2.5-flash` | — · cloud | 2026-06-28 | 2026-06-27-3 | **8/8** | **8/8** |
| `gemini-2.5-flash` | — · cloud | **2026-07-01-8** | 2026-07-01-8 | 7/8 (3 retried, 2 fixed) | 6/8 (3 retried, 1 fixed) |
| `gemini-2.5-flash` | — · cloud | **2026-07-04-2** | 2026-07-04-2 | 6/8 (2 retried, 0 fixed) | 7/8 (2 retried, 1 fixed) |
| `qwen3.6-35b-a3b` | 35B-A3B · m5t | 2026-06-28 | 2026-06-27-3 | **8/8** (1 retried, 1 fixed) | **8/8** (2 retried, 2 fixed) |
| `glm-5.2` | — · cloud | 2026-06-28 | 2026-06-27-3 | **8/8** | **8/8** |
| `glm-5-turbo` | — · cloud | 2026-06-28 | 2026-06-27-3 | 7/8 | **8/8** |
| `glm-5-turbo` | — · cloud | **2026-07-01-8** | 2026-07-01-8 | **8/8** | **8/8** |
| `gpt-oss-120b` | 120B MXFP4 · claw1 | 2026-06-28 | 2026-06-27-3 | **8/8** (2 retried, 2 fixed) | 7/8 (1 retried, 0 fixed) |
| `devstral-24b` | 24B · m5t | 2026-06-28 | 2026-06-27-3 | 7/8 (2 retried, 1 fixed) | 7/8 (8 retried, 7 fixed) |
| `gemma3-27b` | 27B · claw2 | 2026-06-28 | 2026-06-27-3 | 7/8 (8 retried, 7 fixed) | 7/8 (8 retried, 7 fixed) |
| `qwen3-coder-30b` | 30B-A3B · m5t | 2026-06-28 | 2026-06-27-3 | 6/8 (4 retried, 2 fixed) | 7/8 (5 retried, 4 fixed) |
| `mistral-small-24b` | 24B · claw1 | 2026-06-28 | 2026-06-27-3 | 7/8 (1 retried, 0 fixed) | 7/8 (2 retried, 1 fixed) |
| `phi4` | 14B · claw2 | 2026-06-28 | 2026-06-27-3 | 7/8 (8 retried, 7 fixed) | 1/8 (8 retried, 1 fixed) |
| `qwen3.5-9b` | 9B · m5t | 2026-06-28 | 2026-06-27-3 | 5/8 (4 retried, 4 fixed) | 6/8 (6 retried, 6 fixed) |
| `qwen3-4b` | 4B · claw2 | **2026-07-01-8** | 2026-07-01-8 | 6/8 (4 retried, 2 fixed) | 6/8 (4 retried, 2 fixed) |
| `exaone3.5-32b` | 32B · claw1 | 2026-06-28 | 2026-06-27-3 | 1/8 (7 retried, 0 fixed) | 2/8 (8 retried, 2 fixed) |
| `granite4-tiny-7b` | 7B · m5t | 2026-06-28 | 2026-06-27-3 | 0/8 (8 retried, 0 fixed) | 0/8 (8 retried, 0 fixed) |
| `llama3.1-8b` | 8B · claw2 | 2026-06-28 | 2026-06-27-3 | 0/8 (8 retried, 0 fixed) | 0/8 (8 retried, 0 fixed) |
| `granite3.3-8b` | 8B · claw1 | 2026-06-28 | 2026-06-27-3 | 0/8 (8 retried, 0 fixed) | 0/8 (8 retried, 0 fixed) |
| `qwen3-1.7b` | 1.7B · claw1 | **2026-07-01-8** | 2026-07-01-8 | 0/8 (7 retried, 0 fixed) | 0/8 (8 retried, 0 fixed) |

*Correct/N per guide, bold = perfect score. "(N retried, M fixed)" shown only
when the repair loop was actually invoked — both columns already include
retries; there is no separate "first pass" number here (see Setup). Rows for
the same model are grouped together, oldest guide version first.*

<details>
<summary>Compiled detail — Large</summary>

| model | guide ver | aether ver | concise guide compiled | full guide compiled |
|---|---|---|---|---|
| `ornith-1.0-35b-nvfp4` | **2026-07-04-2** | 2026-07-04-2 | 7/8 | 8/8 |
| `gemini-2.5-flash` | 2026-06-28 | 2026-06-27-3 | 8/8 | 8/8 |
| `gemini-2.5-flash` | **2026-07-01-8** | 2026-07-01-8 | 7/8 | 6/8 |
| `gemini-2.5-flash` | **2026-07-04-2** | 2026-07-04-2 | 6/8 | 7/8 |
| `qwen3.6-35b-a3b` | 2026-06-28 | 2026-06-27-3 | 8/8 | 8/8 |
| `glm-5.2` | 2026-06-28 | 2026-06-27-3 | 8/8 | 8/8 |
| `glm-5-turbo` | 2026-06-28 | 2026-06-27-3 | 7/8 | 8/8 |
| `glm-5-turbo` | **2026-07-01-8** | 2026-07-01-8 | 8/8 | 8/8 |
| `gpt-oss-120b` | 2026-06-28 | 2026-06-27-3 | 8/8 | 7/8 |
| `devstral-24b` | 2026-06-28 | 2026-06-27-3 | 7/8 | 7/8 |
| `gemma3-27b` | 2026-06-28 | 2026-06-27-3 | 7/8 | 7/8 |
| `qwen3-coder-30b` | 2026-06-28 | 2026-06-27-3 | 6/8 | 7/8 |
| `mistral-small-24b` | 2026-06-28 | 2026-06-27-3 | 7/8 | 7/8 |
| `phi4` | 2026-06-28 | 2026-06-27-3 | 7/8 | 1/8 |
| `qwen3.5-9b` | 2026-06-28 | 2026-06-27-3 | 5/8 | 6/8 |
| `qwen3-4b` | **2026-07-01-8** | 2026-07-01-8 | 6/8 | 6/8 |
| `exaone3.5-32b` | 2026-06-28 | 2026-06-27-3 | 2/8 | 3/8 |
| `granite4-tiny-7b` | 2026-06-28 | 2026-06-27-3 | 0/8 | 1/8 |
| `llama3.1-8b` | 2026-06-28 | 2026-06-27-3 | 1/8 | 1/8 |
| `granite3.3-8b` | 2026-06-28 | 2026-06-27-3 | 2/8 | 1/8 |
| `qwen3-1.7b` | **2026-07-01-8** | 2026-07-01-8 | 0/8 | 0/8 |

</details>

## CS-classics (19 tasks): textbook algorithms

| model | size · served | guide ver | aether ver | concise guide | full guide |
|---|---|---|---|---|---|
| `ornith-1.0-35b-nvfp4` | 35B-MoE · claw1 | 2026-07-04-2 | 2026-07-04-2 | 18/19 (3 retried, 2 fixed) | 17/19 (5 retried, 4 fixed) |
| `ornith-1.0-35b-nvfp4` | 35B-MoE · claw1 | **2026-07-05-1** | 2026-07-04-2 | 17/19 (6 retried, 4 fixed) | 18/19 (5 retried, 5 fixed) |
| `glm-5-turbo` | — · cloud | 2026-06-28 | 2026-06-27-3 | 18/19 (1 retried, 1 fixed) | **19/19** (1 retried, 1 fixed) |
| `glm-5-turbo` | — · cloud | **2026-07-01-8** | 2026-07-01-8 | **19/19** (2 retried, 2 fixed) | 16/19 |
| `glm-5.2` | — · cloud | 2026-06-28 | 2026-06-27-3 | 18/19 (3 retried, 3 fixed) | 18/19 (1 retried, 1 fixed) |
| `gemini-2.5-flash` | — · cloud | 2026-06-28 | 2026-06-27-3 | 18/19 (3 retried, 2 fixed) | 17/19 (4 retried, 2 fixed) |
| `gemini-2.5-flash` | — · cloud | **2026-07-01-8** | 2026-07-01-8 | 15/19 (8 retried, 4 fixed) | 13/19 (7 retried, 1 fixed) |
| `gemini-2.5-flash` | — · cloud | **2026-07-04-2** | 2026-07-04-2 | 15/19 (8 retried, 4 fixed) | 15/19 (8 retried, 4 fixed) |
| `qwen3.6-35b-a3b` | 35B-A3B · m5t | 2026-06-28 | 2026-06-27-3 | 17/19 (2 retried, 2 fixed) | 18/19 (2 retried, 1 fixed) |
| `gpt-oss-120b` | 120B MXFP4 · claw1 | 2026-06-28 | 2026-06-27-3 | 17/19 (6 retried, 4 fixed) | 17/19 (8 retried, 6 fixed) |
| `qwen3-coder-30b` | 30B-A3B · m5t | 2026-06-28 | 2026-06-27-3 | 10/19 (10 retried, 1 fixed) | 13/19 (8 retried, 2 fixed) |
| `qwen3.5-9b` | 9B · m5t | 2026-06-28 | 2026-06-27-3 | 8/19 | 13/19 (4 retried, 3 fixed) |
| `gemma3-27b` | 27B · claw2 | 2026-06-28 | 2026-06-27-3 | 7/19 (14 retried, 2 fixed) | 12/19 (12 retried, 5 fixed) |
| `qwen3-4b` | 4B · claw2 | **2026-07-01-8** | 2026-07-01-8 | 12/19 (9 retried, 2 fixed) | 12/19 (7 retried, 0 fixed) |
| `devstral-24b` | 24B · m5t | 2026-06-28 | 2026-06-27-3 | 11/19 (11 retried, 3 fixed) | 11/19 (11 retried, 3 fixed) |
| `exaone3.5-32b` | 32B · claw1 | 2026-06-28 | 2026-06-27-3 | 8/19 (14 retried, 3 fixed) | 7/19 (14 retried, 2 fixed) |
| `mistral-small-24b` | 24B · claw1 | 2026-06-28 | 2026-06-27-3 | 7/19 (12 retried, 0 fixed) | 7/19 (14 retried, 2 fixed) |
| `phi4` | 14B · claw2 | 2026-06-28 | 2026-06-27-3 | 6/19 (14 retried, 2 fixed) | 5/19 (14 retried, 0 fixed) |
| `granite4-tiny-7b` | 7B · m5t | 2026-06-28 | 2026-06-27-3 | 2/19 (18 retried, 1 fixed) | 4/19 (16 retried, 1 fixed) |
| `qwen3-1.7b` | 1.7B · claw1 | **2026-07-01-8** | 2026-07-01-8 | 4/19 (17 retried, 3 fixed) | 2/19 (16 retried, 0 fixed) |
| `granite3.3-8b` | 8B · claw1 | 2026-06-28 | 2026-06-27-3 | 2/19 (17 retried, 0 fixed) | 3/19 (16 retried, 0 fixed) |
| `llama3.1-8b` | 8B · claw2 | 2026-06-28 | 2026-06-27-3 | 1/19 (18 retried, 0 fixed) | 2/19 (17 retried, 0 fixed) |

*Correct/N per guide, bold = perfect score. "(N retried, M fixed)" shown only
when the repair loop was actually invoked — both columns already include
retries; there is no separate "first pass" number here (see Setup). Rows for
the same model are grouped together, oldest guide version first.*

*`glm-5-turbo`'s full guide has 3 non-generated cases (proxy timeout on `fibonacci`/`hanoi`/`quick_sort` — see Status), scored as 0 per the never-generated convention, not a correctness miss. `qwen3-4b`'s concise guide (Simple table) similarly has 7 non-generated cases (T'Ra queue/network timeouts, not a capability miss) — see the raw per-model JSON for which tasks.*

*`ornith-1.0-35b-nvfp4`'s `2026-07-04-2` row's full-guide miss is
`cs_edit_distance`: the generated program never terminates within the
harness's local execution timeout (20s) — a real model weakness (an
unbounded/incorrect DP loop), not infra. It also recurs identically on the
`2026-07-05-1` re-run (same task, same failure mode both times), so it's a
standing miss independent of the harness change below. `cs_quick_sort` (full)
and `cs_bfs` (concise), also misses on that row, both compiled and ran but
produced wrong output through all 3 attempts, never converging — see the
harness-change note for what fixed the first of those two. Model card:
DeepReinforce Ornith-1.0, the 35B-MoE member of the family
(`Qwen3_5MoeForConditionalGeneration`, confirmed via `config.json` — not
dense), served NVFP4 on stock llama.cpp with MTP. It is a reasoning model
(`<think>...</think>` before the answer, `--reasoning-parser qwen3`), so its
unusually fast wall-clock here is the MoE architecture (few active experts per
token) plus MTP speculative decoding carrying the reasoning overhead
efficiently — not a non-thinking shortcut. The `2026-07-04-2` run also
surfaced and fixed a real harness bug: `run_model_with_deadline()` in
`tools/aether_doc_bench.py` joined the LLM-call subprocess before draining its
result queue, which can deadlock for the full `request_timeout_seconds` (here
8900s) whenever a generated response is large enough to fill the OS pipe
buffer (~64KB on macOS) — hit on `cs_merge_sort`'s first attempt across two
independent runs. Fixed by reading the queue with a timeout before joining;
unrelated to the VM 2.0 rewrite.*

*The `2026-07-05-1` re-run (harness-only bump, see Status) demonstrates the
second harness fix: `derive_failure_summary` now computes an actual
expected-vs-observed diff for "ran clean but wrong output" repair attempts
instead of a bare `"stdout_mismatch"` label. `cs_quick_sort`'s prior miss is
now fixed — its repair reasoning explicitly cited the new diff
(`"expected 9 token(s), got 3; missing: 1,3,4,6,7,9"`) and correctly
identified that its quicksort dropped every pivot on a distinct-value array
(`loop i in 1..n` → `loop i in 0..n`), a clean before/after win attributable to
the fix. `cs_bfs` also now passes on `full`, though that one isn't a clean
attribution — it's a crash-then-fix case where the *existing* error-message
feedback (unrelated to this change) already applied. Sampling variance
(temperature 0.2) then surfaced two different concise-guide misses on this
run instead: `cs_bubble_sort` produces correctly-sorted values but with a
persistent per-element-newline formatting bug (`"1\n, 2\n, 3\n..."` instead of
one line), unfixed after 3 attempts including a repair that briefly regressed
into a scope error; `cs_bfs` (concise) hit the identical `"Length expects a
string or array argument"` crash on all 3 attempts, i.e. repair had a strong,
specific error message each time and still reproduced the same broken code.
Net: one clean attributable fix, one coincidental fix, two fresh misses of the
same "wrong output despite running" and "same crash won't resolve" shapes the
harness already had — an argument the model's ceiling on `cs`, not the
harness, is now the binding constraint for this pair of tasks.*

<details>
<summary>Compiled detail — CS-classics</summary>

| model | guide ver | aether ver | concise guide compiled | full guide compiled |
|---|---|---|---|---|
| `ornith-1.0-35b-nvfp4` | 2026-07-04-2 | 2026-07-04-2 | 19/19 | 18/19 |
| `ornith-1.0-35b-nvfp4` | **2026-07-05-1** | 2026-07-04-2 | 18/19 | 18/19 |
| `glm-5-turbo` | 2026-06-28 | 2026-06-27-3 | 18/19 | 19/19 |
| `glm-5-turbo` | **2026-07-01-8** | 2026-07-01-8 | 19/19 | 16/19 |
| `glm-5.2` | 2026-06-28 | 2026-06-27-3 | 18/19 | 18/19 |
| `gemini-2.5-flash` | 2026-06-28 | 2026-06-27-3 | 18/19 | 18/19 |
| `gemini-2.5-flash` | **2026-07-01-8** | 2026-07-01-8 | 15/19 | 14/19 |
| `gemini-2.5-flash` | **2026-07-04-2** | 2026-07-04-2 | 17/19 | 15/19 |
| `qwen3.6-35b-a3b` | 2026-06-28 | 2026-06-27-3 | 17/19 | 18/19 |
| `gpt-oss-120b` | 2026-06-28 | 2026-06-27-3 | 17/19 | 17/19 |
| `qwen3-coder-30b` | 2026-06-28 | 2026-06-27-3 | 12/19 | 14/19 |
| `qwen3.5-9b` | 2026-06-28 | 2026-06-27-3 | 8/19 | 14/19 |
| `gemma3-27b` | 2026-06-28 | 2026-06-27-3 | 11/19 | 13/19 |
| `qwen3-4b` | **2026-07-01-8** | 2026-07-01-8 | 12/19 | 14/19 |
| `devstral-24b` | 2026-06-28 | 2026-06-27-3 | 15/19 | 13/19 |
| `exaone3.5-32b` | 2026-06-28 | 2026-06-27-3 | 10/19 | 7/19 |
| `mistral-small-24b` | 2026-06-28 | 2026-06-27-3 | 9/19 | 10/19 |
| `phi4` | 2026-06-28 | 2026-06-27-3 | 6/19 | 5/19 |
| `granite4-tiny-7b` | 2026-06-28 | 2026-06-27-3 | 4/19 | 6/19 |
| `qwen3-1.7b` | **2026-07-01-8** | 2026-07-01-8 | 8/19 | 4/19 |
| `granite3.3-8b` | 2026-06-28 | 2026-06-27-3 | 4/19 | 5/19 |
| `llama3.1-8b` | 2026-06-28 | 2026-06-27-3 | 5/19 | 5/19 |

</details>

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

**`glm-5.2` re-run blocked (2026-07-03):** the current-guide re-run of `glm-5.2`
hit `HTTP 402` ("insufficient credits") on the autoglm/autoclaw account partway
through the `simple` instrument (4 of 30 cases completed before the account ran
dry) — an account funding issue, not a model or harness problem. `glm-5.2`'s
row above is still the `2026-06-28` result; the current-guide re-run was
discarded rather than published, and will be redone once the account is
funded.

**`qwen35-9b-base` thinking-mode scoped test (2026-07-19):** targeted
follow-up on the large suite only, since it was this model's biggest
full-guide gap vs `qwen3-8b-nothink` (5/9 vs 9/9) in the main board. Result
is in the Large table (marked §) — thinking mode did not close the gap; it
traded compile-failures for timeout-failures (every task that finished
compiled correctly, but 3–4 of 9 never finished within the 1800s-per-attempt
cap even with repair), coming out at 5–6/9 vs no-think's 5–8/9. Same three
tasks timed out under both guide sizes, pointing at a runaway-reasoning
failure mode on those specific prompts rather than a context-budget issue.

**Guide version note (2026-07-03):** the language hardened materially this session
(stricter parser, new coded diagnostics, contract/effect changes), and the guide
was updated to match — its self-declared version moved from `2026-06-28` to
`2026-07-01-8`. Rows below are stamped per-model with the guide version they
were actually scored against; do not read a `2026-07-01-8` row as directly
comparable to the `2026-06-28` majority without accounting for that. The
existing `2026-06-28` rows are not being re-run wholesale (a decision, not an
oversight) — only new destinations land on the current guide going forward.

**Aether-version note (2026-07-04):** added the `aether ver` column above,
sourced from each result JSON's recorded `aether_version` field, to stop
trusting `guide ver` as a proxy for "what compiler build produced this
number." First pass surfaced what looked like real drift: every
`2026-07-01-8`-guide row here (`gemini-2.5-flash`, `glm-5-turbo`, `qwen3-4b`,
`qwen3-1.7b`) had recorded `aether_version: 20260702.1858_DEV` — a
build-timestamp string, not a language version. Investigating *why* found a
real bug, not real drift: PBuild's umbrella build was silently overriding
Aether's own `VERSION`-file-derived version with a generic
build-wall-clock/`_DEV` stamp meant for frontends that don't track their own
language version (fixed in PBuild's CMakeLists.txt + a new
`PSCAL_PROGRAM_VERSION_OVERRIDE` hook in pscal-core, 2026-07-04). Decoding the
timestamp against git history (which language `VERSION` was actually checked
out on 2026-07-02 18:58) shows it was `2026-07-01-8` the whole time — an exact
match to `guide ver`, now shown directly in the table. So for every row in
this doc, `aether ver` and `guide ver` agree; the apparent drift was a
reporting bug, now fixed, not an actual mismatch. That is not true fleet-wide,
though — see the no-guide findings for a case ([`cs-aug3`
board](aether_specialization_findings.md#cs-aug3)) where decoding the same
kind of timestamp turned up **real** version drift within a single result
set.

**`ornith-1.0-35b-nvfp4` landed (2026-07-05):** all three instruments complete
against the `2026-07-04-2` guide/aether pair — `simple` 30/30 both variants,
`large` 8/8 (full) / 7/8 (concise), `cs` 17/19 (full) / 18/19 (concise). Rows
above; see the CS-classics footnote for the genuine misses and a harness bug
this run surfaced and fixed (`run_model_with_deadline()` queue/join ordering
deadlock in `tools/aether_doc_bench.py`). Re-run on `cs` after the follow-up
harness fix below (`2026-07-05-1`): `full` 18/19 (only `cs_edit_distance`
remains, a standing miss both runs), `concise` 17/19 (`cs_quick_sort`/`cs_bfs`
from the first run now pass; `cs_bubble_sort`/`cs_bfs` fail instead — see
footnote for which fixes are attributable to the harness change vs. run-to-run
model variance). `simple`/`large` were not re-run (nothing in that harness
change touches their pass/fail path) and stand as reported above.

**Guide version now also covers harness changes (2026-07-05):** bumped
`2026-07-04-2` → `2026-07-05-1` on both guides with **no change to the guide
text itself** — the bump reflects a `tools/aether_doc_bench.py` change
(`derive_failure_summary` now computes an actual expected-vs-observed diff for
"ran clean but wrong output" repair attempts instead of a bare
`"stdout_mismatch"` label; see the CS-classics footnote). Reusing this column
rather than adding a new one is a deliberate simplification — if a row's guide
version bumps with no visible diff to `aether_for_llms_and_others.md` /
`aether_for_llms_with_small_contexts.md`, check here and in git history for a
harness-only reason before assuming drift (this doc already had one real scare
of that shape — see the aether-version note below). Going forward, any change
to the benchmark harness that could plausibly move a score gets a guide-version
bump plus a dated Status note like this one, even absent a guide-text edit.

**In progress:** the two GB10 Sparks (claw1/claw2), routed through the T'Ra
scheduler, are benchmarking a broad tail — more small models (qwen3 1.7b-32b,
deepseek-r1, command-r) and larger ones (gemma3-12b, llama3.3-70b,
command-r-plus, nemotron). `qwen3-1.7b` is the first to complete
(all three instruments, `2026-07-01-8` guide). (The original m5t laptop tail
was moved to the claws, which serve 32B models far faster; `seed-oss-36b`
stays dropped — impractically slow.) The broad single-pass 2B–122B sweep —
including the cloud flagships that ace
every board — remains in the [archive](archive/aether_guided_benchmark.md).
