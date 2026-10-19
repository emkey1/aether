# Qwen3.8-27B fine-tune, no guide: run 6 (2026-10-18/19)

`qwen38-27b-r6` keeps run 5's recipe and adds data aimed at run 5's main-suite failure classes. It also changes
the stopping split. Both language and data move to 2026-10-18-2 (D66: `mstream_write_text` /
`mstream_write_byte`).

| | run 5 `qwen38-27b-modern` | run 6 `qwen38-27b-r6` |
|---|---|---|
| dataset | `modern-20261018`: 1445 instruction + 64 repair | `r6-20261018`: 1681 instruction (220 Python, capped from 663) + 87 repair |
| early stopping | 12 held-out records, every 5 steps, patience 2 | 60 held-out records, every 10 steps, patience 3 |
| stopped at | epoch 0.40, best 0.35 | epoch 1.17, best checkpoint 220 (epoch 1.03, eval loss 0.0793) |
| training time | 46 min | 2 h 05 min |

Data added since run 5:
- a targeted pack of 108 pairs and 23 repair drills: file reads inside `fx` (60 + 12), MStream write verbs,
  module imports, tuple `@post` and array value semantics. It was written from failure classes only, never from
  task text;
- `d2m`: 18 pairs, d2 tasks the guided base could not solve, solved by MiMo with the guide;
- `d3`: 90 pairs in the six weakest categories, solved by the guided base (48) or MiMo (42).

The 60-record split rode through the noise-sized rises that stopped run 5 (0.0965 → 0.0977 at epoch 0.37, below).
Eval loss kept falling until epoch 1.03.

Scoring: no guide, T=0, seed 42, one repeat, two repair rounds, thinking off. Graded on 2026-10-18-2.
- Main suites: vLLM on host-c, plus Ollama on the scoring Mac (the `nocard__` and `fixit__` runs).
- Held-out suites and the Python check: Ollama on the scoring Mac.

FA is the first attempt exact; FX is exact after repairs.

## Main suites (73), first attempt (after repair)

| suite | run 5 | **run 6** (vLLM) | run 6 (Ollama) | run 6 + fix-it card (Ollama) | guided base |
|---|---|---|---|---|---|
| tasks_v2_pos (35) | 28 (32) | 32 (35) | 33 (35) | 32 (34) | 34 (35) |
| tasks_hard_v2 (14) | 11 (14) | 13 (14) | 14 (14) | 14 (14) | 14 (14) |
| tasks_hard_nontoon (5) | 2 (3) | 3 (5) | 4 (5) | 3 (4) | 4 (5) |
| tasks_cs (19) | 18 (19) | 18 (19) | 18 (19) | 19 (19) | 15 (17) |
| **total** | **59 (68)** | **66 (73)** | **69 (73)** | **68 (71)** | **67 (71)** |

With no guide, run 6 matches the guided base (the untrained Qwen3.8 with the ~14K-token medium guide in every
prompt) on the first attempt, and beats it after repair.

## Held-out suites (53)

| suite | run 3 | run 5 | **run 6** | guided base |
|---|---|---|---|---|
| tasks_frontier (15) | 9 (11) | 9 (11) | 11 (12) | 15 (15) |
| tasks_frontier_algo (14) | 10 (12) | 10 (11) | 9 (13) | 11 (13) |
| tasks_frontier_spec (12) | 9 (11) | 7 (11) | 9 (11) | 11 (12) |
| tasks_sealed_toon (12) | 4 (8) | 4 (8) | 7 (9) | 11 (12) |
| **total** | **32 (42)** | **30 (41)** | **36 (45)** | **48 (52)** |

Run 3 was graded on 2026-10-16-3. Replayed on the current language it scores lower (see run 5's RESULTS.md).
- Run 6 is the best no-guide result on the held-out suites so far.
- The gap to the guided base on held-out (12 first attempts) is larger than on main, where there is none.
  The model generalises less well than it fits.

## Python, same model, first attempt (after repair)

| suite | untrained base | run 4 | run 5 | **run 6** |
|---|---|---|---|---|
| tasks_v2_pos (35) | 29 (33) | 18 (33) | 29 (33) | **18 (29)** |
| tasks_hard_v2 (14) | 10 (13) | 11 (14) | 14 (14) | 10 (12) |
| tasks_hard_nontoon (5) | 5 (5) | 4 (5) | 5 (5) | 4 (5) |
| tasks_cs (19) | 19 (19) | 19 (19) | 19 (19) | 17 (19) |
| tasks_frontier (15) | 15 (15) | 7 (14) | 14 (15) | 10 (14) |
| tasks_frontier_algo (14) | 13 (14) | 13 (14) | 14 (14) | 13 (14) |
| tasks_frontier_spec (12) | 12 (12) | 12 (12) | 12 (12) | 10 (12) |
| tasks_sealed_toon (12) | 11 (12) | 11 (12) | 11 (12) | 11 (12) |

**The Aether leak into Python is back.** Ten of the 17 core first-attempt failures are Aether syntax in a
Python answer (`fn main`, `let x: Int`, `fx {`, `ret`). The cause is a train/serve mismatch:
- In training, every record carries a system message: "You generate canonical Aether…" or "You write correct
  Python 3…".
- In scoring and in normal use, the harness sends only the user message. So the model learned that a Python
  system message means Python, and with no system message it falls back on its Aether habit.
- Run 5 trained a third as long, and the habit had not yet set.

## Reading

- **The targeted data worked.**
  - FX-001 (file reads outside `fx`): 4–7 main-suite misses in runs 5/5b, 1 in run 6.
  - The MStream, module-import and tuple-`@post` misses are gone.
  - Run 6's main-suite first-attempt misses on vLLM: one FX-001, one array mutated through a parameter, an
    invented `is_digit` (Aether has no character-class builtins), one compiler gap, one label slip, and two
    tasks the guided base also fails. The compiler gap is a `ToonNode` indexed with `[]`, which compiles and
    then crashes the VM.
  - All 7 are fixed in the repair round.
- **Serving noise:** the same model scored 66 on vLLM and 69 on Ollama, within the ±7 noise found in run 5.
- **The fix-it card made no difference.** It is 5 rules from run 6's main-suite misses, about 250 tokens,
  `training/config/fixit_card.md`. It scored 68 (71) with the card against 69 (73) without, on the same serving.
  This is the second card with no effect; a fine-tuned model does not use a rule it is handed in the prompt.
- **Next (one change per run):** remove the system-message mismatch, by training without a system message or
  scoring with the training one. Then the Python numbers can be trusted for the model card.

## Files

`score/`:
- `tuned__*`: main suites, vLLM, host-c.
- `m5ollama__*`: held-out suites, scoring Mac.
- `python__*`: the Python check.
- `nocard__*` and `fixit__*`: main suites on the scoring Mac, without and with the fix-it card.
