# Qwen3.8-27B fine-tune on the modern surface, no guide (run 5, 2026-10-18)

`qwen38-27b-modern` is the first run on language 2026-10-18-1: the Pascal forms are gone (D55), the modern
stdlib, quantifiers and nested functions are in (D56-D65). Same recipe as `qwen38-27b-distill` (run 4); the data
changed. `qwen38-27b-modern-p4` (run 5b) is the same run with early-stopping patience 4 instead of 2.

| | run 5 `qwen38-27b-modern` | run 5b `qwen38-27b-modern-p4` |
|---|---|---|
| dataset | `modern-20261018`: 1445 instruction (200 Python, capped from 555) + 64 repair | same |
| new data | d2-20261018: 269 Aether + 269 Python pairs, about a third JSON/data-file tasks | same |
| recipe | 4-bit LoRA r=32 a=64, lr 1e-4, seed 3407, early stopping on 12 held-out records, patience 2 | patience 4 |
| stopped at | epoch 0.40; best checkpoint 65 (epoch 0.35, eval loss 0.0923) | epoch 0.45; best checkpoint still 65 |

Run 5 stopped on two eval-loss rises of +0.0002 and +0.0005. Run 5b ran on and found no lower loss, so both
ship checkpoint 65: the same data, seed and step. Their weights differ only by GPU numerics.

Scoring: no guide, T=0, seed 42, one repeat, two repair rounds, thinking off. FA = first attempt exact, FX = exact
after repairs. Graded on aether 2026-10-18-1. Main suites served by vLLM (bf16) on host-c; held-out suites and the
Python check by Ollama (bf16 import of the same merged export) on the scoring Mac.

## Main suites (73), first attempt (after repair)

| suite | run 5 | run 5b | run 5 + card | guided base |
|---|---|---|---|---|
| tasks_v2_pos (35) | 28 (32) | 28 (33) | 28 (32) | 34 (35) |
| tasks_hard_v2 (14) | 11 (14) | 6 (14) | 5 (14) | 14 (14) |
| tasks_hard_nontoon (5) | 2 (3) | 2 (3) | 4 (5) | 4 (5) |
| tasks_cs (19) | 18 (19) | 16 (18) | 18 (19) | 15 (17) |
| **total** | **59 (68)** | **52 (68)** | **55 (70)** | **67 (71)** |

- "guided base" is the untrained Qwen3.8-27B with the 2026-10-18-1 medium guide in the prompt. The reports are in
  `../guided-qwen38-27b-20261018/`.
- "+ card" gives run 5 the 1.5K-token `docs/aether_card.md` as its guide.

## Held-out suites (53), first attempt (after repair)

| suite | run 5 | guided base |
|---|---|---|
| tasks_frontier (15) | 9 (11) | 15 (15) |
| tasks_frontier_algo (14) | 10 (11) | 11 (13) |
| tasks_frontier_spec (12) | 7 (11) | 11 (12) |
| tasks_sealed_toon (12) | 4 (8) | 11 (12) |
| **total** | **30 (41)** | **48 (52)** |

Run 5b was not scored on these: it is run 5's checkpoint.

## Python, same model, first attempt (after repair)

| suite | untrained base | run 4 | run 5 |
|---|---|---|---|
| tasks_v2_pos (35) | 29 (33) | 18 (33) | 29 (33) |
| tasks_hard_v2 (14) | 10 (13) | 11 (14) | 14 (14) |
| tasks_hard_nontoon (5) | 5 (5) | 4 (5) | 5 (5) |
| tasks_cs (19) | 19 (19) | 19 (19) | 19 (19) |
| tasks_frontier (15) | 15 (15) | 7 (14) | 14 (15) |
| tasks_frontier_algo (14) | 13 (14) | 13 (14) | 14 (14) |
| tasks_frontier_spec (12) | 12 (12) | 12 (12) | 12 (12) |
| tasks_sealed_toon (12) | 11 (12) | 11 (12) | 11 (12) |

Run 4 trained on Aether alone, and Aether syntax leaked into its Python. Run 5 adds 200 Python pairs (MiMo's
independent programs for the distillation tasks), and that recovers the base model's Python.

## Reading

**Run-to-run noise is about ±7 first-attempt tasks on the main suites, and one habit causes it.**
- Runs 5 and 5b are the same checkpoint, yet they differ by 7 tasks. All but two of those are in tasks_hard_v2.
- Every hard_v2 difference between run 5, run 5b and the card run is FX-001: `toon_parse_file` called outside
  an `fx` block. The repair round fixes each one: all three runs score 14/14 there after repair.
- So the model sits on the boundary of that habit. Main-suite differences of a few tasks between runs are within
  this noise. That includes runs 3, 4 and 5 below.

**Compared on one grader, run 5 holds level with the best earlier run.**

| first attempt (after repair) | run 3 `qwen38-27b-algo` | run 4 `qwen38-27b-distill` | run 5 |
|---|---|---|---|
| graded on | 2026-10-16-3 | 2026-10-17-1 | 2026-10-18-1 |
| main (73) | 64 (71) | 56 (71) | 59 (68) |
| held-out (53) | 32 (42) | 27 (41) | 30 (41) |

- Re-grading run 3's archived programs on 2026-10-18-1 (`tools/replay_bench.py`) takes its first-attempt total
  from 96 to 87 of 126. Twelve of its programs called `copy`, a removed Pascal form.
- Run 5 scores 89 of 126 on that grader, having seen only about a third of its data. Run 5 writes the modern
  forms throughout, and its Python is intact.

**The card does not help.**
- It stated the `fx` rule, yet the card run made the FX-001 slip on the same tasks as run 5b.
- The trained habit outweighs a rule stated in the prompt, so the fix belongs in the training data.

**Run 5's first-attempt misses that the guided base solves, main suites only** (the held-out suites are not mined):

| misses | class | cause |
|---|---|---|
| 4 | FX-001 | file reads outside `fx` |
| 1 | SCOPE-001 | an invented MStream verb; the data never wrote text to an MStream |
| 2 | SCOPE-001 | module imports |
| 1 | ANN-001 | tuple `@post` slots |
| 2 | wrong output | array value semantics (an in-place heap sort helper); matrix spiral |

Run 6 (`qwen38-27b-r6`) carries the responses:
- a targeted pack for these classes, built from the classes alone and never from task text;
- decision D66, which renames the MStream verbs to `mstream_write_text` / `mstream_write_byte` (the name the model
  reached for);
- d2's unsolved tasks, solved by MiMo with the guide, plus a batch aimed at the categories with the weakest yield;
- a 60-record early-stopping split.

## Files

- `score/`
  - `tuned__*`: main suites, host-c.
  - `m5ollama__*`: held-out suites, scoring Mac.
  - `python__*`: the Python check.
  - `card__*`: the card experiment.
- `../qwen38-27b-modern-p4/score/`: run 5b's main suites.
- `../guided-qwen38-27b-20261018/score/guided__*`: the guided base, all eight suites.
