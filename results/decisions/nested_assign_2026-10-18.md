# Nested functions that assign enclosing locals: archived attempts (D64, D65 evidence)

Evidence for [`docs/proposals/2026-10-18-nested-fn-assignment.md`](../../docs/proposals/2026-10-18-nested-fn-assignment.md).
Binary: aether 143eee7, language 2026-10-17-5 (D62 shipped: nested fns read
their enclosing locals; assigning one is FUNC-002).

## Method

`nested_assign_census.py` reads every archived report (`bench/results/**/*.json`
and `*.json.gz`, and `training/training_runs/*/score/*.json.gz`). It keeps each
attempt whose source declares a `fn` inside another fn's body (a brace scanner:
a method inside `type` is not nested), and grades it on the binary against its
own task definition, through `tools/replay_bench.py`'s task resolver,
`compile_and_run` and failure classes.

To grade what an attempt would do if its assignments were legal,
`nested_assign_lower.py` rewrites it into an equivalent program the compiler
accepts. Each local that a nested fn assigns becomes a top-level `let` (a
global, which any function may assign), and its declaration in the host
becomes an assignment. The compiler names the locals: lower, compile, read the
FUNC-002 name, repeat. That is the same program whenever the host is neither
recursive nor re-entered while a nested fn runs. Every host here is `main` or
a function `main` calls once. One hand edit: in `9736b41d2fa7` the hoisted
`board` collided with another fn's parameter of that name (NAME-001), so the
global is `qboard` inside the host. `nested_annot_stripped/` holds the
attempts that put `@pure` above a nested fn inside the body, with those
annotation lines removed (regex `^[ \t]+@(pure|pre|post|cost)\b.*\n`).

```sh
python3 results/decisions/nested_assign_census.py --aether-bin build/aether \
    --lowered-dir results/decisions/nested_assign_lowered \
    --json results/decisions/nested_assign_census.json
python3 results/decisions/nested_assign_census.py --aether-bin build/aether \
    --lowered-dir results/decisions/nested_annot_stripped
```

## Census

55 attempts declare a nested fn: 37 distinct sources, 5 models
(devstral-small-2, prism-coder-7b, qwen3.5-9b, and the qwen3.8-27b base and
algo-tuned runs), with the medium guide, the card, and no guide. They come from
the B0/B1 boards of 2026-10-07, 2026-10-10 and 2026-10-11 and from two
fine-tune scoring runs. None of these attempts is in the instrument check's
tracked replay, which reads only plain `bench/results/**/*.json`.

| outcome on 2026-10-17-5 | first attempts | repair attempts | distinct sources |
|---|---|---|---|
| FUNC-002 (a nested fn assigns an enclosing local) | 9 | 12 | 15 |
| pass (a read-only nested helper) | 0 | 1 | 1 |
| SYN-001 at a `@pure` written above a nested fn inside the body | 7 | 12 | 7 |
| another error, unrelated to the nested fn | 3 | 11 | 14 |
| **total** | **19** | **36** | **37** |

The models never saw FUNC-001 or FUNC-002. All 21 FUNC-002
attempts were archived on aether 2026-10-06-1 (16) or 2026-10-16-3 (5), where
a nested `fn` failed with `[SYN-001] expected ')' to close argument list`. The
archive therefore says nothing about how models repair FUNC-002.

## The FUNC-002 attempts, made legal

| model | task | attempts (first + repair) | locals assigned | if legal: first | if legal: all |
|---|---|---|---|---|---|
| qwen3.8-27b algo-tuned (no guide) | cs_nqueens | 1 + 2 (same source 3x) | `count`, `cols`, `diag1`, `diag2` (append, slice) | pass | 3 pass |
| qwen3.8-27b algo-tuned (no guide) | cs_dijkstra | 1 + 0 | `adj` (element append) | pass | 1 pass |
| qwen3.8-27b algo-tuned (no guide) | algo_union_find_components | 1 + 0 | `parent` (element write) | pass | 1 pass |
| devstral-small-2 (medium) | hard2_heap_sort, 3 repeats | 3 + 6 | `heap` (append, element write), `size` | 3 pass | 9 pass |
| prism-coder-7b (medium) | cs_nqueens | 1 + 0 | `count`, `board` (element write) | pass | 1 pass |
| devstral-small-2 (medium) | cs_nqueens, 2 repeats | 2 + 4 | `count`, `board` (element write) | 2 silent_wrong | 6 silent_wrong |
| **total** | | **9 + 12** | | **7 pass, 2 silent_wrong** | **15 pass, 6 silent_wrong** |

- Every one of the 15 distinct sources assigns an enclosing **array** (element
  write, append or slice). Seven also bump a scalar counter. A scalars-only
  relaxation would make none of them legal.
- The six silent wrong answers are devstral's own bug: its diagonal test
  `(i - r) == (c - c)` compares against zero, so it counts 720 placements of
  six queens. Under FUNC-002 they fail with a code, but the fix the hint asks
  for leaves the bug, so they would not pass either way.
- Repair behaviour under the old message: devstral kept the nested helper in
  all three attempts of all five cases (it resubmitted identical source twice).
  The tuned model resubmitted the same N-Queens source three times. On Dijkstra
  and union-find it dropped the helper on attempt 2 and passed.
- No attempt writes Python's `nonlocal`.

## Annotated nested functions

19 attempts (7 distinct sources, devstral-small-2 and prism-coder-7b) write
`@pure` directly above a nested fn inside the body. Today that is the generic
`[SYN-001] unexpected token in block; expected a statement`, before D62 sees
the fn. With those lines removed, 3 attempts pass (`hard2_grade_bands`
repeat 2, all three attempts). The other 16 fail later on unrelated errors:
`toon_parse_file` outside `fx` (FX-001, 13) and a tuple type in an array
(TUP-001, 3).

## Files

- `nested_assign_census.py`, `nested_assign_lower.py`: the scripts above.
- `nested_assign_census.json`: one row per attempt (report, model, guide,
  task, attempt, source sha256 prefix, stored and current class, first error,
  the class of its lowered version).
- `nested_assign_lowered/<sha12>.aether`: the 15 rewritten sources.
- `nested_annot_stripped/<sha12>.aether`: the 7 sources without their
  in-body annotations.

## Re-run on the shipped release (2026-10-18-1)

The same census, with the original archived sources (not the lowered or
stripped copies), on the binary that ships D64 and D65:

```sh
python3 results/decisions/nested_assign_census.py --aether-bin build/aether \
    --lowered-dir results/decisions/nested_assign_lowered
```

| outcome on 2026-10-18-1 | first attempts | all attempts |
|---|---|---|
| pass | 8 | 19 |
| silent wrong | 2 | 6 |
| coded error, unrelated to the nested fn | 9 | 30 |
| **total** | **19** | **55** |

- The 21 FUNC-002 attempts run, and each matches its lowered version's class
  from the table above: 15 pass and 6 silent wrong (devstral's N-Queens),
  first attempts 7 pass and 2 silent wrong. No attempt differs from the
  measurement.
- The 3 `hard2_grade_bands` attempts (`07a540678b01`) with `@pure` above a
  nested fn pass as written. The other 16 annotated attempts now fail on the
  unrelated FX-001 and TUP-001 errors the stripped copies showed.
- The read-only helper that passed before still passes. Every remaining coded
  error is unrelated (foreign syntax such as `[Int; 5]`, a missing return
  type, FX-001, TUP-001).

