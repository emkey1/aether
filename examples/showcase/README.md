# Aether showcase

This directory contains more substantial Aether examples than the small
one-file samples in `examples/base`.

Run them from the repository root with:

```sh
./build/bin/aether examples/showcase/agent_report
./build/bin/aether examples/showcase/gradebook
./build/bin/aether examples/showcase/release_board
./build/bin/aether examples/showcase/grade_report
```

`agent_report` exercises the current Aether surface in combination:

- relative `use` imports across sibling Aether modules
- imported `const` values used through inferred `let` bindings
- exported helper functions from another Aether module
- `@pure`, `@pre`, and `@post`
- `type` blocks with methods and `self`
- inferred local bindings
- compact `loop` control flow
- `ToonDoc` / `ToonNode` parsing from a file payload
- typed TOON traversal and defaulted reads
- explicit `fx` output boundaries

If yyjson/TOON support is unavailable in the current build, the program exits
cleanly after reporting that capability gap.

`gradebook` is a second, self-contained showcase (no TOON or imports, so it runs
anywhere): a `type` with methods (`average`, `passed`), a top-level helper that
takes the record, parallel `Text[]` / `Int[]` data, accumulation, a real class
average, and formatted output.

`release_board` parses an inline TOON payload of two release trains, analyzes
both concurrently, and prints a per-release, per-train, and whole-board rollup:

- nested `type` blocks (`Release` owns a `Severity`) with field defaults
- methods that mutate `self` as accumulators, plus `@pure` / `@post` helpers
- a `par` block where each branch writes only the record it was handed, with
  the read-only `ToonNode` handles shared across both
- defaulted TOON reads (`toon_get_int_or`, `toon_get_bool_or`) so a malformed
  row degrades instead of aborting the report
- `clamp` / `max` instead of hand-rolled if-chains, and free `Int`/`Real` mixing

It exists because this is the program shape LLMs reach for unprompted when
asked to show off the language, and it pins down the details they get wrong:
`toon_parse` (not `toon_parse_string`), two-argument `formatfloat`, and that
`clamp`/`min`/`max` already exist. It guards on `has_toon()` and exits cleanly
if the build lacks yyjson/TOON.

`grade_report` parses a TOON dataset of exam scores and prints a per-student
table plus a summary rollup, then runs the whole thing again over an **empty**
dataset:

- the one pattern no other example shows: TOON rows **retained** into a
  `Student[]` via `result = result + [s]`, then walked twice — once to
  accumulate, once to print. The other TOON showcases fold each row into an
  accumulator and never keep it.
- `@pure` on methods, and the fact that purity is transitive: a `@pure`
  top-level function calling an unmarked method is an `ANN-001`
- every derived statistic guarded against a zero divisor, and sentinel-seeded
  `min`/`max` suppressed rather than printed when there is no data. The original
  draft guarded two of three division sites and crashed on an empty list — that
  is the failure this example is built around.
- sentinels set at construction (`new Stats { min: MAX_SCORE + 1, max: -1 }`)
  because a field default must be a literal — a named `const` is rejected too
- grade bands as explicit constants rather than `PASS_MARK + 10` / `+ 5`
  arithmetic, which reads as principled but yields a 20-point B against a
  5-point C
