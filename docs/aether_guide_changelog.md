# Aether guide changelog

Version history for the three LLM-facing guides. Guide versions use the same
`YYYY-MM-DD-N` date-stamp scheme as the language version, but they are **not the
same thing** and they drift apart on purpose:

- [`CHANGELOG.md`](../CHANGELOG.md) tracks the **language** version in
  [`VERSION`](../VERSION) — bumped only when a change alters what programs
  compile or how they run.
- This file tracks each **guide document's** own front-matter stamp — bumped
  whenever the guide text changes, and also on a harness change that could
  plausibly move a score (see `tools/bump_guide_version.py`).

`docs/aether_guided_benchmark.md` stamps every row with the guide version it was
scored against, so this file is what turns that stamp back into "and what was
different about it."

Entries are newest first, one per version stamp, reconstructed from git history
(the commit named is the one that carried the stamp change). Where a commit
touched several guides at once, the same entry appears under each.

## Full guide — `aether_for_llms_and_others.md`

| version | date | commit | change |
|---|---|---|---|
| `2026-08-11-1` | 2026-08-11 | `901ec51` | docs: add the **unknown type name in an annotation** case to the SCOPE-001 catch-all list, now that it is a compile error in every position and at any array depth (language `2026-08-11-2`). Previously only `let v: Bogus = 1` was rejected; `let v: Bogus[]`, `fn f(p: Bogus)`, a `Bogus` return type and a `Bogus` record field all compiled clean, so a model that invented a type name got no feedback to repair from. Notes that declaration order does not matter, so the fix is spelling or a missing `type`, not moving code |
| `2026-08-10-1` | 2026-08-10 | `f899d95` | docs: **NEST-001 now leads with `toon_key_or(node, key, toon_null())`**, the total-path idiom, instead of only the `if toon_has_key` guard stack. A missing key yields a null node that absorbs the rest of the walk (`toon_len` 0, every `_or` getter falls back), so a path of any depth is safe with no nesting. `toon_key_or` and `toon_null` had existed since the TOON surface landed but appeared **only as generated appendix rows** — across 8 models and 3 vendors benchmarked on a nested-path task, not one used them. Records the `toon_has_at` exception (it raises on a null node rather than returning false; test `i < toon_len(n)`) and when to still prefer `toon_has_key` (distinguishing absent from falsy) |
| `2026-08-08-1` | 2026-08-08 | `fcae527` | docs: document **[ARR-003]**, the runtime out-of-bounds error, now that it carries a code and a source line (pscal-core `ab86a7d`). Adds the build-a-table-row-by-row worked case — `table[i]` does not exist until after `table = table + [row];`, so the row being built is the local `row` — plus the half-open reminder that `loop i in 0..n` ends at `n - 1` |
| `2026-08-06-2` | 2026-08-06 | `07daaf4` | docs: document `val`/`valreal` as the checked parse — the previous revision's "there is no `try_parse`" read as "failure cannot be detected"; adds a **Checked parsing** section with the `code` table, canonical-vs-accepted rows for untrusted `Text` → `Int`/`Real`, and a prefer-`val` note on the contract recipe. Appendix regenerated: `val`/`valreal` move to the documented tier, and ~70 SDL/OpenGL/sound names leave Core with `graphics` added to `SKIP_CATEGORIES` |
| `2026-08-06-1` | 2026-08-06 | `e2e5582` | docs: add **Writing what the surface does not give you** (7 recipes, ported from the medium guide, where it had been the only copy); document that `parse_*` cannot report failure; state that there is no map/dict type |
| `2026-07-31-1` | 2026-07-31 | `e47d4d6` | fix: appendix no longer contradicts the prose for 68 documented builtins |
| `2026-07-26-5` | 2026-07-26 | `36308d4` | docs: complete builtin inventory appendix, and gate all three guides |
| `2026-07-26-4` | 2026-07-26 | `58e5d2e` | feat: PREC-001 for bitwise-vs-comparison, has_builtin sees core builtins |
| `2026-07-26-3` | 2026-07-26 | `f2fa697` | corpus: text/tuple/bitwise examples, FIELD-003 wording, clock docs |
| `2026-07-26-2` | 2026-07-26 | `d81f4af` | feat: ARR-002 for a 1-D array indexed two-dimensionally |
| `2026-07-26-1` | 2026-07-26 | `c6b5265` | feat: independent random streams per thread, and NARROW-001 |
| `2026-07-15-2` | 2026-07-15 | `eb47d47` | fix: `ret expr` now coerces to the declared return type (2026-07-15-2) |
| `2026-07-09-1` | 2026-07-09 | `fd900a4` | feat: MStream as a first-class opaque type; surface HTTP (MS-001) -- 2026-07-09-1 |
| `2026-07-04-2` | 2026-07-04 | `86a5e9e` | fix(tuple): reentrant record-by-value lowering instead of shared globals (2026-07-04-2) |
| `2026-07-04-1` | 2026-07-04 | `a429ffb` | fix(tuple): generalize TUP-001 to call-cycle detection, add PAR-003 (2026-07-04-1) |
| `2026-07-01-8` | 2026-07-01 | `dc80601` | feat(parser): reject malformed input loudly — closers, Int[N], format specs, tuple recursion (2026-07-01-7) |
| `2026-07-01-7` | 2026-07-01 | `6ca5d7b` | feat(frontend): retire the text rewriter; AST-based fx/purity checks; alias string-literal fix (2026-07-01-5) |
| `2026-07-01-6` | 2026-07-01 | `14a9c4c` | feat(lang): constant record field defaults (`field: Type = <const>`) + FIELD-003 |
| `2026-07-01-5` | 2026-07-01 | `d21e6f7` | docs(guides): record methods take an implicit self (no explicit self param) |
| `2026-07-01-4` | 2026-07-01 | `2ea1e6c` | feat(diagnostics): code the last uncoded errors [2026-07-01-3] |
| `2026-07-01-3` | 2026-07-01 | `f9b6b24` | fix(diagnostics): real TUP-001 code + hint for tuple destructuring |
| `2026-07-01-2` | 2026-07-01 | `63ca546` | fix(diagnostics): coded errors for three silent AST-path failures (SYN-001 backstop, SCOPE-001 undefined method, PAR-001 shared record) |
| `2026-07-01-1` | 2026-07-01 | `1ec0e66` | feat(contracts): reject array-vs-scalar @pre/@post predicates at compile time (ANN-001) |
| `2026-06-30-2` | 2026-06-30 | `c80b831` | docs(guide): clarify record construction + typed bindings (new T(), no fn new()) |
| `2026-06-30-1` | 2026-06-30 | `7868bef` | fix(parser): name reserved-word/member-name collisions in diagnostics (SYN-001) |
| `2026-06-28-3` | 2026-06-28 | `8868305` | docs+examples: align guides and effects_contracts with the FX-001 effect model |
| `2026-06-28-1` | 2026-06-28 | `e8398ba` | effects: gate host interaction (FX-001/@pure), discovery-first docs, single-source effectfulness |
| `2026-06-27-5` | 2026-06-27 | `8643868` | aether: `_` digit separators in numeric literals (lang 2026-06-27-1) |
| `2026-06-27-3` | 2026-06-27 | `be03f58` | docs: document FUNC-001 (no function values) + the `par` concurrency pattern |
| `2026-06-21-1` | 2026-06-21 | `404a7b7` | docs(guides): version both guides (YYYY-MM-DD-N) + bump helper |

