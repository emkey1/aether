# Exact-Match Understates Competence: Findings from the Aether No-Guide Program

*Audience: maintainers, collaborators, and anyone reasoning about how to measure
whether a model has "learned" a language.* This is a findings note, not a
reference guide. For how to actually write Aether see
[`aether_for_llms_and_others.md`](aether_for_llms_and_others.md); for why the
language is shaped the way it is see
[`aether_architecture_and_rationale.md`](aether_architecture_and_rationale.md).

---

## TL;DR

We fine-tune sub-15B (now 20B+) language models to write Aether with **no
reference guide in the prompt**, and score them by **exact-match** on program
output. Across that program we keep running into one methodological fact that is
easy to state and easy to forget:

> **Exact-match makes competence look discontinuous.** A model can understand a
> language almost completely and still score near zero, because a single
> mechanical surface error zeroes out an otherwise-correct program. The measured
> score and the underlying competence are different quantities, and the gap is
> largest exactly where a benchmark looks most dramatic.

The practical fix is cheap: never report exact-match alone. Pair it with a
**graded** signal (does it compile and run?) and the two together tell you
*where* competence was lost, which a single all-or-nothing number cannot. The
rest of this note is the evidence for that claim and the other things the
program has turned up along the way: a corpus-size by model-size scaling law, a
working method for letting models co-design the language, and a tokenizer-level
confound that is a textbook instance of the headline observation.

---

## 1. The core observation

### 1.1 A worked example

Our scoring instrument runs each generated program and compares its standard
output, byte for byte, against an oracle. The headline KPI ("none") gives the
model the task and **nothing else**: no grammar, no
[`aether_for_llms_and_others.md`](aether_for_llms_and_others.md), no examples.

When we extended the program to a 24B dense model from a different family
(Mistral-Small-24B-Instruct-2501, which uses the Tekken tokenizer), the
exact-match score came back at **8/30**. Taken at face value that reads as a
catastrophic failure, worse than a 7B. It is not. The model's *logic* is almost
entirely correct. It systematically drops the spaces **inside string literals**
while spacing all surrounding code correctly:

```
prompt:    print exactly `hello from benchmark`
generated: fx { println("hellofrombenchmark"); }     # logic fine, spaces gone
```

```
prompt:    print one line per iteration in the form `0 => 0`, `1 => 1`, ...
generated: loop i in 0..5 { fx { println(i, "=>", i * i); } }   # squares right, " => " became "=>"
```

The squares are correct. The percentages are correct. The recursion is correct.
Every program that failed, failed by the *same* mechanical defect, and the
handful that passed were exactly the tasks whose expected output happens to
contain no internal spaces. The competence is high; the metric reads 8/30.

### 1.2 Why this is general, not a one-off

The single number that rescues the interpretation is **run-ok**: the fraction of
generations that compile and execute without error. For this model run-ok was
**75/90** (83%) while exact-match was 24/90 (27%). High run-ok with low
exact-match is the signature of *intact competence behind a surface defect*. The
inverse pattern, run-ok and exact-match falling *together*, is the signature of
*genuine capability loss* (see the 7B in §3.1, whose run-ok really does drop).
Either way it is the **pair** that localizes the failure. Exact-match alone
cannot distinguish "cannot do the task" from "can do the task, mis-typed one
byte," and those are the two cases you most need to tell apart.

### 1.3 The connection to "emergence"

This is the same effect that the emergent-abilities literature argues about.
Wei et al. (2022) reported capabilities that appear to switch on suddenly with
scale; Schaeffer et al. (2023, *Are Emergent Abilities of LLMs a Mirage?*)
showed that a large share of that apparent sharpness is manufactured by
**discontinuous metrics** — exact-match being the prototypical one — and that the
underlying competence often improves smoothly. Our Mistral result is a tidy
instance: under a continuous metric ("correct modulo whitespace," or even just
run-ok) the model looks strong; under exact-match it looks broken. Whenever one
of our curves looks suspiciously abrupt, the first question is whether we are
measuring the model or measuring the metric.

