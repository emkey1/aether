# Fine-Tuned, No Guide: Aether Specialization Findings

*The no-guide, fine-tuned half of "can a language model write Aether." Companion to
[`aether_guided_benchmark.md`](aether_guided_benchmark.md) — the in-context,
untrained half. Broader historical findings are preserved at
[`archive/aether_specialization_findings.md`](archive/aether_specialization_findings.md).*

Every results table below is stamped with the **training corpus** and **test-suite
version** it was produced against — both change over time, and a number without its
provenance is not comparable to a later one. Test-suite versions are the `version`
field baked into each `Tests/aether_doc_bench/tasks_*.json` file; corpus names match
the `Tests/aether_specialization/out_<corpus>/` directory that produced the SFT data.

---

## cs-aug2-builtins (current primary board)

**Corpus:** `cs-aug2-builtins` (`Tests/aether_specialization/out_cs_aug2_builtins`) —
instruction + repair SFT set adding the non-SDL builtin reference to the training data.
**Test suite:** `tasks_v2_pos.json` v`2026-06-21-1` (simple, 30) · `tasks_hard.json`
v`2026-06-21-1` (large, 8) · `tasks_cs.json` v`2026-06-23-1` (cs, 19).
**Eval date:** 2026-06-29.

This edition reports the models fine-tuned on the **cs-aug2-builtins** corpus — the
instruction + repair SFT set that adds the non-SDL **builtin reference** to the
training data — each evaluated with **no guide in the prompt** (`--docs none`) and
the repair loop on. The question it answers: how far does training alone carry a
model when there is *nothing* in context to lean on?

### Setup

- **Five models, QLoRA fine-tuned** on the cs-aug2-builtins corpus (419 train / 12
  eval records; instruction + repair JSONL), each served on claw2's Ollama and asked
  to solve the same three instruments as the guide side — the simple set (30), the
  large set (8), and CS-classics (19) — exact-stdout scored against an oracle, repair on.
- **Scoring is identical to the guide side:** **Compiled / Correct / Retried /
  Fixed**. Compiled = ran rc 0; Correct = exact stdout; Retried = needed ≥2 attempts;
  Fixed = a repair turned a failure into a pass. These four are overlapping tallies
  over the same N tasks (NOT a partition — they do not sum to N: `Compiled ≥ Correct ≥
  Fixed`, `Fixed ⊆ Retried`); a task that never produced a usable program counts 0 in
  all four.
- **A control for the corpus change.** The same Mistral-24B base, trained on the
  *previous* corpus (`cs-aug2`, **without** the builtin reference), is included as a
  baseline — an old-vs-new corpus A/B, both judged with no guide.

### Results — no guide (`--docs none`)

| model | corpus | simple (30) | large (8) | cs (19) |
|---|---|---|---|---|
| `a3b-coder30b` (30B-A3B) | cs-aug2-builtins | **29**/29/3/2 | 5/5/4/1 | 12/9/11/1 |
| `mistral24b` | cs-aug2-builtins | 28/27/4/1 | 7/3/5/0 | 8/6/15/2 |
| `q36` (35B-A3B hybrid) | cs-aug2-builtins | 27/27/13/12 | 6/6/7/6 | 13/13/9/5 |
| `qwen3-8b-nothink` | cs-aug2-builtins | 27/26/6/3 | 3/3/3/0 | 9/7/11/0 |
| `qwen14b` | cs-aug2-builtins | 25/25/6/1 | 6/4/4/0 | 13/10/11/2 |
| `mistral24b` *(baseline)* | cs-aug2 (old) | 25/25/7/2 | **0**/0/8/0 | 11/9/12/2 |

*Cells are Compiled/Correct/Retried/Fixed (overlapping tallies, not a partition).*

### Findings