## Medium-context guide — `aether_for_llms_medium_contexts.md`

| version | date | commit | change |
|---|---|---|---|
| `2026-08-10-1` | 2026-08-10 | `f899d95` | docs: **NEST-001 rewritten around `toon_key_or(node, key, toon_null())`** — the total-path idiom replaces the guard-stack example as the primary shape, with the `toon_has_at` exception noted. `toon_key_or`/`toon_null` were absent from this tier entirely (not even an appendix row), which is the tier every benchmark run uses. Net +182 tokens; measured 14,833 against the 15,000 ceiling, so **headroom is down to 167** and this guide needs a trim pass before the next addition |
| `2026-08-08-1` | 2026-08-08 | `fcae527` | docs: add **[ARR-003]** to the repair rules — index, dimension and valid range in the message, source line in `[Error Location]`, and the row-not-appended-yet reading of `valid indices are 0..0` |
| `2026-08-06-2` | 2026-08-06 | `07daaf4` | docs: `val` is the checked parse — a paragraph and one gated snippet replacing the bare "there is no `try_parse`", covering the `code` convention and that the destination is left unmodified on failure |
| `2026-08-06-1` | 2026-08-06 | `e2e5582` | docs: document that `parse_*` cannot report failure, and add a validate-before-parse recipe |
| `2026-07-30-1` | 2026-07-30 | `b1748f1` | docs: medium guide documents mstreamfromstring (MS-001) |
| `2026-07-26-6` | 2026-07-26 | `58e5d2e` | feat: PREC-001 for bitwise-vs-comparison, has_builtin sees core builtins |
| `2026-07-26-5` | 2026-07-26 | `f2fa697` | corpus: text/tuple/bitwise examples, FIELD-003 wording, clock docs |
| `2026-07-26-4` | 2026-07-26 | `d81f4af` | feat: ARR-002 for a 1-D array indexed two-dimensionally |
| `2026-07-26-3` | 2026-07-26 | `c6b5265` | feat: independent random streams per thread, and NARROW-001 |
| `2026-07-26-2` | 2026-07-26 | `091e4f0` | docs: medium-context guide, sized for 32K models |