---

## 2. Background: the no-guide specialization program

Aether is a compact front end on the PSCAL backend, designed under one thesis
(from [`aether_architecture_and_rationale.md`](aether_architecture_and_rationale.md)):

> Aether is optimized so that a language model can write valid, correct Aether
> **with no reference guide in its prompt**, and the benchmark suite, not taste,
> is the instrument that tells us when the language design is wrong.

Concretely:

- **Models.** Open-weight code models fine-tuned with LoRA SFT: Qwen2.5-Coder-7B
  and -14B (dense), Qwen3-Coder-30B-A3B (MoE, ~3B active), and
  Mistral-Small-24B-Instruct-2501 (dense, different family).
- **Corpus.** A few hundred short *compositional* Aether programs, each
  generated and then **oracle-verified** (kept only if its real output matches
  the intended output byte-for-byte). The "1x" corpus is ~183 SFT records, the
  "2x" ~383.
- **Benchmark.** 30 held-out tasks, temperature 0, scored by exact standard-output
  match, with the reference guide withheld (the "none" condition). The benchmark
  is de-contaminated against the training corpus at the output level.
- **Loop.** Train, score with no guide, read the *per-task* failures, and decide
  whether each failure is a model limitation or a **language** limitation. The
  second case is the interesting one (see §3.2).

The training and benchmarking harness lives outside this repository; what lives
*here* is the language those findings feed back into: the compiler and runtime
in [`../src/aether/`](../src/aether), the design vision in
[`../src/aether/DESIGN.md`](../src/aether/DESIGN.md), the reference guides in
this `docs/` folder, and the conformance programs in [`../tests/`](../tests).

---

## 3. Scope of findings

### 3.1 A corpus-size by model-size scaling law (a dense rule)

Holding the recipe fixed per model and varying only the corpus (1x vs 2x), the
no-guide score moves in opposite directions depending on model size:

| model (recipe) | 1x | 2x | 3x | peak |
|---|---|---|---|---|
| Qwen2.5-Coder-7B dense (r32/a64/3ep) | 28 | 25 | — | **1x** |
| Qwen2.5-Coder-14B dense (r64/a128/4ep) | 26 | **30** | 24 | **2x** |
| Qwen3-Coder-30B-A3B MoE, 3B active (r32/a64/3ep) | 29 | 29 | — | flat |
| Mistral-Small-24B dense (r64/a128/4ep) | 27 | 29 | 27 | 2x |

*(Scores re-scored on the current compiler; see §3.5. The MoE and 7B-2x run-ok
moved with exact-match — 84→75 for the diluting 7B, ~flat for the others — so the
dense degradation is real, not a metric artifact.)*

The small dense model is **hurt** by more data, the large dense model is
**helped**, and the sparse MoE is **inert**. The per-task diffs make the
mechanism concrete: at 2x the 7B recovered one task but broke five *basics*
(printing a greeting, naming a constant, initializing a record), and its run-ok
fell with it (84→75). That is capacity-limited **catastrophic interference**:
fitting the larger corpus overwrites things it already had. The 14B absorbed the
same extra corpus cleanly (fixed three tasks, broke none, run-ok flat). The MoE
neither diluted nor gained.

Two readings worth keeping:

- **Active vs total parameters.** The MoE does not dilute despite having fewer
  *active* parameters than the 7B, so dilution-resistance tracks **total**
  parameters; but it does not gain like the dense 14B, so generalization-gain
  tracks **active** parameters. "Scale the corpus to the model" is a *dense*
  rule; the MoE is corpus-insensitive (the stable, if unspectacular, pick). That
  inertia is on near-distribution tasks only: §3.6 finds the *same* breadth makes
  the MoE the *best* generalizer to novel hard shapes, so "corpus-insensitive" is
  not the same as "unremarkable."
