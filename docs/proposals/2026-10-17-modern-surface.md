# Proposal: a modern standard surface, no Pascal-era forms, limited nested functions

Status: **accepted** by the owner, 2026-10-17, with these resolutions: `try_parse_int` /
`try_parse_real`; `min_of` / `max_of`; record sorting by `sorted_by(xs, "field")` /
`sorted_by_desc(xs, "field")` with a literal, statically checked field name; and **no transition
period**: the Pascal-era forms become class 2 (coded diagnostic naming the modern form) in the same
release that ships their replacements ("rip the bandage off": the language has essentially no
outside users yet, and the public push follows the trained model). Decision rows D55-D62 are in
`docs/aether_decisions.md`.

## Why

The thesis (rationale §1) is that a model should write correct Aether from as little in its prompt
as possible, and §4 says how: *say what the model expects, lower to what the VM has*. Two parts of
today's surface work against that.

1. **Pascal-era forms.** The guides still teach the shared engine's Pascal heritage:
   `copy(s, start, count)`, `pos(needle, s)` (Pascal argument order), `stringofchar(ch, n)`,
   output parameters (`val(text, n, code)`, `readln(f, line)`, `gettime`, `getdate`), and the
   `File` + `assign` / `reset` / `rewrite` / `readln` / `eof` / `close` sequence. Few models or
   people reach for these first.
2. **Missing everyday helpers.** There is no `upper`/`lower` (D21 accepted whole-string case
   conversion on 2026-10-06; it never shipped, and `upcase("ab")` still prints `A`, a silent wrong
   result), no `replace`, `contains`, `starts_with`, `join`, `sort`, `sum`, `any`/`all`,
   `repeat`, `index_of`, checked parse with a return value, or one-call file read/write. The full
   guide spends a section ("Writing what the surface does not give you") teaching models to
   hand-roll them.

### Evidence

The 2026-10-17 comparison of the same model (Qwen3.8-27B) writing Python (untrained) and Aether
(fine-tuned, no guide) on 126 tasks: 26 tasks pass in Python on the first attempt and fail in
Aether. Classified by cause:

