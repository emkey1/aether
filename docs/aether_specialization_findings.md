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

**Model shorthand names encode generation and MoE-vs-dense, not just family —
read them literally.** `qwen3-8b` is Qwen3 (`Qwen3ForCausalLM`). `qwen25-14b`
is Qwen2.5-Coder-14B-Instruct (`Qwen2ForCausalLM`) — an older generation
despite the larger parameter count; this name was `qwen14b` before
2026-07-17, which read as if it were same-generation as `qwen3-8b` and
caused a real "why does the smaller model win" confusion until the configs
were checked. `qwen36-27b` (dense) and `qwen36-35b-a3b` (MoE, 256
experts/8 active) are both Qwen3.6 (`qwen3_5`/`qwen3_5_moe` in HF's own
internal naming — the project uses the vendor's "3.6" branding, not HF's
internal string); `qwen36-35b-a3b` was `q36` before 2026-07-18, a name that
gave no hint of size or MoE-ness and was easy to misread as a 3.6B or 36B
dense model. `qwen3-coder30b-a3b` (MoE, 128 experts/8 active,
`Qwen3MoeForCausalLM`) was `a3b-coder30b` before the same date — that name
had no Qwen/generation prefix at all. **"A3B" means ~3B *active* parameters
per token, not total size** — don't read `qwen36-35b-a3b`'s "3b" as its
real size (35B total) or `qwen3-coder30b-a3b`'s (30B total). `mistral24b`
and `deepseek6.7b` have no Qwen-generation ambiguity. When in doubt, check
`config.json`'s `architectures`/`model_type` fields on the actual weights
rather than trusting a name at face value.

---

## cs-aug4 (current primary board)

