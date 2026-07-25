# Migration plan: 0-based `Text` indexing

Status: **implemented 2026-07-25.** Written as a proposal first; the plan below
is kept as the record of what was decided and why. What actually shipped:

- `pscal-core` `47353dd` -- `frontendIsZeroBasedStrings()`, the VM index sites,
  `copy()`'s base and `pos()`'s base + `-1` sentinel.
- `aether` -- submodule bump, `s[a..b]` on a Text lowered to `copy()` (the
  "open question" below was answered yes), guide + fixture updates,
  `tests/text_zero_based_pass.aether`.
- Corpus -- 33 of 276 files edited (51 mechanical `copy()` rewrites plus 10
  reviewed files); see "Result" at the end.

Measurements below are against `aether` at `bf9f2d8` and the 276-file corpus in
`PBuild/Tests/aether_specialization/corpus_candidates`.

The **dataset rebuild and retrain are still outstanding** -- that is the one
step of the sequencing not carried out here.

## Decision being proposed

Make the whole `Text` API 0-based and uniform with arrays:

| Operation | Today | Proposed |
|---|---|---|
| `s[i]` | 1-based; `s[0]` is a runtime error | 0-based; `s[0]` is the first character |
| `copy(s, start, count)` | `start` is 1-based | `start` is 0-based |
| `pos(needle, s)` | 1-based; returns `0` when absent | 0-based; returns `-1` when absent |

The alternative -- moving only `s[i]` and leaving `copy`/`pos` 1-based -- was
rejected: it leaves two different bases inside one string API, which is worse
than either consistent choice.

## Why: the current design is measurably costing us

**Arrays are 0-based half-open; `Text` is 1-based.** The same loop idiom
therefore means different things, and the `Text` reading is silently wrong:

```aether
loop i in 0..length(xs) { ... }   // arrays: visits every element
loop i in 1..length(s)  { ... }   // Text: DROPS THE LAST CHARACTER, no error
```

Getting it right today requires `loop i in 1..length(s) + 1`, which nobody
writes naturally.

**Evidence from the last completed claw3 eval sweep** (`EVAL QUEUE COMPLETE`,
`aether_eval_queue_cs_aug19_precision_claw3`), by failure cause:

| Cause | Attempts failed | Tasks |
|---|---|---|
| `String index 0 out of bounds` | 6 | `cs_substring`, `cs_lcs` (3 each) |
| `Copy expects (String/Char, Integer, Integer)` | 9 | `cs_merge_sort` (3 at each of 3 points) |

For scale, the two largest *coded* causes in the same run were `SYN-001` (15)
and `SCOPE-001` (12). String-index confusion is a top-tier cost, not a rough
edge. (The `copy()`-on-an-array half of that is already mitigated by the
`TYPE-001` diagnostic added in `bf9f2d8`, which names `arr[lo..hi]`; the
diagnostic tells the model what to write, but the base mismatch is why they
reached for the wrong thing.)

**Evidence from the corpus.** Of 276 files, exactly **one**
(`24_palindrome_check.aether`) indexes a scalar `Text` with `s[i]` -- and it
counts *downward* from `string_len(s)` to 1 with a condition loop, avoiding
range loops entirely. Meanwhile **26 files** use `copy(s, i + 1, 1)` inside a
`0..length(s)` loop. That `+ 1` is the base mismatch made visible: models write
array-style 0-based loops, then pay a per-character token tax to bridge into a
1-based API. `s[i]` is effectively a dead feature, and the workaround that
replaced it is strictly more expensive for a small-context model.

After the change, the dominant idiom collapses from
`ord(copy(word, i + 1, 1))` to `ord(word[i])`.

## Correction to an earlier claim

An earlier note in this effort stated that Aether never registers its frontend
kind, and that fixing that would break every `@post` contract. **Both are
wrong.** Verified under lldb against the real binary:

```
(lldb) expr (int)frontendGetKind()
(int) $0 = 3          // FRONTEND_KIND_AETHER
```

Aether compiles rea's `main.c` via `ENGINE_SOURCES` (CMakeLists.txt:86). That
file's `PSCAL_FRONTEND_KIND` is `#ifndef`-guarded (`rea/main.c:64-66`), so
Aether's `-DPSCAL_FRONTEND_KIND=FRONTEND_KIND_AETHER` wins and the push at
`rea/main.c:831` sets AETHER. `frontendIsAether()` is therefore **live**, and
`frontendIsPascal()` is already false for Aether -- which is why `@post` cannot
be relying on Pascal's `result` handling. It works through Aether's own path.

Consequence: **no frontend-kind registration work is needed, and there is no
`@post` landmine.** The change is a targeted edit to existing, already-reached
branches. This makes the migration smaller than previously described.

## What changes

### 1. `external/pscal-core` (submodule)