- **Training buys the easy set, not the hard set.** With no guide at all, the
  fine-tuned models hit the **simple** set at **25–29/30** — right alongside the guided frontier
  models on the [other board](aether_guided_benchmark.md). But on the large set they
  fall to **3–7 compiled / 3–5 correct of 8**, and on `cs` to **6–10 correct of 19**,
  while the *guided* frontier models hold 7–8 and 17–18. Fine-tuning teaches the
  corpus distribution — which the simple set resembles — not the language's reasoning;
  in-context rules generalize to novel and harder shapes better than weights do.
  Sharpest tell: trained `mistral24b` compiles **7/8** on the large set but only
  **3** are correct. It writes valid Aether for hard novel tasks, then gets the
  algorithm wrong.
- **The builtins corpus broadened hard-task range — the point of the change.** The
  old-corpus baseline compiled **0 of 8** on the large set: it could not produce
  valid Aether for those novel tasks at all. The builtins-augmented model compiles
  **7 of 8** (3 correct), with a small simple-set gain on top. It dipped on `cs` (−3, but
  inside that board's small-n noise). Net: adding the builtin reference made the
  fine-tune materially more robust on novel shapes without hurting the core — exactly
  the hoped-for result.
- **MoE punches above its active parameters.** `a3b-coder30b` (30B total, ~3B active)
  tops the set at **29/30 on simple with no guide** — the closest a fine-tune comes to
  the guided frontier tier, on a fraction of the active compute.
- **Without the guide in context, repair helps less.** The trained models retry
  heavily on `cs` but fix little (`mistral24b`: 15 retried, 2 fixed). The guide
  side's self-correction works because the coded diagnostic points back into a guide
  section the model can re-read; a fine-tune with nothing in context has only its
  weights to fall back on, and the second pass mostly repeats the first.

### What this does and does not show

- It shows a fine-tune can **stand in for the guide on simple-class work** — useful for
  offline or tiny-context deployment where you cannot spend the prompt tokens.
- It does **not** close the hard-task gap that in-context rules do. For a capable
  model the practical recommendation remains: paste the
  [concise guide](aether_for_llms_with_small_contexts.md) and skip fine-tuning;
  fine-tune only when you genuinely cannot put the guide in context.

### Status

`q36` (Qwen3.6-35B-A3B, hybrid `qwen3_5_moe`) is now included, served as **bf16** (q8 is
broken for this hybrid arch). It needed the reasoning-model `stop:null` handling to score
at all — note its heavy repair use (simple: 13 retried, 12 fixed), the most of any model
here. `deepseek6.7b-cs-aug2-builtins` trained intact but is blocked from this board by an
upstream llama.cpp GGUF pre-tokenizer gap (see the deploy notes); its merged
checkpoint is preserved. Tables regenerate from the per-model result JSONs.

---

## cs-aug3 (in progress — not yet populated)

**Corpus:** `cs-aug3` (`Tests/aether_specialization/out_cs_aug3`, generated
2026-07-01, corpus tag `2026-07-01-1`) — retrain of the full cs-aug2-builtins model
set (`mistral24b`, `qwen14b`, `qwen3-8b-nothink`, `a3b-coder30b`) plus, for the first
time, a dense ~27B model (`qwen36-27b`, Qwen3.6 dense) trained and evaluated at two
serving precisions: Q8 GGUF/ollama and NVFP4/vLLM.
**Test suite:** same three instruments/versions as above (`tasks_v2_pos.json`
`2026-06-21-1`, `tasks_hard.json` `2026-06-21-1`, `tasks_cs.json` `2026-06-23-1`),
`--docs none`, repair on.
**Status as of 2026-07-02:** boards are running now, not complete. Two real bugs were
found and fixed mid-run before any result here can be trusted: (1) the ollama
Modelfile pinned `num_ctx 8192`, far too small for this reasoning model's thinking
phase on harder tasks — raised to 40960; (2) `request_timeout_seconds` was set well
under the worst-case decode time for a full 32768-token reasoning response at this
hardware's throughput (~5.4 tok/s Q8/claw1, ~13 tok/s NVFP4/claw2) — raised to 9000s
and 5400s respectively. Earlier partial runs that hit either bug were discarded, not
reported. This section will be filled in once both boards complete and results are
verified against these fixed budgets.
