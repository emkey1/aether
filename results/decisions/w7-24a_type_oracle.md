# W7-24a: the type oracle core, gate evidence

Built on aether 7f58a33 (language 2026-10-07-1). The oracle is
`src/aether/types.c` (`aetherTypeOf`, the scope environment, the use-classifying
walker `aetherTypedWalk`) and the pass `aetherTypedPass`, called at the end of
`aetherPerformSemanticAnalysis` after NARROW-001. With `AETHER_DUMP_TYPES`
unset the pass returns at once; set, it prints one row per sink and reports
nothing to the diagnostic stream. No rule is switched on.

## Program sets

| Set | Programs | Where |
|---|---|---|
| tests | 245 (149 `*_pass`) | `tests/*.aether` |
| examples | 70 | every program file under `examples/` |
| corpus | 748 | umbrella `Tests/aether_specialization/corpus_candidates_manifest.json` items, run with the corpus fixtures in the working directory |
| archive | 413 | the review's archived model programs (the 596-program set, as since extended) united with every distinct `source_code` in the umbrella's tracked `Tests/aether_doc_bench/results/**/*.json`: 629 files. 190 of them still carried the harness's `__AETHER_BENCH_START__`/`__AETHER_BENCH_END__` sentinel lines from the raw generation. Those lines are stripped and the result deduplicated by content, leaving 413 distinct programs |

18 programs print different output on two runs of one binary (random seeds,
the clock, the environment, par timing). Their AST and diagnostics are still
compared; their run stdout is not.

(The first runs of this lane used the 629 raw files with the sentinels still in
place, and so measured 189 sentinel copies as SCOPE-001 failures. Every number
below comes from the re-run on the 413 cleaned programs at the lane's final
commit.)

## Gate 1: 0 added diagnostics, flags-off identity

Three arms over all 1,476 programs: the 7f58a33 binary, the lane's final
binary, and the lane's final binary with `AETHER_DUMP_TYPES=/dev/null` (the
pass runs over every program, then compilation continues). The same check on
this commit's own binary, over the raw 629-file archive, also gave 0 diffs.

| Arm | `--dump-ast-json` (stdout+stderr) differs | run rc/stdout differs | run stderr differs | corpus golden pass |
|---|---|---|---|---|
| base 7f58a33 | - | - | - | 717 / 734 |
| final binary | 0 | 0 | 0 | 717 / 734 |
| final binary, pass on | 0 | 0 | 0 | 717 / 734 |

```sh
tools/aether_experiment_census.py --identity \
    --arm base=<7f58a33 build>/aether: --arm head=build/aether: \
    --arm head_dump='build/aether:;AETHER_DUMP_TYPES=/dev/null' \
    --tests tests --examples examples \
    --corpus <umbrella>/Tests/aether_specialization/corpus_candidates_manifest.json \
    --corpus-root <umbrella> --fixtures <umbrella>/Tests/aether_specialization/fixtures \
    --dir <cleaned archive dir> --nondeterministic <the 18 keys> --timeout 60
```

## Gate 2: the dump agrees with the declared types

`tools/check_type_oracle.py` (CTest `aether_type_oracle`) over the 149
`tests/*_pass.aether`, 1,462 sinks:

| expression | exact | widen | narrow | compat | unknown | MISMATCH |
|---|---|---|---|---|---|---|
| ARRAY_ACCESS | 41 | 0 | 0 | 3 | 0 | 0 |
| ARRAY_LITERAL | 60 | 0 | 0 | 21 | 0 | 0 |
| BINARY_OP | 273 | 0 | 6 | 0 | 0 | 0 |
| BOOLEAN | 21 | 0 | 0 | 0 | 0 | 0 |
| FIELD_ACCESS | 10 | 0 | 0 | 0 | 0 | 0 |
| NEW | 36 | 0 | 0 | 0 | 0 | 0 |
| NUMBER | 407 | 0 | 1 | 0 | 0 | 0 |
| PROCEDURE_CALL | 185 | 0 | 5 | 37 | 2 | 0 |
| STRING | 104 | 0 | 0 | 0 | 0 | 0 |
| TERNARY | 10 | 0 | 0 | 0 | 0 | 0 |
| UNARY_OP | 6 | 0 | 0 | 0 | 0 | 0 |
| VARIABLE | 231 | 3 | 0 | 0 | 0 | 0 |
| **all** | 1,384 | 3 | 12 | 61 | 2 | **0** |

- *widen* is an Int value into a Real sink; *narrow* a Real value into an Int
  sink, which today's D1 sink coercion accepts (every row is in
  `narrowing_*_pass`, `ret_int_division_pass` or an Int/Int `/`).
- *compat* is Char into Text (Text indexing), `[]` into `T[]`, and TOON or
  MStream handles, which lower to Int slots.
- The 2 *unknown* rows are `task_result` / `threadgetresult` bound to Text in
  the two deny fixtures; the table leaves that builtin out (see below).

The same check over the other sets (no gate; a MISMATCH there is either an
oracle bug or a program the compiler should not accept):

| Set | sinks | MISMATCH | unknown |
|---|---|---|---|
| corpus | 21,791 | 0 | 10 |
| examples | 1,023 | 0 | 2 |
| archive | 6,349 | 28 | 28 |

All 28 archive MISMATCH rows come from two variants of one program,
`let adj: Int[][] = []; adj = adj + [1, 4];`. The append lowering
(`setlength` plus one indexed store per element) flattens the row into
separate Int elements of a 2-D array, which then fails at run time with the
uncoded `Length expects a string or array argument.` The oracle reports
`declared Int[], oracle Int` at each store. This is a real defect, and a
candidate first rule for the W7-24 pass: an array `+` whose right side has the
left side's rank.

`check_type_oracle.py --builtins` (CTest `aether_type_oracle_builtins`)
checks the 49 rows of the interim builtin return table that `builtins_json`
also types: 0 disagreements. One registry conflict was kept out of the table:
`builtins_json` types `task_result` / `thread_get_result` as Int, while
`tests/task_deny_pass` and `thread_pool_deny_pass` bind it to Text (the
`dnslookup` payload). W7-07 or W7-16 should settle it.

## Deviations

- `--dump-types` is spelled `AETHER_DUMP_TYPES=1` (stdout, then exit) or
  `AETHER_DUMP_TYPES=<path>` (write the file, keep compiling). Every CLI option
  is parsed by the shared engine's `main.c` (rea), which rejects unknown ones
  before any front-end hook runs. The CLI spelling needs that rea option-table
  entry and a pin bump, so it was left for the next R- release.
- Builtin return types come from an interim table in `types.c` (W7-07's role
  until W7-16's manifest), checked against `builtins_json` by the CTest above.
  Module exports are read from the loaded module ASTs, not from W7-18's export
  table.
- The pass walks the main program and then every loaded module, using its own
  loop rather than W7-06's.