The index base is already a per-frontend decision; Aether just needs to join
the 0-based side. Every site below currently reads `frontendIsShell()` and
should become a shared helper -- suggest `frontendIsZeroBasedStrings()` in
`common/frontend_kind.h`, returning `frontendIsShell() || frontendIsAether()`
-- so the policy lives in one place rather than being spelled out six times.

| File:line | What |
|---|---|
| `vm/vm.c:10011` | `expected_index` for `TYPE_UNICODE_STRING` (the `Text` path) |
| `vm/vm.c:10041` | `expected_index` for `TYPE_CHAR` |
| `vm/vm.c:10053` | `expected_index` for `TYPE_WIDECHAR` |
| `vm/vm.c:995` | `vmResolveStringIndex`, `is_shell_frontend` (narrow `TYPE_STRING`) |
| `vm/vm.c:6215, 7064` | `vm->shellIndexing = frontendIsShell()` -- rename to `zeroBasedIndexing` |
| `backend_ast/builtin.c` `vmBuiltinCopy` (~3234) | `start_idx < 1` guard and the `start_0based` computation |
| `backend_ast/builtin.c` `vmBuiltinPos` (~2683) | drop the `+ 1` on both return paths; return `-1` (not `0`) when absent |

Pascal, rea, and clike keep 1-based strings throughout -- the gate is
frontend-scoped, so nothing about their semantics moves.

**One real constraint.** Pascal's `s[0]`-is-the-length-byte sentinel
(`STRING_LENGTH_SENTINEL`, `vm.c:8663/9605/9967`) lives in the *non*-shell
branch of `vmResolveStringIndex`, guarded by `!frontendIsShell()`. Moving
Aether to the 0-based branch necessarily drops that length-query for Aether --
which is correct and intended (index 0 must become a real character), but those
three sites' guards must be widened in step, or `s[0]` will take the
length-query path instead of returning the first character. This is the one
place where a partial edit yields a silently wrong result rather than an error.

### 2. `aether` repo

| File | Edit |
|---|---|
| `src/aether/ast_parser.c:4207` | The `TYPE-001` string-slice hint hardcodes "`start` is 1-based" and the `s[a..b]` -> `copy(s, a, b - a)` translation. Both change. |
| `src/aether/ast_parser.c` (array-copy hint) | Says "half-open and 0-based" for slices -- already correct, recheck wording once `copy` moves. |
| `tests/text_index_char_builtins_pass.aether` | `s[1]`, `wide[2]`, `padded[1]` -> `s[0]`, `wide[1]`, `padded[0]`. |
| `tests/builtin_query_pass.aether` | 4 × `pos(...) > 0` -> `>= 0`. |
| `examples/base/builtin_queries` | 3 × `pos(...) > 0` -> `>= 0`. |
| `tests/string_slice_fail.aether`, `tests/array_copy_fail.aether` | Fixture comments + the `run.sh` assertions that match hint text. |
| `docs/aether_for_llms_and_others.md:777-778` | `copy`/`pos` table rows. |
| `docs/aether_for_llms_with_small_contexts.md:288` | Same, condensed form. |
| `docs/ideas_and_todo.md:274, 1545` | The 1-based statement and the open finding this plan closes. |

New regression fixtures to add: `s[0]` is the first character; `s[length(s) - 1]`
is the last; `loop i in 0..length(s) { s[i] }` visits every character exactly
once (the footgun, locked down); `pos` returns `-1` when absent and `0` for a
prefix match; `copy(s, 0, n)` takes from the start.

### 3. Corpus (`PBuild/Tests/aether_specialization/corpus_candidates`)

**38 of 276 files (14%)** need edits: 64 `copy()` calls and 7 `pos()` calls.

`copy()` start-argument shapes, by mechanical tractability:

| Count | Shape | Transform |
|---|---|---|
| 41 | `copy(s, VAR + 1, n)` | drop the `+ 1` -> `copy(s, VAR, n)` |
| 7 | `copy(s, 1, n)` | -> `copy(s, 0, n)` |
| 2 | `copy(s, <literal>, n)` | decrement the literal |
| 12 | `copy(s, <expr>, n)` | **manual review** |

So 50 of 64 are mechanical and scriptable; 14 need a human. Consider also
rewriting the 41 `copy(s, i, 1)` results to `s[i]` once the bases agree -- that
is the token-efficiency win, and it is what makes the corpus teach the idiom we
actually want. Treat that as a separate reviewed pass, not part of the
mechanical sweep.

`pos()`: 1 comparison (`!= 0`) plus 5 results bound to a variable and used
later -- all 7 need manual review, since the not-found sentinel moves from `0`
to `-1` *and* the returned index shifts by one.

### ⚠ The migration's own footgun

