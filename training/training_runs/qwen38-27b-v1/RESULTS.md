# Qwen3.8-27B fine-tune, no guide: v1 (2026-10-21), the Aether 1.0-beta refresh

`qwen38-27b-v1` is run 7's recipe on the Aether 1.0-beta data. Run 7 (`qwen38-27b-r7`) is the model published on
2026-10-19. It was trained on 2026-10-18-2 and lost 8 first attempts when D72 renamed `formatfloat` to
`format_float` (see `../qwen38-27b-r7/RESULTS.md`, "On later languages"). Only the data and the language changed;
LoRA r=32/α=64 on the 4-bit base, the 60-record stopping split checked every 10 steps, patience 3, seed 3407, no
system message.

| | run 7 `qwen38-27b-r7` | **v1 `qwen38-27b-v1`** |
|---|---|---|
| dataset | `r7-20261019` on 2026-10-18-2: 1681 instruction (220 Python) + 87 repair | `v1-20261021` on 2026-10-20-12 (1.0-beta2): 1981 instruction (220 Python) + 87 repair |
| new in the data | | distilled batches d2m/d3/d4 on 1.0, the 18 D68 map seed pairs, the 84-pair standard-library pack (plan step 2b) |
| Aether records using maps / `format_float` / `formatfloat` | 0 / 0 / 178 | 81 / 208 / 0 |
| stopped at | epoch 1.31; best checkpoint 250 (epoch 1.17, eval loss 0.0782) | epoch 1.04; best checkpoint 230 (epoch 0.92, eval loss 0.0780) |
| training time | 2 h 17 min | 2 h 07 min |

The cursor-sharing pack and PARAM-001 (D77, 2026-10-21) came after this dataset was built; v1 has neither.

**Grader.** Everything below is graded on 2026-10-20-12 (`1.0-beta2`, the tagged build on the hosts). The
graders' VM is pscal-core 785f2db, before the VM audit; the audited VM is not deployed until the sanity check
(plan step 6).
- Run 7, its two seeds and the guided base were trained or scored on 2026-10-19-3. Their archived programs were
  re-graded on 2026-10-20-12 with `tools/replay_bench.py` (`replay/`), so every column shares one grader.
- Every pass→fail flip in those replays is a D72 rename: `formatfloat` (8 for run 7, 12 and 9 for the seeds, 12 for
  the guided base) and one `mstream_append_text` (guided base). Nothing else moved.
- The guided base's replay understates it: its guide was the 2026-10-19-3 guide, which taught the old names. The
  "guided base, 1.0" column is a fresh run with the 1.0-beta2 medium guide (`../guided-qwen38-27b-1.0-beta2/`).

Scoring: no guide, T=0, seed 42, one repeat, two repair rounds, thinking off.
- Main suites: vLLM on host-c.
- Held-out suites and the Python check: Ollama on the scoring Mac.

FA is the first attempt exact; FX is exact after repairs. A replay re-grades first attempts only: a first attempt
that passed was never repaired, so replayed columns give no after-repair count.

## Main suites (73), first attempt (after repair)

| suite | run 7, replayed | **v1** | guided base, 1.0 |
|---|---|---|---|
| tasks_v2_pos (35) | 31 | **35 (35)** | pending |
| tasks_hard_v2 (14) | 10 | **14 (14)** | pending |
| tasks_hard_nontoon (5) | 3 | 3 (4) | pending |
| tasks_cs (19) | 18 | **19 (19)** | pending |
| **total** | **62** (as published: 68) | **71 (72), 97% (99%)** | pending |

Run 7's seeds replay to 62 (`-s2`) and 61 (`-s3`) on the main suites.

## Held-out suites (53)

| suite | run 7, replayed | **v1** | guided base, 1.0 |
|---|---|---|---|
| tasks_frontier (15) | 10 | 9 (12) | pending |
| tasks_frontier_algo (14) | 10 | 10 (13) | pending |
| tasks_frontier_spec (12) | 12 | 10 (12) | pending |
| tasks_sealed_toon (12) | 5 | 8 (9) | pending |
| **total** | **37** (as published: 39) | **37 (46), 70% (87%)** | pending |

Run 7's seeds replay to 35 (`-s2`) and 38 (`-s3`) on the held-out suites.

The held-out suites were never used to choose or write training data.

