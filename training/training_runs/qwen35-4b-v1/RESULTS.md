# Qwen3.5-4B fine-tune, no guide: `qwen35-4b-v1` (2026-10-21), the first small model

`qwen35-4b-v1` is `qwen38-27b-v1`'s recipe and data on Qwen3.5-4B (instruct, Apache-2.0, the same tokenizer and
`qwen3_5` architecture family as Qwen3.8-27B). The one change the model forces: bf16 LoRA instead of 4-bit, as
Unsloth advises for Qwen3.5.

| | `qwen38-27b-v1` | **`qwen35-4b-v1`** |
|---|---|---|
| base | Qwen3.8-27B, 4-bit | Qwen3.5-4B, bf16 |
| dataset | `v1-20261021`: 1981 instruction (220 Python) + 87 repair | the same |
| LoRA | r=32, α=64, seed 3407, 60-record stopping split every 10 steps, patience 3 | the same |
| stopped at | epoch 1.04; best checkpoint 230 (epoch 0.92, eval loss 0.0780) | epoch 1.04; best checkpoint 230 (epoch 0.92, eval loss 0.1110) |
| training time | 2 h 07 min | 22 min |

**Grader.** Everything is graded on 2026-10-20-12 (`1.0-beta2`). Scoring: no guide, T=0, seed 42, one repeat, two
repair rounds, thinking off.
- Main suites: vLLM on host-c.
- Held-out suites, the Python check and both baselines: Ollama (bf16) on the scoring Mac.
- The baselines are new for this run, on the same grader:
  - `../guided-qwen35-4b-1.0-beta2/`: the untrained 4B with the 1.0-beta2 medium guide;
  - `../base-qwen35-4b-python-20261021/`: the untrained 4B's Python check.
  - The untrained 4B with no guide scores 0 on every Aether suite (measured on 2026-10-19-3).

FA is the first attempt exact; FX is exact after repairs.

## Aether, first attempt (after repair)

| suite | untrained, + 1.0 guide | **`qwen35-4b-v1`, no guide** | `qwen38-27b-v1`, no guide |
|---|---|---|---|
| tasks_v2_pos (35) | 32 (33) | 31 (32) | 35 (35) |
| tasks_hard_v2 (14) | 7 (8) | **11 (11)** | 14 (14) |
| tasks_hard_nontoon (5) | 0 (0) | 0 (0) | 3 (4) |
| tasks_cs (19) | 7 (9) | **15 (15)** | 19 (19) |
| **main (73)** | **46 (50)** | **57 (58), 78%** | 71 (72) |
| tasks_frontier (15) | 5 (6) | 4 (6) | 9 (12) |
| tasks_frontier_algo (14) | 2 (2) | **5 (5)** | 10 (13) |
| tasks_frontier_spec (12) | 4 (4) | 3 (3) | 10 (12) |
| tasks_sealed_toon (12) | 1 (1) | 2 (2) | 8 (9) |
| **held-out (53)** | **12 (13)** | **14 (16), 26%** | 37 (46) |

On the 2026-10-19-3 guide and grader the guided 4B scored 52 (main) and 13 (held-out); on the 1.0-beta2 guide it
scores 46 and 12, losing 4 on tasks_cs and 2 on tasks_sealed_toon. The guide's content changed with the 1.0 round
(renames, maps); whether that or noise accounts for it is not separated here.

## Python, same model, first attempt (after repair)

| suite | untrained 4B | **`qwen35-4b-v1`** |
|---|---|---|
| tasks_v2_pos (35) | 22 (27) | 13 (13) |
| tasks_hard_v2 (14) | 3 (4) | 1 (1) |
| tasks_hard_nontoon (5) | 1 (1) | 0 (0) |
| tasks_cs (19) | 13 (13) | 14 (14) |
| tasks_frontier (15) | 11 (12) | 6 (6) |
| tasks_frontier_algo (14) | 7 (12) | 6 (7) |
| tasks_frontier_spec (12) | 4 (5) | 1 (2) |
| tasks_sealed_toon (12) | 4 (6) | 0 (0) |
| **total (126)** | **65 (80)** | **41 (43)** |

Two untrained-4B Python cases (`toon_type_probe`, `mstream_field_parse`) return only the end marker at T=0, every
time. The harness files an empty answer as an infrastructure failure and withholds those suites' headline; they
are counted here as misses, and a re-run reproduces them.

## Reading

- **Without a guide the 4B beats its guided self.** 57 against 46 on the main suites and 14 against 12 held-out,
  with no guide in the prompt against a ~14K-token one. The gain is in the harder suites: tasks_hard_v2 11 against
  7 and tasks_cs 15 against 7.
- **It is far behind the 27B**: 14 of 53 held-out against 37, and nothing on tasks_hard_nontoon.
- **Repair barely helps the 4B**: +1 main and +2 held-out after two rounds, against +1 and +9 for the 27B. Of its
  55 first-attempt misses, 27 are coded compiler or runtime errors with a hint, 27 are wrong output from a program
  that ran, and 1 is a timeout. So the 4B usually gets a diagnostic and still cannot act on it.
- **Python forgetting, as for the 27B**: 65 → 41 first attempts; after repair 80 → 43, because the fine-tuned 4B
  also stops repairing its Python. The card for any 4B release must say Aether only.
- **Next:** `qwen35-4b-v2` adds `qwen38-27b-v1`'s verified programs to the data (one change); it tests whether a
  stronger teacher's volume moves a 4B that the guided solvers' data took this far.

## Files

- `score/`
  - `tuned__*`: main suites, host-c.
  - `m5ollama__*`: held-out suites, scoring Mac.
  - `python__*`: the Python check.
- `train/run_metadata.json`: the training run as executed.