- **The metric caveat applies here too, and the data survives it.** Unlike the
  Mistral case, the 7B's run-ok drops alongside exact-match, so this is *real*
  degradation, not a metric artifact. The pairing in §1.2 is what lets us say so
  with confidence.

**Relation to the literature.** This rhymes *directionally* with Chinchilla
(Hoffmann et al., 2022): larger models want more data. It does **not** import
Chinchilla's regime. Chinchilla is single-epoch, compute-optimal *pretraining*
measured in LM loss, where more data never *hurts* a fixed model; ours is
multi-epoch SFT on a tiny corpus measured in exact-match, where it plainly does.
The better-matched references for our regime are LIMA (Zhou et al., 2023, quality
over quantity for instruction tuning) and the data-constrained scaling work
(Muennighoff et al., 2023, where repeated-data returns flatten and then go
negative past roughly four epochs, which is where we sit). The 3x sweep settles
the open question: the dense corpus-optimum is **bounded** and **shifts right with
model size** — the 7B peaks at 1x, the 14B at 2x — but past its own ceiling each
model *dilutes*. The 14B drops 30→24 at 3x, the same interference the 7B showed
going 1x→2x. The 24B then **sharpens** it: a different family at a larger size, it *also* peaks
at 2x, but its 3x penalty is far smaller (29→27, down 2, against the 14B's 6). Read
across the three sizes, the **3x penalty shrinks as the model grows**, so the
optimum is still creeping right, just coarsely. Neither the 14B nor the 24B is large
enough to *want* 3x, but the 24B is measurably closer to it; the size at which 3x
first becomes the optimum is therefore **larger than 24B**, not absent. (Two points
make a thin trend, so a >24B dense run would confirm it.) So the Chinchilla
*direction* keeps holding: bigger models do want more data, the optimum just
advances a corpus-doubling at a time and needs a real jump in scale to move. That is
the LIMA / data-constrained regime's slow version of the law, not compute-optimal
pretraining's linear one.

### 3.2 Letting the model co-design the language

The most useful signal in the failure logs is not a single model's mistake but a
**consistent** reach, across capable models, for a construct that feels natural
and does not exist yet. When that happens we treat it as evidence about the
*language*, not the model, and add the construct. Two examples landed this way:

- **Record-literal initializer** `new T { field: value, ... }`. Models kept
  writing it; the language only had `new T()` plus field-by-field assignment. We
  added the literal form.
- **TOON accessor symmetry** `toon_key_or(node, key, default)` and `toon_null()`.
  Every TOON *value* accessor already had a plain and an `_or` form; the *node*
  accessor `toon_key` did not. The 14B and the MoE both reached for the missing
  `toon_key_or`; the 7B used the plain form and passed. The asymmetry was a real
  API gap. The lowering lives in
  [`../src/aether/translate.c`](../src/aether/translate.c) (search `toon_key_or`,
  `toon_null`), and adding it took the best dense model to **30/30** on the clean
  sweep.
- **Integer division and modulo** (`n / 2`, `n % 2`) — surfaced by the hard-task
  probe of §3.6, and a different flavor of the same signal: not a *missing*
  construct but a *broken* one. Every model that wrote the natural recursion
  `if n % 2 == 0 { ret 1 + f(n / 2); }` failed to compile, with
  `Operands for 'mod' must be integers. Got REAL and INTEGER`. The cause was a
  coercion asymmetry in the shared backend: `/` returns Real (deliberately,
  Pascal-style), an Int *assignment* truncates that Real, but passing it as an
  Int *argument* did not — so the Real leaked into the recursive Int parameter
  and broke the following `%`. The fix coerces Real→Int at the argument boundary
  exactly as assignment already does, leaving `/` untouched (real-valued ratios
  are unaffected). The discipline of §3.2 still held: the *unanimous, identical*
  failure across capable models was the bug report, and the compile-fail vs
  wrong-output split in the failure log (§3.6) is what marked it a language bug
  rather than a model error. Re-scoring the cohort against the fixed compiler —
  no retraining, no re-inference — recovered the task for the five models that
  had written `n % 2`, while leaving untouched the three that had side-stepped it
  with a division-based parity check. The fix paid out exactly where the models
  had pointed.

This is the thesis of the architecture doc in practice: when the benchmark says
the language is wrong, change the language. The discipline is to do it only when
a *capable* model reaches for a *natural* construct, not to chase every one-off
error (a weaker model writing Python keywords is a model problem, not a language
gap).

### 3.3 Tokenizer fidelity as a confound

The Mistral result in §1 is not just an illustration of the metric point; it is
also a finding in its own right. The space-dropping is **specific to the model
family**: the Qwen models, trained on the *identical* data and served through
the *identical* stack, do not do it. The training data round-trips through the
tokenizer losslessly (a spaced string survives encode-then-decode), so the
corpus is not at fault. The defect appears at generation time and is consistent
with a Tekken-tokenizer interaction. The diagnostic that localizes it (compare
greedy generation through two different inference engines, then base model vs
fine-tune) is staged; the merged checkpoints persist, so re-scoring a fix costs
an evaluation, not a retrain.

The takeaway for the program is that **the tokenizer is part of the
measurement**. A model can be penalized for a tokenizer's surface behavior in a
way that has nothing to do with whether it learned the language, and exact-match
will report that penalty as if it were incompetence. This is the §1 observation
again, one level down.

### 3.4 Grokking and emergence: what this regime is and is not

It is tempting to connect "more training hurt the small model" to **grokking**
(Power et al., 2022; the model left training on modular arithmetic that suddenly
generalized long after memorizing, later explained mechanistically by Nanda et
al., 2023 as a learned Fourier/"numbers-on-a-clock" circuit). The connection is
weaker than it looks, because the word "overtraining" means two different things:

- **Different axis.** Grokking varies training *steps* on a *fixed* dataset; we
  vary *dataset size* at fixed steps. Our small model is not over-*stepped*, it
  is over-*corpused* for its capacity.
- **Different regime.** Grokking trains a small network *from scratch* on a clean
  algorithmic rule with a single discoverable circuit; we *fine-tune* a
  pretrained model that already knows how to code, on a broad, messy
  distribution with no one circuit to grok. The phase-transition conditions
  (a discoverable symmetry, weight-decay pressure, 10^4–10^6 steps) are absent.
- **Opposite promise.** Grokking says "keep going and generalization arrives";
  our result says "more data at fixed budget makes the small model worse."
  Training the 7B-at-2x far longer would, we predict, deepen memorization rather
  than trigger a late jump. That is a cheap, falsifiable experiment if anyone
  wants to run it.

So our small-model degradation is ordinary interference, not grokking-adjacent.
The honest bridge between this program and the grokking/emergence story is the
one in §1: **exact-match metrics make everything look more sudden than it is**,
in both directions.

### 3.5 The eval binary is part of the measurement too

A subtler version of §3.3 bit us directly. For a stretch the benchmark harness
built its Aether compiler from a **stale source** — the old monorepo, weeks behind
the split repo where the language actually evolves — so recently added constructs
(a missing TOON accessor among them) silently *could not compile*, and every model
that reached for them lost the task. The scores were real, but they measured a
**compiler that no longer existed**, not the models. The fix has two halves:
rebuild the eval compiler from the canonical source before each run, and — because
every generation is stored — **re-score** old runs by recompiling them with the
current compiler, no re-inference needed (recompiling is CPU-cheap; serving the
models is not). Re-scoring recovered exactly the affected tasks (the 14B-2x 29→30
above) and nothing else. The lesson generalizes §1 one more level: not just the
metric and the tokenizer, but the **toolchain that runs the generated code** is
part of the measurement, and it drifts if you let it.

### 3.6 Did the model learn the language, or the corpus's shapes?

The no-guide KPI asks whether a model can write Aether for tasks drawn like the
training corpus. That leaves a deeper question open: did the model learn the
*language*, or did it learn the corpus's program *shapes*? A model that only
memorized our templates would score well near the training distribution and
collapse on genuinely novel programs; a model that learned the language would
transfer.

We tested this with a held-out **hard set**: eight oracle-verified programs
whose *control-flow shapes* are novel relative to the corpus — not merely larger
versions of it. They include a two-pass cross-record statistic (compute a mean,
then flag each record by its deviation from it), a stateful sequential
simulation with rejection and clamping, nested traversal of an array inside an
array, an explicit two-state machine, adjacent-pair analysis with a running
streak, recursion, grouping with an argmax, and bucketing that returns the
winning *element* rather than a count. The scenarios are disjoint from both the
corpus and the easy benchmark, and there is **no retraining** — we ran the
existing fine-tunes unchanged, so this measures what they had already learned.

| model | hard (any-rep /8) | easy (/30) |
|---|---|---|
| Qwen3-Coder-30B-A3B MoE | **6/8** (1x), **7/8** (2x) | 29 |
| Qwen2.5-Coder-14B | 4/8 | 30 |
| Qwen2.5-Coder-7B | 4/8 | 28 |
| Mistral-Small-24B | 4/8 (1x), 3/8 (2x) | 29 |

Three findings.

1. **Generalization is real but partial.** Scores fall from ~93-100% on the easy
   benchmark to 38-88% on the hard set. They do *not* fall to zero — which pure
   template memorization would predict — so the models learned genuine,
   transferable Aether. They are simply brittle on the shapes that sit farthest
   from the corpus.
2. **The MoE generalizes best, and parameter count does not drive it.** The A3B,
   which was merely *inert* on the easy corpus-scaling law (§3.1, neither
   diluting nor gaining), is the *strongest* generalizer to novel shapes (6-7/8);
   the largest dense model, the 24B, is the *weakest* (3-4/8). This sharpens the
   active-versus-total reading of §3.1: total parameters buy both
   dilution-resistance *and* breadth of transfer, while active parameters buy
   easy-benchmark gain. Compositional generalization tracks breadth, not size.
3. **The instrument is well-calibrated.** Every hard task is solved by at least
   one model and missed by at least one; none is trivial and none is impossible.
   The hardest shapes for the whole cohort (adjacent-pair streak detection, and
   the bucket task that must return an element) are precisely the ones farthest
   from the corpus's habitual classify-and-accumulate shape.

This is the §1 observation raised to the level of the whole program: a single
in-distribution KPI cannot separate "learned the language" from "learned our
shapes," but a disjoint, harder instrument can, and it says the competence is
real with a brittle frontier. Whether *training* on hard shapes pushes that
frontier out is the live question in §5.

### 3.7 Reasoning before coding hurts a compact DSL

Reasoning ("thinking") models are now the dominant paradigm — the present, not the
future — so whether a model can be trained to *reason* its way to correct Aether is a
question the program has to answer. We fine-tuned a hybrid Qwen3-8B to emit a
reasoning block before its code, and evaluated it both with thinking enabled and
disabled, at matched sampling, across corpus sizes.

| corpus | thinking on | thinking off |
|---|---|---|
| 1x | 24 | 29 |
| 2x | 26 | 28 |
| 3x | 23 | 26 |

Thinking is worse at every corpus size, and more corpus does not rescue it: the
thinking-on scores do not trend upward (24, 26, 23). The hard set agrees (1/8
with thinking, 2/8 without).

The graded signal that explains *why* is a new one, in the spirit of §1.
Alongside any-rep score (does a task pass on *any* of three samples) we read
**all-rep consistency** (does it pass on *all* three). Thinking-off: 24/20/23 of
30. Thinking-on: 5/9/10. Thinking does not merely score lower — it
*destabilizes* the output. At temperature the model reasons its way to a
*different*, often non-compiling program on each sample, and run-ok confirms it
(thinking-on 52-61 of 90, thinking-off 75-84). Beyond run-ok, the spread between
any-rep and all-rep localizes a failure mode a single number hides: variance
injected by the reasoning itself.

The diagnosis is that our thinking-SFT taught the *form* of reasoning without
the *function*. The training traces were a base model rationalizing
already-correct code, which is reasoning theater rather than reasoning that
improves the program; we never trained the block to actually help, so it fills
with per-sample noise that derails the code.

The scope matters. This is a verdict on a *naive training approach* — not on
thinking models. Because reasoning models are the present, being able to train
them to write Aether well is a requirement the program has not yet met, not a
closed door. The unsolved version needs a better teacher (traces distilled from
a model that genuinely reasons about code, not the base rationalizing a known
answer) and outcome-shaped reasoning (reinforcement on the real signal, compile
and exact-match, so the reasoning is rewarded only when it helps), paired with
tasks hard enough to be worth reasoning about (§3.6).

### 3.8 Capability injection: training on hard shapes, where there is room

§3.6 left a question — the fine-tunes are brittle on novel hard shapes, so can
*training* on such shapes push the frontier out? We tested it directly. We
generated 32 oracle-verified programs in the same eight shapes as the hard
benchmark but in disjoint scenarios, merged them into the corpus, and retrained
the two models at the ends of the generalization spectrum: the dense 24B (the
*weakest* hard generalizer at 3–4/8, so the most room) and the A3B MoE (the
*best* at 6–7/8, so the least). The outcome is sharply architecture-dependent:

| model | corpus | hard /8 | easy /30 |
|---|---|---|---|
| 24B dense | 2x (baseline) | 3 | 29 |
| 24B dense | **2x + large** | **7** | 29 |
| A3B MoE | 2x (baseline) | 7 | 29 |
| A3B MoE | 2x + large | 6 | 24 |

The dense 24B **gained four hard tasks** (3 → 7) while holding the easy bench
exactly (29 → 29), reaching 7/8 — the best score anything posts on the hard set.
The MoE did the reverse: no hard gain (it was already near its ceiling) and a
five-point easy dilution; trimming its base corpus to make room (a 0.75x variant)
did not rescue it, because the easy loss tracks the hard examples themselves, not
the corpus size. The reading is the §3.1 rule restated for transfer: injection
works only where there is **absorption headroom** — a dense model that, by "scale
the corpus to the model," can take the *larger* base *plus* the new examples. The
saturated MoE, corpus-insensitive by §3.1, has nowhere to put them and only loses
ground. The win is real but **bounded**: injection closed the dense model's gap
*to the MoE's level*, not past it. The frontier moved; it did not vanish.

---

## 4. Methodological takeaways

1. **Never report exact-match alone.** Pair it with run-ok (graded execution).
   Exact-match falling *with* run-ok is real capability loss; exact-match falling
   *without* it is a surface or metric artifact. The pair localizes the failure;
   the single number hides it.
2. **Treat consistent, capable-model errors as language-design signal.** Add the
   construct the models reach for when it is natural and missing; ignore one-off
   mistakes from weak models. The benchmark, not taste, adjudicates.
3. **Scale the corpus to the model (dense models).** Small dense models dilute
   on a larger corpus; large ones absorb it; sparse MoEs are roughly
   corpus-insensitive.
4. **The tokenizer is part of the measurement.** A family-specific surface
   behavior can masquerade as incompetence under exact-match.
5. **So is the toolchain.** Pin the eval compiler to the canonical source and
   rebuild it before each run; a stale compiler silently caps scores. Because
   generations are stored, re-score against a fixed compiler rather than
   re-running the models.
6. **Graded signals beyond run-ok localize distinct failures.** Run-ok separates
   a surface defect from capability loss (§1); all-rep consistency separates
   stable competence from reasoning-injected variance (§3.7); transfer to a
   disjoint hard set separates a learned language from learned templates (§3.6).
   Each is a different lens on the gap exact-match hides; collect more than one.

---

## 5. Status and open questions

- **Mistral-24B is resolved.** The §3.3 space-dropping was a vLLM/Tekken serving
  bug, not the weights; serving with the native tokenizer gives a clean 27/30 (1x)
  and 29/30 (2x), in-band with the Qwen dense models.
- **The 3x sweep is done** (§3.1): the 14B and 24B both peak at 2x, but the 3x
  penalty shrinks with size (the 14B falls 6 points, the 24B only 2), so the
  corpus-optimum is still creeping right. **Open question:** a dense model larger
  than 24B should be the first to actually prefer 3x; one >24B run would tell us
  whether the optimum reaches 3x before any 3x/4x corpus is worth building.
- **Best model to date** is the 14B at 2x with the §3.2 language additions, at
  **30/30** on the no-guide benchmark.
- **The generalization frontier is mapped (§3.6).** Existing fine-tunes transfer
  to novel hard shapes only partially (38-88% against 93-100% on the easy set),
  and the MoE, not the largest dense model, transfers best. The live experiment
  is **capability injection**: retraining the 24B and the A3B on the corpus plus
  ~30 oracle-verified hard examples (in scenarios disjoint from the hard
  benchmark) to test whether training on hard shapes pushes the frontier out
  without diluting the easy bench. **Resolved (§3.8):** the gap *closes* on the
  dense 24B (hard 3 → 7, easy flat at 29), reaching the MoE's level; the same
  examples only *diluted* the saturated MoE (no hard gain, easy 29 → 24, and a
  0.75x base-trim did not help). Injection is an absorption-capacity tool, not a
  universal one — it pays where §3.1 says corpus would, on a dense model with
  room.
- **Reasoning-before-coding is net-negative under our current training (§3.7),
  and that is an open problem, not a closed one.** Thinking hurts at every corpus
  size and on the hard set, and the all-rep consistency collapse shows it injects
  variance. Training reasoning models to write Aether well still needs a better
  teacher and outcome-shaped reinforcement; it stays a required capability
  because reasoning models are the present paradigm, not a discarded direction.

---

## References

**External literature.**

- Hoffmann et al., *Training Compute-Optimal Large Language Models* (Chinchilla),
  2022, arXiv:2203.15556.
- Zhou et al., *LIMA: Less Is More for Alignment*, 2023, arXiv:2305.11206.
- Muennighoff et al., *Scaling Data-Constrained Language Models*, 2023,
  arXiv:2305.16264.
- Power et al., *Grokking: Generalization Beyond Overfitting on Small Algorithmic
  Datasets*, 2022, arXiv:2201.02177.
- Nanda et al., *Progress Measures for Grokking via Mechanistic Interpretability*,
  2023, arXiv:2301.05217.
- Wei et al., *Emergent Abilities of Large Language Models*, 2022,
  arXiv:2206.07682.
- Schaeffer et al., *Are Emergent Abilities of Large Language Models a Mirage?*,
  2023, arXiv:2304.15004.

**In this repository.**

- [`aether_architecture_and_rationale.md`](aether_architecture_and_rationale.md)
  — the as-built rationale and the benchmark-as-instrument thesis.
- [`aether_for_llms_and_others.md`](aether_for_llms_and_others.md) — the full
  reference guide (the artifact the "none" KPI withholds).
- [`aether_for_llms_with_small_contexts.md`](aether_for_llms_with_small_contexts.md)
  — the condensed guide for small context windows.
- [`../src/aether/DESIGN.md`](../src/aether/DESIGN.md) — forward-looking design
  vision.
- [`../src/aether/translate.c`](../src/aether/translate.c) — lowering for the
  model-driven additions (`toon_key_or`, `toon_null`, record literals).
- [`../tests/`](../tests) — conformance programs exercising the idioms above.
