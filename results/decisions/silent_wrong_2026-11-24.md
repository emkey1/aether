# Seven silently wrong behaviours (D90 to D96), 2026-11-24

**Question.** The review of 2026-11-24 found seven places where a program
that compiles under `1.0` gives a wrong answer with no error. D75 lets such a
program become a coded error, with a decision row, paired-replay and corpus
evidence, and a hint that names the fix. Does any archived or tested program
depend on the old behaviour?

**Arms.** Base: aether `830a6d8` (language `2026-11-24-1`, pscal-core
`d51e663`). New: the same with D90 to D96 and the diagnostics changes of
language `2026-11-24-2` (pscal-core `8b1e351`). Both built on the build
machine, macOS arm64, Release.

## 1. Census of the forms

```bash
python3 results/decisions/silent_wrong_census.py .
```

Over 6,716 distinct programs (every attempt's `source_code` in the 333 reports
under `bench/results`) and the 218 programs of `tests/replay`:

| form | decision | programs |
|---|---|---:|
| a TOON or stream binding in a `let` that does not start its line | D90 | 0 |
| `\xNN` at or above 0x80 | D92 | 0 |
| a negative literal Text slice bound | D95 | 0 |
| `round(x, n)` | D96 | 0 |
| an `Int` binding from a Real literal of 1e19 or more | D94 | 0 |

For scale, 1,002 of the 6,716 programs bind a TOON or stream handle with a
`let`, every one at the start of its line. D91 (a name an imported module
exports) and D93 (a JSON integer above 2^63) have no source pattern to count;
the replay below covers them.

## 2. Paired replay of every archived attempt

```bash
python3 tools/replay_bench.py --aether-bin <base>/build/aether --report-json base.json \
    'bench/results/**/*.json.gz' 'bench/results/**/*.json'
python3 tools/replay_bench.py --aether-bin <new>/build/aether --report-json new.json \
    'bench/results/**/*.json.gz' 'bench/results/**/*.json'
```

328 reports, 9,955 attempts, 6,085 distinct programs on each binary.

| | base | new |
|---|---:|---:|
| first attempts that pass (of 6,001) | 3,379 | 3,379 |
| repair attempts that pass (of 3,954) | 683 | 683 |
| pass → fail flips against the stored result | 398 | 398, the same attempts |
| failing attempts whose first error is coded | | 11 more |

No attempt passes on one binary and fails on the other. The 398 flips are the
language changes made before `1.0` (the reports are older than the snake_case
builtins and the rest), identical on both. The only difference is 11 failing
attempts that stopped with the uncoded `VM Error: Array element index out of
bounds.` and now stop with `[ARR-003] array index is out of bounds: the array
(or the row) is empty there.` (tasks `cs_sieve_primes`, `cs_edit_distance`,
`cs_nqueens`, `trap_grid_clone`, `trap_toplevel_init`).

The gated replay CTest runs (`tools/replay_bench.py --aether-bin build/aether`,
the plain reports git tracks) is unchanged: 44 waived, 0 unwaived.

## 3. Every program in the tree against the `1.0` release binary

Each `.aether` under `tests/` and `examples/` (1,165 programs, with its
`.flags`, `.in`, `.env` and `.files`), run on the `1.0` release binary and on
the new build; stdout, exit status and the first stderr line compared.

24 differ. 21 are the fixtures this change adds (`tests/fx/...`, listed in the
decision rows), which is what they are for. The other 3 differ by `par` output
order or `randomize()` (`tests/fx/par/par_error_after_inner_par_fail`,
`tests/par_stdout_lines_pass`, `tests/random_seeded_pass`), as they did before
the change.

## 4. The corpus

`tools/pin_gate.sh` runs `tools/corpus_ab.py` over
`training/corpus_candidates`, base against new: 745 programs, 734 compared, 734 the same, 0 unexplained differences (11 nondeterministic programs left out). The same gate passed conformance, `tests/run.sh` cold and warm, rea's tests, the umbrella's other front ends and an AddressSanitizer run.

## Reading

Nothing archived, replayed, tested or in the corpus uses a form these
decisions change, so no stored result moves. The cost is carried by programs
nobody has kept: each decision row states which.