**Corpus:** `cs-aug4` (`Tests/aether_specialization/out_cs_aug4`, dataset
`2026-07-15-1`) — same instruction+repair shape as `cs-aug3` (390 corpus items,
408 instruction records, 38 repair records), regenerated from scratch against
a **fixed compiler**: `aether 2026-07-09-1` at commit `9a44e67`, which includes
[pscal-core#6](https://github.com/emkey1/pscal-core/pull/6)/[aether#10](https://github.com/emkey1/aether/pull/10) —
a real VM crash on ordinary recursion (`setTypeValue` leaving stale bits
across a Real→Int retype, misread as a live box pointer and freed) bisected
and fixed during this generation's prep. The prior `cs-aug3` corpus was
silently generated and verified against the *broken* `2026-07-09-1` build
before the bug was known; `cs-aug4` is the first corpus verified clean
against the fix.

**Test suite:** `tasks_v2_pos.json` v`2026-07-15-1` (simple, **35** — up from
30) · `tasks_hard.json` v`2026-07-15-1` (large, **9** — up from 8) ·
`tasks_cs.json` v`2026-06-23-1` (cs, 19, unchanged). The simple/large bumps
are **not comparable cell-by-cell to `2026-06-21-1` rows** — 11 stale
reference solutions were repaired (fx-gating changed since they were written)
and 6 new tasks were added targeting language features with previously zero
coverage: `MStream`/HTTP memory streams, *recursive* tuple-returning
functions, constant field defaults, digit separators, `@pure` above a
module's `export fn`, and large-N array-append growth. All six new goldens
were confirmed absent from the `cs-aug4` training corpus before scoring.

**Eval date:** 2026-07-16. **Serving:** vLLM bf16, `--enforce-eager`,
temperature 0.2, `--repair-attempts 1`, single pass (not the 3-repeat
convention used for `qwen36-27b-cs-aug3`). The two Qwen3.6-family models
(`qwen36-27b`, `qwen36-35b-a3b`) were served with `chat_template_kwargs: {enable_thinking:
false}` — with thinking on, `qwen36-27b` needed ~4 hours just for the 35-task
simple suite (each generation was an 8-9K-character reasoning trace); with it
off, the same suite took about 13 minutes. This is a deliberate protocol
difference from `cs-aug3`'s thinking-on treatment of this model family, not a
degraded run.

### Setup

- **Same 7 models as the `cs-aug2-builtins`/`cs-aug3` cohort**, retrained
  QLoRA (bf16 for `qwen36-27b`, 4-bit for the rest) on `cs-aug4`, in size
  order: `deepseek6.7b`, `qwen3-8b-nothink`, `qwen25-14b`, `mistral24b`,
  `qwen36-27b`, `qwen3-coder30b-a3b`, `qwen36-35b-a3b`.
- **`qwen35-9b` added 2026-07-18** as an eighth, size-comparison model —
  Qwen3.5-9B (`qwen3_5` architecture, released 2026-03-02), chosen as a
  newer-generation counterpart to `qwen3-8b-nothink` at a near-identical
  parameter count, after `mistral24b`/`qwen25-14b` both underperformed their
  size class relative to `qwen3-8b-nothink` on this board. Trained 4-bit
  QLoRA, same recipe/hyperparameters as `qwen3-8b-nothink` (`--lora-r 32
  --lora-alpha 64 --learning-rate 1e-4 --epochs 3`), served with
  `chat_template_kwargs: {enable_thinking: false}` like the other `qwen3_5`
  family members. Seed-pinned (`seed: 42`), so this result — unlike most of
  the rest of this board — is directly comparable to any other seed-pinned
  run without the pre-2026-07-17 noise caveat.
- **Training queue was interrupted twice by claw2 hardware faults** (see
  `claw2` reliability notes) mid-run; the resumable queue design (skip if
  `merged_16bit` already exists) meant no completed model was lost or
  redone — only the in-flight model at each interruption retrained from
  scratch.
- **Two serving-side tokenizer bugs surfaced and were fixed before scoring**,
  both now recognized as a recurring failure class in this project (a third,
  training-side instance is `deepseek6.7b`'s exclusion below): `mistral24b`'s
  Unsloth-merged export's rewritten `tokenizer.json` reproduced the
  archived Tekken space-dropping bug (`0=>0` instead of `0 => 0`); serving
  with the base model's tokenizer (`--tokenizer <base snapshot>`) fixed it
  cleanly (16/35 broken → 32/35 correct, identical weights). Applied
  pre-emptively to `deepseek6.7b` as well given its documented tokenizer
  history.

### Results — no guide (`--docs none`), repair-attempts 1

| model | corpus | simple (35) | large (9) | cs (19) |
|---|---|---|---|---|
| `qwen25-14b` | cs-aug4 | 34/**34**/1/0 | 9/7/3/1 | 14/12/8/1 |
| `qwen36-35b-a3b` (35B-A3B hybrid, no-think) | cs-aug4 | 34/**34**/3/2 | 6/6/4/1 | 12/**12**/7/1 |
| `qwen36-27b` (dense, no-think) | cs-aug4 | 33/33/5/3 | 7/6/7/4 | 11/9/10/2 |
| `mistral24b` | cs-aug4 | 33/32/5/2 | 7/5/8/4 | 12/9/10/0 |
| `qwen3-coder30b-a3b` (30B-A3B MoE) | cs-aug4 | 29/28/9/2 | **8**/7/3/1 | 12/9/9/0 |
| `qwen3-8b-nothink` | cs-aug4 | 30/30/7/2 | 8/6/5/2 | 11/9/10/0 |
| `deepseek6.7b`† | cs-aug4 | 30/28/7/0 | 5/4/5/0 | 12/8/13/2 |
| `qwen35-9b` (no-think) | cs-aug4 | 31/30/10/5 | 5/4/8/3 | 14/6/12/0 |

*Cells are Compiled/Correct/Retried/Fixed (overlapping tallies, not a
partition — they do not sum to N): Compiled = ran rc 0; Correct = exact
stdout; Retried = needed ≥2 attempts; Fixed = a repair turned a failure into
a pass. `Compiled ≥ Correct` is not guaranteed here (a program can compile,
run, and produce wrong output) but held in every cell on this board.
†`deepseek6.7b` served via llama.cpp/GGUF, not vLLM — see below.*

### `qwen35-9b-cs-aug4` — newer generation, similar size, still not a clean win

Added specifically to test whether generation (Qwen3.5, `qwen3_5`
architecture) beats parameter count at explaining the `qwen3-8b-nothink` vs.
`mistral24b`/`qwen25-14b` pattern noted above. It doesn't, at least not here:
`qwen35-9b` trails `qwen3-8b-nothink` on every suite (simple 30 vs. 30 correct
— tied — but large 4 vs. 6, and cs a clear 6 vs. 9), despite being the newer
architecture at a comparable parameter count and using the identical training
recipe.

The cs-suite gap is the most striking: 6/19 correct with **zero** of the 12
retried tasks recovered by repair — every other model on this board recovers
at least one retried task via repair (`qwen25-14b` and `qwen36-35b-a3b` both recover 1,
`qwen36-27b`/`mistral24b`/`qwen3-coder30b-a3b` recover 2-4). Raw generations were
checked for the tokenizer/serving corruption class documented in
[[tokenizer-serving-gotcha]] — none found; failures are genuine task misses
(e.g. `cs_fibonacci` printed each value on its own line with a stray leading
comma, `0\n, \n1\n, \n1\n...`, instead of a single comma-joined line), not a
detokenization artifact. Read together with the guided-benchmark board (where
`qwen3-8b-base` also outperformed both `mistral24b-base` and
`qwen25-14b-base`), the emerging picture is that **parameter count and
release date are weak predictors of Aether specialization quality** for this
task — `qwen3-8b`'s underlying base model appears to just be unusually strong
for this narrow domain, a property that didn't carry forward into its
`qwen3_5`-generation successor at a similar size.

### `deepseek6.7b-cs-aug4` — a vLLM serving bug, not a training defect

This model's first-pass numbers (9/35 simple) looked like a broken
fine-tune — every generation was missing inter-word spaces and leaking
literal byte-level BPE glyphs (`Ġ` = space, `Ċ` = newline;
`println("hello from benchmark")` → `hellofrombenchmark`). The investigation
took several wrong turns before landing on the real cause (full chain
recorded in the tokenizer-serving-gotcha project note, since each wrong turn
looked convincing on its own):

- Tokenizer round-trip, chat-template rendering, and Unsloth's masking
  boundary all checked out clean against real training data.
- The 408-record training corpus had zero anomalous records.
- A bf16 retrain (vs. the original 4-bit QLoRA) produced clean output
  under direct `transformers.generate()` — but **the same checkpoint
  reproduced the identical corruption once served through vLLM**, including
  via the raw completions endpoint with a pre-rendered prompt (bypassing
  vLLM's own chat-template logic). The bf16 result was a false signal from
  testing against the wrong path, not a fix.
- Decisive test: the **stock, never-fine-tuned** base
  `deepseek-coder-6.7b-instruct`, served through the same vLLM setup,
  produced the same glyph corruption. **This is a vLLM bug in decoding this
  model's tokenizer, unrelated to anything about our training, corpus, or
  quantization choice.**
- A plausible-looking harness-side fix (substitute `Ġ`→space, `Ċ`→newline in
  vLLM's raw output before scoring) was tested end-to-end against all 25
  failing cases and recovered **zero** — the structural/syntax spacing came
  back, but the specific string-literal content the task asks the model to
  echo was still corrupted underneath, not just mis-displayed.
- **Actual fix:** exported the checkpoint to GGUF (Unsloth's built-in
  `save_pretrained_gguf`) and served it with `llama.cpp` instead of vLLM.
  Completely clean — the historical llama.cpp pre-tokenizer gap that
  blocked this model's GGUF path in the cs-aug2-builtins era appears fixed
  upstream. The results above are this llama.cpp-served run.

Net: `deepseek6.7b` is a perfectly normal small-model result once served
correctly — smallest model in the cohort, near the bottom of the pack, no
sign of a defect. The real finding is infrastructural: **vLLM cannot
currently serve this model's tokenizer correctly on this project's stack;
llama.cpp/GGUF can.** Worth checking whether other deepseek-family models
hit the same vLLM issue before assuming any one of them is serving-safe.

### Findings

- **The hard-task gap the project has tracked since `cs-aug2-builtins`
  (fine-tunes hit 25-29/30 simple but only 3-7/8 large with no guide) is
  visibly narrower on this corpus generation.** Six of seven models score
  5-7/9 on the large suite — the same range this project's guided
  *frontier* models have historically held, not the 3-5/8 no-guide fine-tune
  floor (`deepseek6.7b`, the smallest model in the cohort at 6.7B, is the
  exception at 4/9 — plausibly just a capacity floor, not evidence against
  the pattern). `qwen25-14b` and `qwen36-35b-a3b` both reach 12/19 on cs-classics,
  matching or beating the previous best fine-tuned no-guide score on that
  instrument. Whether this is the corpus refresh (bench-log-mined drills,
  the newly fixed compiler letting more corpus cases verify cleanly), the
  larger/more current base models, or some mix, is not yet isolated — worth
  a same-corpus/different-vintage-compiler A/B if it matters later.
- **`qwen25-14b` is the strongest model on this board**, leading or tying the
  top score on all three suites — a change from earlier boards where the
  30B-class MoE (`qwen3-coder30b-a3b`) or the reasoning hybrid (`qwen36-35b-a3b`) usually led.
- **New-task coverage results are a clean signal of real corpus gaps, not
  eval noise.** Per-task pass counts across all 7 models: `mstream_roundtrip`
  **0/7** (total, uniform failure — the `MStream`/HTTP surface added
  2026-07-09 has zero training exposure in this corpus); `tuple_recursive_digits`
  5/7; `field_defaults_gauge` 6/7; `module_pure_export` 6/7;
  `digit_separators_sum` 6/7; `hard_append_growth` **7/7**. The
  append-in-loop idiom is universally solid; digit separators are solid bar
  one model; recursive tuple returns, field defaults, and `@pure`-annotated
  module exports are each a near-miss (one or two models fail) — plausible
  next-corpus drill candidates alongside the outright `MStream` gap.
- **Tokenizer/serving bugs remain the single biggest threat to trusting a
  low score at face value — this board hit two, of different severity.**
  `mistral24b` needed a one-flag fix (serve with the base model's
  tokenizer). `deepseek6.7b` needed a full serving-backend switch (vLLM →
  llama.cpp/GGUF) after training-side theories about the same symptom were
  investigated and ruled out — see below. A naive read of either model's
  first-pass result would have logged a false "bad fine-tune" finding, and
  in `deepseek6.7b`'s case nearly did so through two different wrong
  root-cause claims before the actual fix was found and verified end-to-end.

### Methodological note: `repair-attempts` comparisons before 2026-07-17 are not controlled experiments

While investigating why `repair-attempts=1` (this board's convention) and
`repair-attempts=2` (the guided-benchmark side's convention) gave visibly
noisy, sometimes-worse-not-better deltas per model when re-run back to back,
we found the harness (`tools/aether_doc_bench.py`) had **no random seed
pinning anywhere** — every request to every destination sampled fresh at
`temperature=0.2` with no `seed` parameter. This means two harness
invocations of "the same" task, including the very *first* attempt (not just
repair retries), are independent stochastic draws — confirmed empirically by
diffing `qwen36-27b`'s attempt-0 raw generations between a `repair=1` run and
a `repair=2` run for tasks that flipped pass/fail: the initial generations
were textually different, not just the repair path. So a `repair-attempts=1`
vs `repair-attempts=2` re-run is not isolating the repair-budget variable —
it's two uncontrolled samples, and a "regression" under more repair attempts
can simply mean the first attempt drew a worse sample that time, unrelated to
having more retries available.

**Fixed 2026-07-17**: `tools/aether_doc_bench.py` now supports a
per-destination `"seed"` field (threaded through all three OpenAI-compatible
request builders), verified to produce byte-identical output across repeated
calls with the same seed. All `cs-aug4` sweep scripts now pin `seed: 42` by
default. **Every repair-attempts / corpus-version / any other A/B comparison
in this document from before this fix should be read as directional, not a
controlled measurement** — this includes the `cs-aug2-builtins` vs `cs-aug3`
guide-corpus comparisons and the repair-recovery-rate figures throughout.
This matters most for smaller, less capable models, whose outputs are
noisier and more sensitive to a bad initial draw than frontier models are.

### Status

Full board landed in one session (2026-07-16) after two claw2 hardware
interruptions and two tokenizer/serving fixes (one one-flag fix for
`mistral24b`, one full backend switch for `deepseek6.7b`). The `MStream`-drill
gap identified here is a natural target for the next corpus generation. A
`repair-attempts=1` vs `repair-attempts=2` re-run of the full board (2026-07-17)
found a small net gain (+5 correct out of 441 suite-task pairs) but with
inconsistent per-model direction — see the methodological note above; this
was the investigation that surfaced the harness's missing seed pinning, now
fixed.

---

## cs-aug2-builtins

**Corpus:** `cs-aug2-builtins` (`Tests/aether_specialization/out_cs_aug2_builtins`) —
instruction + repair SFT set adding the non-SDL builtin reference to the training data.
**Test suite:** `tasks_v2_pos.json` v`2026-06-21-1` (simple, 30) · `tasks_hard.json`
v`2026-06-21-1` (large, 8) · `tasks_cs.json` v`2026-06-23-1` (cs, 19).
**Eval date:** 2026-06-29. **Aether compiler:** `2026-06-27-3` for all five
cs-aug2-builtins models; the `cs-aug2` (old) baseline row ran one build earlier,
`2026-06-27-2`.

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
| `qwen3-coder30b-a3b` (30B-A3B) | cs-aug2-builtins | **29**/29/3/2 | 5/5/4/1 | 12/9/11/1 |
| `mistral24b` | cs-aug2-builtins | 28/27/4/1 | 7/3/5/0 | 8/6/15/2 |
| `qwen36-35b-a3b` (35B-A3B hybrid) | cs-aug2-builtins | 27/27/13/12 | 6/6/7/6 | 13/13/9/5 |
| `qwen3-8b-nothink` | cs-aug2-builtins | 27/26/6/3 | 3/3/3/0 | 9/7/11/0 |
| `qwen25-14b` | cs-aug2-builtins | 25/25/6/1 | 6/4/4/0 | 13/10/11/2 |
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
- **MoE punches above its active parameters.** `qwen3-coder30b-a3b` (30B total, ~3B active)
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

`qwen36-35b-a3b` (Qwen3.6-35B-A3B, hybrid `qwen3_5_moe`) is now included, served as **bf16** (q8 is
broken for this hybrid arch). It needed the reasoning-model `stop:null` handling to score
at all — note its heavy repair use (simple: 13 retried, 12 fixed), the most of any model
here. `deepseek6.7b-cs-aug2-builtins` trained intact but is blocked from this board by an
upstream llama.cpp GGUF pre-tokenizer gap (see the deploy notes); its merged
checkpoint is preserved. Tables regenerate from the per-model result JSONs.

---

## cs-aug3

**Corpus:** `cs-aug3` (`Tests/aether_specialization/out_cs_aug3`, generated
2026-07-01, corpus tag `2026-07-01-1`) — retrain of the full cs-aug2-builtins model
set (`mistral24b`, `qwen25-14b`, `qwen3-8b-nothink`, `qwen3-coder30b-a3b`) plus, for the first
time, a dense ~27B model (`qwen36-27b`, Qwen3.6 dense).
**Test suite:** same three instruments/versions as above (`tasks_v2_pos.json`
`2026-06-21-1`, `tasks_hard.json` `2026-06-21-1`, `tasks_cs.json` `2026-06-23-1`),
`--docs none`, repair on.

`qwen36-27b` was evaluated at two serving precisions: **NVFP4/vLLM (claw2)** and
**Q8 GGUF/ollama (claw1)**. Two real budget bugs were found and fixed mid-run on
both before any result could be trusted: (1) the ollama Modelfile pinned `num_ctx
8192`, far too small for this reasoning model's thinking phase on harder tasks —
raised to 40960; (2) `request_timeout_seconds` was set well under the worst-case
decode time for a full 32768-token reasoning response at this hardware's throughput
(~5.4 tok/s Q8/claw1, ~13 tok/s NVFP4/claw2) — raised to 9000s and 5400s
respectively. Even after both fixes, claw1's Q8/ollama board kept producing an
unreliable signal — orphaned in-flight generations repeatedly survived client-side
kills and wedged the server's single decode slot, plus a reproducible cluster of
instant failures at every relaunch — and was abandoned as not worth the ongoing
recovery cost. **The `qwen36-27b-cs-aug3` result below is NVFP4/vLLM on claw2
only**; no Q8/ollama number for this model is reported.

### Results — `qwen36-27b-cs-aug3` (NVFP4/vLLM, claw2), no guide, 3 repeats/task

| instrument | n (tasks×3 repeats) | generated | compiled | exact (rate) | fixed by repair | aether ver |
|---|---|---|---|---|---|---|
| simple (v2_pos) | 90 | 87 | 69 | 66 (73.3%) | 21 | `2026-07-01-4` |
| large | 24 | 20 | 7 | 7 (29.2%) | 7 | `2026-07-01-8` |
| cs | 57 | 54 | 36 | 36 (63.2%) | 18 | `2026-07-01-8` |

**This board's `simple` pass is not directly comparable to its `large`/`cs`
passes — real compiler drift, not rebuild noise.** These originally recorded
as opaque local build-timestamps (`20260701.1525_DEV` /
`20260702.1858_DEV`) — an umbrella build-versioning bug that silently
discarded Aether's real language version, fixed 2026-07-04 (see
[`aether_guided_benchmark.md`](aether_guided_benchmark.md)). Decoded against
git history, `20260701.1525_DEV` (`simple`) was built under language version
`2026-07-01-4`, four version bumps *before* `20260702.1858_DEV`
(`large`/`cs`, `2026-07-01-8`). The gap in between includes the AST-frontend
cutover (the text rewriter retired as the default frontend), several new
strict-rejection parser rules, and diagnostic recoding — not cosmetic
changes. So this board's own three numbers were not scored by the same
compiler: treat the `simple` result as a different, earlier-language-version
run than `large`/`cs`, not as one coherent 3-instrument sweep.

Run with `--repeats 3` (not the single-pass convention of the rest of this
table) for reliability given this model's reasoning-heavy, noisier generation
behavior — cells are raw totals out of the repeated case count, not directly
comparable cell-by-cell to the single-pass rows above without dividing by 3.
Normalized to the same **/30, /8, /19** basis as the rest of the table:
**~22/30 simple, ~2.3/8 large, ~12/19 cs** — squarely in line with the other
cs-aug3-generation dense/MoE models on this board, and consistent with the
broader finding above that fine-tuning buys the easy set, not the hard tail.

The other four cs-aug3 models (`mistral24b`, `qwen25-14b`, `qwen3-8b-nothink`,
`qwen3-coder30b-a3b`) were not re-verified in this session — their board status
should be checked the next time claw2 is back up before folding them into
this table.
