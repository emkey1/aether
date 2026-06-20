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
  rule; the MoE is corpus-insensitive (the stable, if unspectacular, pick).
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
going 1x→2x. The 24B then **bounds** it: a different family at a larger size, it
*also* peaks at 2x (29→27 at 3x), so the optimum plateaus rather than marching
right indefinitely; with more total parameters, though, it dilutes far more gently
(−2 vs the 14B's −6) — robustness past the peak, not a higher peak. So the
Chinchilla *direction* holds from 7B to 14B and then stops; "bigger always wants
more" does not; this is squarely the LIMA / data-constrained regime, not
compute-optimal pretraining.

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

---

## 5. Status and open questions

- **Mistral-24B is resolved.** The §3.3 space-dropping was a vLLM/Tekken serving
  bug, not the weights; serving with the native tokenizer gives a clean 27/30 (1x)
  and 29/30 (2x), in-band with the Qwen dense models.
- **The 3x sweep is done** (§3.1): the dense corpus-optimum is bounded at 2x for
  the 14B (it dilutes to 24 at 3x), so 4x is not worth running on this model.
- **Best model to date** is the 14B at 2x with the §3.2 language additions, at
  **30/30** on the no-guide benchmark.

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