## Small-context guide — `aether_for_llms_with_small_contexts.md`

| version | date | commit | change |
|---|---|---|---|
| `2026-08-10-1` | 2026-08-10 | `f899d95` | docs: NEST-001 leads with the `toon_key_or(node, key, toon_null())` total-path idiom; the `if toon_has_key` guard stays as the one-line alternative, plus the `toon_has_at` null-node exception |
| `2026-08-06-2` | 2026-08-06 | `07daaf4` | docs: point the parse-validity sentence at `val`/`valreal` instead of the non-actionable "validate the text before parsing it" |
| `2026-08-06-1` | 2026-08-06 | `e2e5582` | docs: document that `parse_*` cannot report failure, and state that there is no map/dict type |
| `2026-07-26-4` | 2026-07-26 | `58e5d2e` | feat: PREC-001 for bitwise-vs-comparison, has_builtin sees core builtins |
| `2026-07-26-3` | 2026-07-26 | `f2fa697` | corpus: text/tuple/bitwise examples, FIELD-003 wording, clock docs |
| `2026-07-26-2` | 2026-07-26 | `d81f4af` | feat: ARR-002 for a 1-D array indexed two-dimensionally |
| `2026-07-26-1` | 2026-07-26 | `c6b5265` | feat: independent random streams per thread, and NARROW-001 |
| `2026-07-15-2` | 2026-07-15 | `eb47d47` | fix: `ret expr` now coerces to the declared return type (2026-07-15-2) |
| `2026-07-09-1` | 2026-07-09 | `fd900a4` | feat: MStream as a first-class opaque type; surface HTTP (MS-001) -- 2026-07-09-1 |
| `2026-07-04-2` | 2026-07-04 | `86a5e9e` | fix(tuple): reentrant record-by-value lowering instead of shared globals (2026-07-04-2) |
| `2026-07-04-1` | 2026-07-04 | `a429ffb` | fix(tuple): generalize TUP-001 to call-cycle detection, add PAR-003 (2026-07-04-1) |
| `2026-07-01-8` | 2026-07-01 | `dc80601` | feat(parser): reject malformed input loudly — closers, Int[N], format specs, tuple recursion (2026-07-01-7) |
| `2026-07-01-7` | 2026-07-01 | `6ca5d7b` | feat(frontend): retire the text rewriter; AST-based fx/purity checks; alias string-literal fix (2026-07-01-5) |
| `2026-07-01-6` | 2026-07-01 | `14a9c4c` | feat(lang): constant record field defaults (`field: Type = <const>`) + FIELD-003 |
| `2026-07-01-5` | 2026-07-01 | `d21e6f7` | docs(guides): record methods take an implicit self (no explicit self param) |
| `2026-07-01-4` | 2026-07-01 | `2ea1e6c` | feat(diagnostics): code the last uncoded errors [2026-07-01-3] |
| `2026-07-01-3` | 2026-07-01 | `f9b6b24` | fix(diagnostics): real TUP-001 code + hint for tuple destructuring |
| `2026-07-01-2` | 2026-07-01 | `63ca546` | fix(diagnostics): coded errors for three silent AST-path failures (SYN-001 backstop, SCOPE-001 undefined method, PAR-001 shared record) |
| `2026-07-01-1` | 2026-07-01 | `1ec0e66` | feat(contracts): reject array-vs-scalar @pre/@post predicates at compile time (ANN-001) |
| `2026-06-30-2` | 2026-06-30 | `c80b831` | docs(guide): clarify record construction + typed bindings (new T(), no fn new()) |
| `2026-06-30-1` | 2026-06-30 | `7868bef` | fix(parser): name reserved-word/member-name collisions in diagnostics (SYN-001) |
| `2026-06-29-1` | 2026-06-29 | `31fb8ff` | docs: rename the small-context guide to the "concise" guide (label only) |
| `2026-06-28-4` | 2026-06-28 | `2a8149b` | docs(small guide): remove two internal duplications |
| `2026-06-28-3` | 2026-06-28 | `8868305` | docs+examples: align guides and effects_contracts with the FX-001 effect model |
| `2026-06-28-1` | 2026-06-28 | `e8398ba` | effects: gate host interaction (FX-001/@pure), discovery-first docs, single-source effectfulness |
| `2026-06-27-5` | 2026-06-27 | `8643868` | aether: `_` digit separators in numeric literals (lang 2026-06-27-1) |
| `2026-06-27-3` | 2026-06-27 | `be03f58` | docs: document FUNC-001 (no function values) + the `par` concurrency pattern |
| `2026-06-21-1` | 2026-06-21 | `404a7b7` | docs(guides): version both guides (YYYY-MM-DD-N) + bump helper |

