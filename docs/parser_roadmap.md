# Aether Parser Roadmap — replace the text rewriter with a real AST frontend

Status: planned (2026-06-26). Owner: the Aether frontend.

## Why

Aether has no parser today. `src/aether/translate.c` (~9800 lines) is a **line-based
text rewriter**: it lowers Aether source to Rea source (text -> text), then rea's
parser builds the AST. That architecture is the root cause of a recurring bug class,
not isolated defects:

- **Compound-line mis-parses.** A field and a method (or a `fn` body) sharing a
  physical line dropped/garbled the trailing construct (fixed 2026-06-26 with a
  normalize pre-pass, but the fix is itself a symptom: the rewriter assumes one
  construct per line).
- **Unreliable error line numbers.** Verified empirically: an error on source
  line 3 of a 1-function program is reported at line 5, and the offset *scales*
  (+2 for 1 fn, +5 for 2, +8 for 3, roughly +3 per top-level function, worse with
  compound lines). Cause: the rewriter prepends forward declarations and splits
  one-line bodies, and the rewrite line-map is **write-only** (`aetherNoteRewriteLineMapping`
  has no reader) while rea prints its own post-rewrite line. This misleads the
  benchmark's repair/"second chance" pass.
- **Every new feature is another text-rewrite special case** (TOON, inline-if,
  contracts, tuples, JSON aliases, one-liner expansion, compound-line normalize...).

A real lexer + parser producing an AST with source positions fixes the whole class
**by construction**: nodes carry line/col, and nothing cares how lines are laid out.

## Design: parse straight to the shared AST, keep the backend

Build an Aether lexer + parser that emits the **shared pscal/Rea AST directly**, and
reuse everything below the parser unchanged:

- Reuse rea's lexer (`rea/lexer.c`) as the base; add/confirm Aether tokens.
- Emit the same pscal AST nodes rea's parser builds (so semantic analysis, codegen,
  and the VM are untouched).
- Keep Aether's own semantic layer (`aether/semantic.c`: effects, contracts, purity).

Pipeline change (frontend only):

```
now: source -> aetherRewriteSource (text) -> parseRea -> AST -> semantic -> codegen -> VM
new: source -> aetherLex -> aetherParse -> AST (+positions) -> semantic -> codegen -> VM
```

`translate.c` already encodes what every construct *means* in Rea/AST terms. That
knowledge **is the spec** for the parser's AST-construction rules; porting is
re-expressing text->text rules as tokens->AST rules.

## Phases (incremental, behind `AETHER_PARSER=ast`)

- **P0 Scaffolding.** Add the `AETHER_PARSER` flag; `parseAether()` dispatches:
  `ast` -> new parser, else -> the rewriter (default during dev). Run the test
  suite under both. *Done when:* the flag toggles with no behavior change.