`pos(...) > 0` currently means "found". Under a `-1` sentinel it must become
`pos(...) >= 0`. Left unchanged it does not error -- it silently reports "not
found" for any match **at index 0**, i.e. every prefix match. There are 7 such
sites in this repo alone (`tests/builtin_query_pass.aether`,
`examples/base/builtin_queries`). Grep for `pos(` comparisons specifically;
do not rely on the test suite to surface these, because a prefix match is
exactly the case a smoke test is least likely to cover.

## Sequencing

Cross-repo, so ordering matters (see the standing rule about submodule pointer
bumps preceding upstream pushes -- a dangling pointer previously broke fresh
clones):

1. Land the `pscal-core` change upstream and push it.
2. Bump `external/pscal-core` in `aether`, with the repo-side edits and new
   fixtures in the same commit, so the tree is never half-migrated.
3. Corpus sweep: mechanical pass (50 `copy()` sites) + reviewed pass (14
   `copy()` + 7 `pos()`), then the optional `copy(s,i,1)` -> `s[i]` rewrite.
4. One dataset rebuild, one retrain.

Batch the corpus/dataset/retrain step with any other pending corpus-affecting
change so there is one cycle, not several. The two array-concat fixes
(`8ebf8e7`) and the two diagnostics (`2489b3c`, `bf9f2d8`) are deliberately
*not* in that batch: they only widen what compiles, so they required no corpus
edit and were landed standalone.

**Deploy hazard:** every commit in this repo reaches claw1/2/3 and swaps the
grading binary. Confirm no eval is in flight before each step.

## Verification plan

1. **Corpus A/B, all 276 files, old vs new binary.** The bar used for the
   recent fixes was zero exit-code differences; here differences are *expected*
   in the 38 edited files, so the check inverts: every one of the other 238
   must be byte-identical, and each of the 38 must produce the *same output as
   before its edit*. Beware three known-noisy files -- `10_monte_carlo_pi` and
   `03_concurrent_tally` (time-seeded/thread-ordered) and
   `74_ising_model_binder_cumulant` (nondeterministic run to run) -- plus two
   that print raw heap pointers. Compare interleaved, not sequentially.
2. **Targeted fixtures** for the boundaries: index 0, index `length - 1`,
   out-of-range both ends, empty string, multi-byte UTF-8 (the `é` case already
   in `text_index_char_builtins_pass`), `pos` prefix match, `pos` absent.
3. **Sibling-language guard:** run rea's and pscal's own suites against the
   rebuilt core to confirm 1-based behavior is untouched there.
4. **Re-run the eval sweep** and confirm the `String index 0 out of bounds` and
   `Copy expects` classes are gone rather than merely relocated.

## Rollback

The `pscal-core` change is a single frontend-gated predicate; reverting the
submodule bump restores 1-based behavior wholesale. The corpus edits are the
expensive half to undo, so do not start the corpus sweep until the compiler
half has passed step 1 and step 3 above.

## Open question for review — *answered yes, implemented*

Should `s[a..b]` on a `Text` become a real substring once the bases agree?
It was rejected with `TYPE-001` pointing at `copy()` (`2489b3c`). Slicing is
the most natural way to reach for a substring, and now that the bases agree
`s[a..b]` and `arr[a..b]` mean the same thing.

Implemented in `buildArraySlice`: a Text base lowers to `copy(s, a, b - a)`.
Because the bases now agree the translation needs no index fixup, and because
`copy` is an ordinary expression the slice stays an expression -- so unlike the
array case it hoists nothing and works in `ret s[a..b];`, which has no
statement slot to splice into. The `TYPE-001` rejection and its
`string_slice_fail` fixture are gone; the mirror diagnostic for `copy()` on an
*array* stays, since that one is still a genuine mistake.

## Result

Verified against all 276 corpus files, new binary vs the pre-migration
reference:

- **0 hard failures.**
- **265 byte-identical.**
- 10 differ only by inherent nondeterminism, each proven: 7 produce identical
  output when the old and new binaries are run interleaved (time-seeded RNG,
  thread interleaving), and 3 disagree with *themselves* across consecutive
  runs (two print raw heap pointers, one is the Ising model).
- 1 differs by exactly one line -- `aether_batch10_combined` prints `pos()`'s
  raw result, so `pos demo = 3` became `2`. That is the intended semantic
  change, not breakage, and the file was left alone.

The mechanical `copy()` sweep handled 51 of 58 call sites. The 7 it could not,
plus 3 files whose 1-based cursor lived outside a `copy()` argument, needed
review -- and one file, `25_kv_store_persistence`, is why the sweep could not
be fully mechanical: its *count* expressions were derived from `pos()`, so
rewriting only the `start` argument produced a plausible but wrong result. Any
future base change should treat "count derived from pos()" as a review case
from the start.

Two predicted traps both showed up in the corpus exactly as described:
`pos(...) != 0` as the found-test (which a `-1` sentinel makes *always true*)
in `06-29b_sched_m5_devstral_03`, and the 1-based `1..len` /`1..len+1` loop
idioms in four more files.