| cause | tasks | examples (model's first attempt) |
|---|---:|---|
| nested helper function capturing the caller's state | 3 | `cs_nqueens`, `cs_dijkstra`, `algo_union_find_components` (`fn backtrack(...)` inside `main`) |
| MStream verbs that do not exist | 3 | `mstreamlength`, `mstreamappendbytes`, `mstreamloadfile` |
| whole-string Text helpers | 2 | `s.upper()`, `Text * Int` for repetition |
| checked parse as a value | 1 | `let (n, ok) = val(...)` |
| one-call file write | 1 | `create_file(...)` |
| quantifier in a contract | 1 | `@pre all(...)` |
| rounding rule (silent wrong) | 1 | ledger rounding expects half-up; `formatfloat` rounds half-to-even |
| ordinary logic or format slips | ~14 | silent-wrong output, run-time errors |

The first seven rows (12 tasks, 9% of the 126) are surface gaps, not reasoning. Earlier boards
show the same names: the pilot invented `mstream_new`, `yyjson_parse_file`, `text_to_int`; the
algorithm-idiom pack had to teach hand-written sorts, joins and DP-table construction.

## Principles (from the existing decisions)

- **Names models expect, lowered to the engine** (§4). New helpers are surface names; where the
  engine already has the operation (substring, `pos`, `trim`, `IntToStr`), they lower to it in the
  Aether front end (`src/aether/ast_prepasses.c` / `ast_parser.c`); where it does not (sort, file
  slurp, MStream append-text), they are new pscal-core builtins (rationale §8's five sites).
- **Four synonym classes** (D7, `tests/surface/registry.json`). New forms are **class 1, taught**.
  Pascal-era forms move straight to **class 2**: a coded diagnostic naming the modern form, in the
  same release (owner, 2026-10-17: no transition). The corpus, drills and guides are migrated in that
  release so nothing in the repo depends on a removed form. Every spelling gets a registry entry and
  a fixture.
- **Measure before change** (D3). Most additions are zero-GPU to measure: `tools/replay_bench.py`
  re-grades every archived program with the new compiler, so a task that failed only because a
  helper was missing flips to pass with no new generation. Behaviour changes (D21's whole-string
  `upcase`) go through the census/waiver path; the silent-wrong share (KPI S) must not rise.
- **No new value types.** D47 (no map type), D49 (no function values) and D53 (narrow tuples)
  stand. Helpers return existing types; checked parse uses a tuple return, which D53 already allows.
- **Effects stay explicit.** File, stdin and clock helpers are effectful (`fx`, FX-001, rejected in
  `@pure`); Text, array and parse helpers are pure.
- **The medium guide has 15 tokens of headroom** (14,985 of 15,000). The modern surface replaces
  text rather than adding it: the hand-rolled-helper section and the `val` / `File` tables shrink.
  Each guide change is measured against the ceiling (`tools/check_guide_tokens.py`).

## Part A: replace the Pascal-era forms

| today (Pascal heritage) | modern canonical form | type | lowering |
|---|---|---|---|
| `copy(s, start, count)` | `s[a..b]`, `s[a..]`, `s[..b]` (exist) | Text | none needed |
| `pos(needle, s)` | `index_of(s, needle)` (-1 if absent), `contains(s, x)`, `starts_with(s, p)`, `ends_with(s, p)` | Int / Bool | `pos` + comparisons |
| `stringofchar(ch, n)` | `repeat(s, n)` (any Text, not only one character) | Text | new builtin or loop lowering |
| `upcase(s)` / `toupper(s)` (first character only) | `upper(s)`, `lower(s)`, whole string; `upcase` becomes an alias (D21) | Text | new builtin |
| `val(text, n, code)` / `valreal(...)` (output parameters) | `try_parse_int(t) -> (Int, Bool)`, `try_parse_real(t) -> (Real, Bool)` | tuple | wrap `val` |
| `inttostr(n)`, `itoa(n)` | `int_to_text(n)` (exists; stays canonical) | Text | none |
| `File` + `assign` / `reset` / `rewrite` / `append` / `readln(f, l)` / `eof` / `close` / `erase` / `rename` | `read_file(p) -> Text`, `read_lines(p) -> Text[]`, `write_file(p, t)`, `append_file(p, t)`, `delete_file(p) -> Bool`, `rename_file(a, b) -> Bool`, `file_exists(p)` (alias of `fileexists`) | fx | new builtins over the engine's file layer |
| `readln(line)` from stdin | `read_line() -> Text`, `read_all_stdin() -> Text` | fx | wrap `readln` |
| `paramcount()` / `paramstr(i)` | `args() -> Text[]` | fx | loop over `paramstr` |
| `gettime(h, m, s, cs)` / `getdate(...)` | `clock_ms() -> Int`, `now_text() -> Text` | fx | wrap |
| `setlength(xs, n)` (used to pop) | `xs[..n]` (open slices, 2026-10-17-1) | array | none |
| `mstreamcreate`, `mstreamfromstring`, `mstreambuffer`, `mstreamappendbyte`, `mstreamloadfromfile`, `mstreamsavetofile`, `mstreamfree` | `mstream_new()`, `mstream_from_text(t)`, `mstream_text(ms)`, `mstream_append_byte(ms, b)`, `mstream_append_text(ms, t)`, `mstream_length(ms)`, `mstream_load(path) -> MStream` (fx), `mstream_save(ms, path)` (fx), `mstream_free(ms)` | MStream | aliases + 2 new builtins |

The `File` API is withdrawn from the surface with the rest (class 2). If a genuinely streaming
reader is needed later, it gets a modern design (e.g. a `lines(path)` loop collection), not the
Pascal sequence.
HTTP keeps its `MStream` out-buffer, documented with the new names.

## Part B: fill the gaps (the documented modern standard library)

Small, pure unless noted, documented in one compact guide table each.

| area | helpers | notes |
|---|---|---|
| Text | `upper`, `lower`, `replace(s, old, new)`, `contains`, `starts_with`, `ends_with`, `index_of`, `repeat`, `join(xs, sep)`, `split` (exists), `trim` (exists) | `join` on `Text[]` |
| Arrays (`Int[]`, `Real[]`, `Text[]`) | `sum(xs)`, `min_of(xs)`, `max_of(xs)`, `sorted(xs)` (stable, ascending), `sorted_desc(xs)`, `reversed(xs)`, `contains(xs, v)`, `index_of(xs, v)`, `count(xs, v)` | `sorted` returns a new array: arrays are values; no comparator (D49) |
| Arrays of records | `sorted_by(xs, "field")`, `sorted_by_desc(xs, "field")` | stable; the field name must be a string literal naming an `Int`, `Real` or `Text` field of the element type, checked at compile time (FIELD-002 otherwise); no function value exists, so D49 holds (the ideas file's "names, not functions" route) |
| Quantifiers | expression syntax `all x in xs: cond`, `any i in a..b: cond` | bounded, reuses `loop` binding forms; works in `@pre`/`@post` (contracts today cannot say "every element"); lowered to a loop |
| Parsing | `try_parse_int`, `try_parse_real` -> `(value, ok)` | `parse_*` stay total |
| Numbers | `round_to(x, digits) -> Real` (half away from zero, the schoolbook rule specs usually mean), and `formatfloat` documented as half-to-even | fixes the silent-wrong ledger case |
| Files, stdin, args, clock | Part A table | fx |

**Not in scope:** maps/dicts (D47), map/filter with function values (D49), format strings (D48),
methods on builtin types beyond the existing class-1 `.length`/`.len` (W6-05 stands: `s.upper()`
gets a SCOPE-001 hint naming `upper(s)`).

## Part C: limited nested functions (refines D49, does not reverse it)

Models write a local helper that reads the enclosing function's state, as Python does: N-Queens
backtracking over `n` and `cols`, Dijkstra's relax step over `dist` and `adj`, union-find's `find`
over `parent`. Proposed rule:

- A `fn` may be declared inside a function body. It may **read** the enclosing function's
  parameters and locals declared before it, and call itself and other nested functions of the same
  enclosing function.
- It may **not assign** to an enclosing local (new coded diagnostic, e.g. FUNC-002: "a nested
  function cannot change `count` of its enclosing function; return the new value, or keep it in a
  record field"). Record fields are reachable as today (records are pointer-backed), so shared
  mutable state goes through a record, the same rule as `par`.
- It is not a value: no passing, storing or returning it (FUNC-001 as today). No nesting deeper than
  one level.
- **Lowering:** lambda lifting. The nested function becomes a top-level function with one extra
  parameter per enclosing local it reads, passed at every call site. Reads see the value at call
  time, which is exactly the semantics, since nothing can assign them. Arrays read this way are
  passed without a copy (read-only), so recursion over a board array does not pay a copy per call
  (needs a read-only-parameter fast path; measure).
- Methods (`type` bodies) keep their METH-001 rule; this applies to free functions only.

This gives models the helper they write, keeps D49's invariant (no function values, no mutable
capture), and needs no closure runtime.

## Draft decision rows

| ID | Decision | Rule to decide / ship |
|---|---|---|
| D55 | Pascal-era forms become class 2 (coded diagnostic naming the modern form) in the release that ships their replacements; no transition | corpus, drills and guides migrated in the same release; replay pass->fail on archived programs using removed forms is expected and waived as a class |
| D56 | Text helpers (`upper`, `lower`, `replace`, `contains`, `starts_with`, `ends_with`, `index_of`, `repeat`, `join`) are class 1, taught; ships D21 | fixtures; census for `upcase` whole-string (D21 waivers) |
| D57 | Array helpers (`sum`, `min_of`, `max_of`, `sorted`, `sorted_desc`, `reversed`, `contains`, `index_of`, `count`) and record sorting `sorted_by` / `sorted_by_desc` with a literal field name | fixtures for Int/Real/Text and records; stability tests; FIELD-002 for an unknown or non-comparable field |
| D58 | Bounded quantifier expressions `all`/`any` (including in contracts) | parser fixtures; contract fixtures; no change to existing programs |
| D59 | `try_parse_int` / `try_parse_real` return `(value, ok)`; `val` / `valreal` class 2 | fixtures including `""`, `"12x"`, `"-3"` on Linux and macOS |
| D60 | One-call file, stdin, args and clock helpers (fx); the `File` API is class 2 | fixtures under `--deny`; FX-001 and ANN-001 coverage |
| D61 | MStream snake_case family plus `mstream_append_text`, `mstream_length`, `mstream_load` | fixtures; HTTP example rewritten |
| D62 | Nested functions with read-only capture, lambda-lifted (refines D49) | FUNC-002 fixtures; recursion fixtures; replay of nqueens/dijkstra/union-find first attempts |

## Rollout

1. **Release A (no new syntax), one cut-over:** D55, D56, D57, D59, D60, D61 and `round_to`. New
   helpers ship as class 1; every Pascal-era form becomes class 2 in the same release; SCOPE-001 hints
   for the method spellings (`s.upper()` names `upper(s)`). Registry entries and fixtures. In the same
   release: a codemod migrates the corpus, seed pairs, drills, distilled pairs, examples and tests to
   the modern forms (each program re-verified for identical stdout); repair drills whose lesson
   inverts are retired or repointed (the drill-polarity gate finds them, e.g.
   `repair_mstream_new_name`, `repair_text_to_int_name`); all guides and the card switch; replay
   census with the pass->fail of archived programs that used removed forms waived as one class.
2. **Release B (syntax):** D58 quantifiers and D62 nested functions.
3. **Measurement:** replay on every release (zero GPU); one guided board (card and medium) after
   Release B; the next fine-tune trains on the modern surface.

## Resolved questions (owner, 2026-10-17)

- `try_parse_int` / `try_parse_real`; `min_of` / `max_of`.
- Record sorting: `sorted_by(xs, "field")` / `sorted_by_desc(xs, "field")`, literal field names
  checked at compile time (chosen as the form most likely to raise first-attempt scores: models write
  key-based sorts, the hand-written loop is long and error-prone, and no function value is involved).
- Transition: none; the Pascal-era forms are removed in the release that adds their replacements.