- **P1 Lexer.** Reuse rea's lexer; confirm Aether tokens: `fn ret let const if else
  loop fx type new`, `->`, `:`, `..`, `@` (pure/pre/post/cost), literals, comments.
  Line+col on every token. *Done when:* every suite fixture + benchmark program lexes.

- **P2 Core grammar -> AST (recursive descent).**
  - Program: top-level `fn`, `type`, `const`, `@`-annotated decls.
  - `type N { fields; methods }` -> record/class node; inject the implicit receiver
    as the first method param (crib rea `parser.c` ~2679).
  - `fn f(p: T) -> R { body }` -> function node.
  - Statements: `let`/`const name: T = e`, assignment, `if/else` (statement),
    `loop i in a..b`, `fx { }` (-> block), `ret [e]`, expression-statement.
  - Expressions (precedence climbing): literals, identifiers, calls, `.` member,
    `[]` index, `new T { ... }` record literal, unary/binary ops, and
    `if c { a } else { b }` as an **expression**.
  *Done when:* v2/cs/core suite programs parse -> AST -> run with byte-identical
  output to the rewriter path.

- **P3 Construct -> AST mapping.** Fill the table below with the exact pscal AST node
  per construct, read from translate.c's current emission.

- **P4 Aether semantics.** Point the effect check (FX-001: effects only inside `fx`),
  contracts (`@pre/@post/@cost`), and `@pure` at the new AST. Verify `aether/semantic.c`
  is AST-based (likely) and not coupled to rewritten text.

- **P5 Exotic features.** TOON blocks (may stay a pre-pass at first), tuples +
  destructuring, stdlib aliases (`toon_*`, print aliases), inline-if decls. One at a time.

- **P6 Validation gates.**
  - `tests/run.sh` full suite green, including the `*_fail` negatives reporting the
    right diagnostics and `compound_lines_pass`.
  - Benchmark (tasks_v2_pos / tasks_cs / tasks_hard) at **parity** with the rewriter
    path across the model corpus, the hundreds of real LLM-generated programs are the
    conformance set.
  - New line-number tests (the LN1-LN5 cases): errors report the true source line.

- **P7 Cutover (translate.c retained).** Flip the default so the AST parser is the
  frontend, but **keep `translate.c` and the rewrite line-map in the tree** as an
  opt-in legacy path (for reference, A/B comparison, and fallback) — do NOT delete.
  Steps:
  1. Invert the dispatch in `parser.c` `parseAether()`: default (env unset) ->
     `parseAetherAst`; `AETHER_PARSER=rewriter` (or `=legacy`) -> the old rewriter
     path. (`AETHER_PARSER=ast` stays valid as an explicit synonym for the default.)
  2. Update the `*_fail` fixtures' expected diagnostics to the AST's (it reports the
     true line and a more precise message than the rewriter's lenient strings); add
     the LN1-LN5 true-line tests.
  3. Bump the language version via `tools/bump_version.py` (diagnostics changed) +
     CHANGELOG. The guide needs no change.
  4. Re-run `tests/run.sh` (now default = AST) + the benchmark-corpus parity, and a
     spot A/B against `AETHER_PARSER=rewriter` to confirm the legacy path still works.
  - **Gating:** only flip once the worth-fixing P6 gaps are closed and corpus parity
    is ~99%+ on the AST-worse axis, so the flip is not a regression for real programs.
  - **Pre-cutover safety:** because the rewriter is retained, the flip is reversible
    at runtime (`AETHER_PARSER=rewriter`) and the legacy code stays buildable; retiring
    it later is a separate, optional step (owner's call: keep for now).

## Construct -> AST mapping (to complete in P3)

| Aether | AST node (fill from translate.c) | Notes |
|---|---|---|
| `fn f(a: T) -> R { }` | function decl | params, return type, body block |
| `type N { x: T; fn m() {} }` | record/class | fields + methods; implicit receiver as first param |
| `let x: T = e;` / `const` | typed var/const decl | explicit type |
| `fx { stmts }` | block | effect marker erased at lowering; checked in semantic |
| `ret e;` / `ret;` | return | value vs void |
| `if c {} else {}` (stmt) | if | |
| `if c { a } else { b }` (expr) | if-expression | value position |
| `loop i in a..b { }` | counted/for loop | range -> bounds |
| `new T { f: v }` | record literal | the only "value brace" |
| `@pure/@pre/@post/@cost` | fn attributes + semantic checks | |
| stdlib aliases | calls to canonical builtins | |
| tuples + destructuring | tuple type + destructure | |

## Risks + mitigations

- **Exotic long tail (TOON/contracts/tuples/aliases):** keep the rewriter behind the
  flag; port one feature at a time; the benchmark catches regressions; TOON can stay
  a pre-pass initially.
- **AST coverage gap:** unlikely (the rewriter already lowers everything to Rea AST
  today); surface any gap in P2.
- **Parity drift:** require equal benchmark pass counts before cutover.

## Payoff

Correct line/col in every diagnostic (the repair pass becomes trustworthy; the
line-map "option A" is subsumed). The compound-line bug class is gone. New features
are a grammar rule + an AST node, not a text-rewrite special case. ~9800 lines of
`translate.c` retired.

## Estimate

Core P0-P2 ~1 focused day; exotic tail P4-P5 ~1 more depending on TOON/contracts
depth; P6-P7 gated on benchmark parity. See [[aether-frontend-rewriter]] for how the
current rewriter works and how to debug/dump it.
