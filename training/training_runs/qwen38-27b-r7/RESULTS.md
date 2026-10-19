# Qwen3.8-27B fine-tune, no guide: run 7 (2026-10-19), the release candidate

`qwen38-27b-r7` is run 6 with one change: no training record carries a system message. Run 6 trained with a system
turn on every record (an Aether one or a Python one). The harness and ordinary use send none, so the question was
whether that mismatch caused run 6's Python to fall back on Aether. The data, recipe and stopping split are run 6's.

| | run 6 `qwen38-27b-r6` | run 7 `qwen38-27b-r7` |
|---|---|---|
| dataset | `r6-20261018`: 1681 instruction (220 Python) + 87 repair, each with a system turn | `r7-20261019`: the same records, no system turn |
| stopped at | epoch 1.17; best checkpoint 220 (epoch 1.03, eval loss 0.0793) | epoch 1.31; best checkpoint 250 (epoch 1.17, eval loss 0.0782) |
| training time | 2 h 05 min | 2 h 17 min |

**Grader.** Everything below is graded on aether 2026-10-19-3, the language deployed while run 7 waited to be
scored. Three changes landed in that time:
- D67 added the character-class builtins.
- `[]` on an opaque handle became a compile-time error.
- A method spelling that cannot reach its function became SCOPE-001.

Run 6's archived programs and the guided base's were re-graded on 2026-10-19-3 with `tools/replay_bench.py`, so all
three columns share one grader. First attempts only; the repairs are as run.
- Run 6: 2 first attempts flip fail→pass (both D67 `is_digit`), 0 the other way.
- Guided base: 1 flips pass→fail (a program that now gets the handle-index error).
- The replay reports are in `replay/`.

Scoring: no guide, T=0, seed 42, one repeat, two repair rounds, thinking off.
- Main suites: vLLM on host-c.
- Held-out suites and the Python check: Ollama on the scoring Mac.

FA is the first attempt exact; FX is exact after repairs.

## Main suites (73), first attempt (after repair)

| suite | run 6 | **run 7** | guided base |
|---|---|---|---|
| tasks_v2_pos (35) | 32 (35) | **33 (35)** | 34 (35) |
| tasks_hard_v2 (14) | 13 (14) | 13 (14) | 14 (14) |
| tasks_hard_nontoon (5) | 4 (5) | 4 (5) | 4 (5) |
| tasks_cs (19) | 18 (19) | 18 (19) | 15 (17) |
| **total** | **67 (73)** | **68 (73), 93% (100%)** | **67 (71)** |

"Guided base" is the untrained Qwen3.8-27B with the ~14K-token medium guide in every prompt (reports:
`../guided-qwen38-27b-20261018/`). With no guide, run 7 matches it on the first attempt and solves every task after
repair.

## Held-out suites (53)

| suite | run 6 | **run 7** | guided base |
|---|---|---|---|
| tasks_frontier (15) | 11 (12) | 10 (13) | 15 (15) |
| tasks_frontier_algo (14) | 10 (13) | 10 (13) | 11 (13) |
| tasks_frontier_spec (12) | 9 (11) | **12 (12)** | 11 (12) |
| tasks_sealed_toon (12) | 7 (9) | 7 (7) | 10 (11) |
| **total** | **37 (45)** | **39 (45), 74% (85%)** | **47 (51)** |

The held-out suites were never used to choose or write training data. Run 7 is the best no-guide result on them.
The gap to the guided base is 8 first attempts: 18 for run 5, 10 for run 6.

## Python, same model, first attempt (after repair)

| suite | untrained base | run 6 | **run 7** |
|---|---|---|---|
| tasks_v2_pos (35) | 29 (33) | 18 (29) | 16 (30) |
| tasks_hard_v2 (14) | 10 (13) | 10 (12) | 11 (14) |
| tasks_hard_nontoon (5) | 5 (5) | 4 (5) | 3 (5) |
| tasks_cs (19) | 19 (19) | 17 (19) | 18 (19) |
| tasks_frontier (15) | 15 (15) | 10 (14) | 3 (12) |
| tasks_frontier_algo (14) | 13 (14) | 13 (14) | 11 (13) |
| tasks_frontier_spec (12) | 12 (12) | 10 (12) | 12 (12) |
| tasks_sealed_toon (12) | 11 (12) | 11 (12) | 11 (11) |

**Dropping the system message did not stop the leak.**
- Every failed Python first attempt on tasks_v2_pos (16 of 19) and tasks_frontier (12 of 12) is Aether syntax:
  `fn main`, `let x: T`, `fx {`, `ret`.
- The repair round, which shows the Python error, recovers most of them.
- So the leak is catastrophic forgetting from training to ~1.2 epochs on a language that looks like Python, not a
  prompt-format artefact. Run 5, at 0.35 epoch, had not yet lost its Python.
- Owner decision (2026-10-19): the release is an Aether specialist. The model card states the Python numbers and
  recommends the base model, or the adapter unloaded, for other languages.

## Reading

- **Release candidate.** Run 7 is the best on every Aether measure.
  - It has no system-prompt requirement, which makes it the simplest to serve.
  - Two more training seeds (`qwen38-27b-r7-s2`, `-s3`) measure run-to-run variation for the model card.
- **Run 6 vs run 7 on Aether** (+1 main, +2 held-out) is inside the noise found in run 5. Removing the system
  message neither helped nor hurt Aether.
- **Run 7's 5 first-attempt misses on the main suites, all fixed by repair:**
  - `mstream_len` instead of `mstream_length` (SCOPE-001; the compiler hint names it);
  - `toon_get_int_or` with 4 arguments instead of 3 (BUILT-002);
  - an in-place heap-sort helper that mutates an array parameter (ARR-001: value semantics);
  - wrong output on `tuple_recursive_digits` and `cs_nqueens`, which the guided base also fails.
  - No file read outside `fx` this time; that was the dominant miss of runs 5/5b.

## Files

- `score/`
  - `tuned__*`: main suites, host-c.
  - `m5ollama__*`: held-out suites, scoring Mac.
  - `python__*`: the Python check.
- `replay/`: run 6 and the guided base re-graded on 2026-10-19-3.