## Python, same model, first attempt (after repair)

| suite | untrained base | run 7 | **v1** |
|---|---|---|---|
| tasks_v2_pos (35) | 29 (33) | 16 (30) | 14 (30) |
| tasks_hard_v2 (14) | 10 (13) | 11 (14) | 6 (13) |
| tasks_hard_nontoon (5) | 5 (5) | 3 (5) | 3 (5) |
| tasks_cs (19) | 19 (19) | 18 (19) | 16 (19) |
| tasks_frontier (15) | 15 (15) | 3 (12) | 3 (13) |
| tasks_frontier_algo (14) | 13 (14) | 11 (13) | 10 (14) |
| tasks_frontier_spec (12) | 12 (12) | 12 (12) | 11 (12) |
| tasks_sealed_toon (12) | 11 (12) | 11 (11) | 4 (10) |
| **total (126)** | **114 (123)** | **85 (116)** | **67 (116)** |

Python is graded by its own output only, so no language change touches these columns.
- **The leak is larger.** v1 fails 59 Python first attempts; 46 of them are Aether syntax (`fn`, `let x: T`,
  `fx {`, `ret`), against 34 of 41 for run 7. The Python records are the same 220 in both datasets; v1 has 300 more
  Aether records, so Python's share fell from 13% to 11%.
- **After repair the two are level** (116 of 126): the Python error message brings the model back.
- This is the owner's 2026-10-19 decision in force: the release is an Aether specialist, and the card states the
  Python numbers and recommends the base model, or the adapter unloaded, for other languages.

## v1's two main-suite misses

Both are in tasks_hard_nontoon:
- `hard2_heap_sort`: an in-place helper that mutates an array parameter. ARR-001 (value semantics) reports it and
  the repair round fixes it. Run 7 missed the same task the same way.
- `hard2_expr_eval`: a recursive-descent parser that passes its cursor as an `Int` parameter, so each helper
  advances its own copy and the caller never sees it move. It compiled, ran and printed the wrong answer, and the
  repair rounds did not find it.
  - Follow-up (2026-10-21): D77 adds PARAM-001, a warning on a scalar parameter used as a shared cursor (0 false
    positives over 6,393 correct programs; it flags this program), and the 11-pair cursor-sharing pack teaches the
    pattern (`training/seed_instruction_pairs.json`, `cursor_*`). Both are for the next dataset.

## Reading

- **v1 replaces run 7.** On one grader (2026-10-20-12) it gains 9 main-suite first attempts (71 against 62) and ties
  on the held-out suites (37 against 37; run 7's seeds span 35-38).
  - 6 of the 9 are tasks run 7 lost only to `formatfloat` (D72); v1 passes all six, never writes the old name, and
    14 of its main-suite programs use `format_float`.
  - Against run 7 as published, each on its own language (run 7: 68 main, 39 held-out, seeds 68-69 and 39-40), v1
    is +3 on main, just above that spread, and -2 on held-out, just below it. The refresh keeps run 7's quality and
    moves it to the 1.0 language.
- **Held-out misses are mostly the same tasks.** Of v1's 16 held-out first-attempt misses, 11 are tasks run 7 also
  missed, chiefly invented builtin names (`ints_to_text`, `toon_text_array`, `mstream_from_file`, `isDigit`) and
  MStream file calls outside `fx`. The other 5 are two TYPE-001s, a TUP-001 and two silently wrong outputs; all 5
  pass after repair.
  - `isDigit` got no hint (only `isdigit` maps to `is_digit`), and both repairs failed. A general
    camelCase→snake_case SCOPE-001 hint was filed for the compiler.
- **Maps are trained but not yet measured.** 81 training records use `Map[...]`; no main-suite program does,
  because those tasks predate maps and arrays solve them. The benchmark does not yet measure map fluency.
- **Python got worse on the first attempt** (85 → 67 of 126); see above. It fits the catastrophic-forgetting reading
  from run 7 (more Aether data, a smaller Python share) and is accepted for an Aether-only model.

## Files

- `score/`
  - `tuned__*`: main suites, host-c.
  - `m5ollama__*`: held-out suites, scoring Mac.
  - `python__*`: the Python check.
- `replay/`: run 7, its seeds and the 2026-10-18 guided base re-graded on 2026-10-20-12.
- `train/run_metadata.json`: the training run as executed.
