# `return` as an alias of `ret` (D7), 2026-10-08

**Question.** Should `return v;` / `return;` be accepted as `ret` (D7 class 4,
tolerated and never taught) instead of SYN-001 naming `ret` (class 2, today)?

**Arm.** `AETHER_EXPERIMENT=return=alias` parses `return` exactly as `ret`
(fixture `tests/experiments/return_alias.aether`). Off by default; with it unset
the binary is unchanged.

**Method.** Recompile stored first attempts with the same compiler (aether
510b048 plus this arm), alias off and on, and grade against each task's expected
stdout:

```bash
python3 tools/aether_first_attempt_histogram.py --bench-root <umbrella> --recompile --aether-bin build/aether [--experiment return=alias] <reports>
```

| first attempts | pass | silent-wrong | coded | uncoded |
|---|---|---|---|---|
| B0 panel so far, 249 (qwen36-35b-a3b, qwen36-27b, devstral-small-2, bonsai2-27b), off | 214 | 13 | 21 | 1 |
| same, alias | 214 | 13 | 21 | 1 |
| AFM 3 Core card board, 291 (umbrella `results/afm3_20261008/board_card`), off | 10 | 30 | 244 | 7 |
| same, alias | 12 | 39 | 232 | 8 |

**Reading.** No panel model writes `return`, so the alias changes nothing there.
The one model that does gains 2 passes and 9 silent wrong answers: past the
keyword its programs still carry semantic bugs, which the coded error stopped
before they printed anything plausible. A guide table naming `ret` does not cure
the habit either (26 `return` first errors under card-v1 and under the table
variant on the held-out set), so the alias would not be redundant, only harmful.

**Decision.** D7 keeps `return` in class 2. Reopen if a panel model's first
attempts show `return` as a top-5 first error, measured with this arm.
