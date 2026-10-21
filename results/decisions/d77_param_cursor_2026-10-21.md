# A scalar parameter used as a shared cursor: PARAM-001 (D77 evidence)

Language 2026-10-21-5 (1.0-beta4dev). Measured before the warning shipped, with
the same rule logging its decisions through `AETHER_EXPERIMENT_LOG`.

## The bug

A fine-tune wrote a recursive-descent parser whose levels share the read
position as an `Int` parameter, `fn parseTerm(expr: Text, pos: Int) -> Int`,
and advance it inside each callee with `pos = pos + 1`, expecting the
caller's `pos` to move. A scalar parameter is a copy, so each level's final
`pos` is discarded at return and every level re-reads from where it started.
The program compiles, runs and prints wrong numbers; three repair rounds did not
find it. The correct shapes are a tuple that returns the new position
(`-> (Int, Int)`), a nested fn that assigns the enclosing cursor (D64), or a
record field (records are reference-backed).

## Candidate rules

Every rule judges the same population: a scalar (`Int`, `Real`, `Bool`, `Text`)
parameter that its function assigns and never returns plainly (`ret q`, a tuple
slot `q`, or the @post `result = q`), excluding by-reference capture parameters.

- **naive**: warn on any such parameter. Rejected: 64 correct programs. A
  parameter used as the callee's own scratch counter is common and correct
  (`gcd(a, b)`, `siftDown(heap, i, n)`, `digitSum(n)`, coin-change loops), and
  the caller often reads its own variable after the call meaning its unchanged
  value (`println("gcd(", a, ", ", b, ") = ", g)`).
- **call site, read after the call**: the caller passes a local `v` and reads it
  before writing it. Still 5 correct programs (the caller printing or comparing
  its unchanged value, `if back == n`), so a read alone is not evidence.
- The three shapes below, each tied to evidence that the write was meant for the
  caller. They are what shipped.

## The shipped rule (once per callee parameter)

- **dead**: the callee updates the parameter from itself (`pos = pos + 1`,
  `buf = buf + s`, `q += k`) and a backward liveness pass over the body finds
  that value never read again. A self-update nothing reads can only have been
  meant for the caller.
- **thread**: a function whose own cursor parameter it advances and does not
  return passes it to a callee that advances the same kind of parameter, and
  then reads it in a condition or an index before writing it (the recursive
  descent shape: `parseExpr` calls `parseTerm(expr, pos)` and loops on
  `pos < length(expr)`). A write first (`pos = pos + 1`, `pos = next`) is the
  caller moving the cursor itself, and stays silent.
- **stuck**: the call sits in a loop whose condition reads `v`, and nothing in
  the loop body writes `v` (an assignment or a by-reference argument). The loop
  cannot advance (`loop pos < length(s) { let w = readWord(s, pos); ... }`).

Calls into an imported module's functions (`Fields.readWord`) are resolved, and a
dead update inside a module is reported under the module's path.

## Measurement

`param_cursor_census.py` (this directory) collects every Aether program the
repo holds, compiles each distinct source once with `--no-run --no-cache`, and
counts the programs that compile. Known correct: the seed and distilled pairs,
the repair drills' fixed sources, the corpus candidates, every dataset record,
the passing `tests/` and `examples/` programs, and every archived benchmark
attempt graded `pass` (`bench/results`, `training/training_runs/*/score`,
`training/distill/*/report__aether*`). Failing: every other archived attempt,
the drills' broken sources, and the `_fail`, `_warn` and backlog tests. A
program counts as correct only if every record of it does.

```sh
python3 results/decisions/param_cursor_census.py --aether-bin build/aether \
    --json results/decisions/param_cursor_census.json
```

| programs that compile | count | naive | PARAM-001 |
|---|---:|---:|---:|
| known correct | 6,393 | 64 | **0** |
| failing | 1,042 | 19 | 6 |

The six failing hits are the three `tests/fx/diag/param001_*_warn_pass`
fixtures and three archived attempts, all graded `silent_wrong`:

- two `hard2_expr_eval` first attempts by the no-guide fine-tunes (runs `modern`
  and `modern-p4`): Int cursors threaded through `parseExpr`, `parseTerm` and
  `parseFactor`. Warned at the thread shape in `parseExpr` and the dead
  `pos = pos + 1` after `parseExpr` returns in `parseFactor`;
- an on-device model's `mstream_roundtrip` attempt whose Void
  `write_buffer(buf: Text)` does `buf = buf;` and returns, a write that only the
  caller could have wanted.

With `--reports` adding the run-1 fine-tune's reports and the umbrella's
untracked board reports, both kept outside this repo, the counts are 6,469
correct (0 hits) and 1,087 failing (7 hits). The extra hit is the run-1
fine-tune's `hard2_expr_eval` first attempt, the program that motivated D77.

A regex screen of the failing programs that compile, for a parameter named
`pos`, `idx`, `index`, `cursor`, `i` or `p` that is advanced (`p = p + ...`,
`p = p - ...`), finds no program the warning misses.

`--check-gates` on the release binary passes: no training record prints
PARAM-001.
