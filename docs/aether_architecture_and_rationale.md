# Aether Architecture & Rationale (as-built)

*Audience: human maintainers.* This document explains how Aether actually works
today and **why** specific implementation decisions were made — including the
awkward ones. It is deliberately separate from:

- [`src/aether/DESIGN.md`](../src/aether/DESIGN.md) — the original design
  *vision* (goals, phases), written 2026-06. Historical: where it and this
  document disagree, this document and the decision register win.
- [`docs/aether_for_llms_and_others.md`](aether_for_llms_and_others.md) — the
  *reference guide* fed to models and humans who just want to write Aether.
  Descriptive, not rationale.

This document is the third leg: the *as-built* engineering rationale. If you are
about to change the compiler, add a builtin, edit the corpus, or wonder "why on
earth is it done *this* way," start here.

---

## 1. The thesis, and what is actually measured

> Aether is optimized so that a language model can write **valid, correct Aether
> from as little in its prompt as possible**, and the benchmark suite — not taste —
> is the instrument that tells us when the language design is wrong.

This section used to name no-guide correctness (`none`) as the one hard target.
That is not what the project measures or runs on. The newest no-guide board in
this repo is cs-aug20 (fine-tuned models, graded by aether `c660b1b`), and every
board since has handed the model a guide. In practice the project runs on "the
guide is enough", and the KPIs now say so (decision **D9**, option (a)):

- **G, the medium guide on a local open-weight panel, is the primary KPI.** It
  is what the benchmark runs and what most users of the language will do.
- **P, the frozen card** (`docs/aether_card.md`, about 2,000 tokens), on the same
  panel, says how close Aether is to what models already believe. The gap
  between G and P is the prior-alignment signal, and it needs no GPU retrain.
- **N, no guide on the fine-tuned reference model**, stays the long-run test of
  whether the language can be learned, measured cheaply by replay and retrained
  only at milestones.
- **S, the silent-wrong share,** is a release gate: no release may raise it.

§3.1 defines all six KPIs. "Compact and human-auditable" (the DESIGN.md framing)
remains necessary but secondary: DESIGN.md's "≤1.1× Python tokens" goal is
retired as a gate and survives only as the report-only KPI T.

---

## 2. The compile pipeline, as actually built

Aether now lives in its **own repository** (`emkey1/aether`). This repo holds only
the Aether front end (`src/aether/`), its docs, tests, examples, and
`CMakeLists.txt`. The shared engine and backend are pulled in at build time via
`FetchContent`: `aether` fetches **`rea`** (`emkey1/rea`), which transitively
fetches **`pscal-core`**; after a build they appear under `build/_deps/rea-src/`
and `build/_deps/pscal_core-src/`. So the path references below span this repo
(`src/aether/…`), the fetched **rea** and **pscal-core** deps, and the benchmark
harness in the **PBuild umbrella** — they are no longer all in one tree.

```
Aether source
  → source pre-passes                  (this repo:  src/aether/ast_prepasses.c)
  → Aether recursive-descent parser    (this repo:  src/aether/ast_parser.c)
  → Aether semantic checks             (this repo:  src/aether/semantic.c)
  → shared PSCAL AST                    (pscal-core: src/ast/…)
  → Rea engine semantic analysis       (rea dep:    src/rea/…, via the hook seam)
  → shared bytecode compiler           (pscal-core: src/compiler/…)
  → shared PSCAL VM                     (pscal-core: src/vm/…)
```

Key facts a maintainer must internalize:

- **Aether is a standalone AST frontend** (since the P7 cutover, 2026-06-27).
  `ast_parser.c` tokenizes with Rea's lexer and builds the shared PSCAL AST
  directly, in the same node shapes Rea's own parser produces, so everything
  downstream is the shared pipeline unchanged. The bootstrap-era line-based
  text rewriter (`translate.c`, the transitional path DESIGN.md §6.2 once
  described) was **retired and deleted on 2026-07-01** — see `parser_roadmap.md` for
  the migration history. **There is no separate Aether VM, bytecode, or
  runtime.** That is a hard architectural rule.

- **The front end plugs in through a runtime hook seam, not a forked in-tree
  engine.** The build compiles Rea's engine sources (`ENGINE_SOURCES` in
  `CMakeLists.txt`, taken from the fetched `rea`) together with `src/aether/`, and
  `src/aether/state.c` registers Aether's parser/semantic/module behaviour via
  `reaSetFrontendHooks(&hooks)` (implementing `rea/frontend_hooks.h`). That seam is
  why there is no copy of Rea in this tree: Aether supplies *behaviour* through the
  hook struct and the shared engine calls back into it. Recent work moved every
  front-end entry point onto this seam.

- **One binary, two personalities.** The `aether` binary is built from Rea's
  `main.c` (`src/rea/main.c`, in the `rea` dep) with `PSCAL_FRONTEND_KIND == FRONTEND_KIND_AETHER`. At runtime
  `frontendGetKind()` distinguishes Aether from Rea so the *same* code can behave
  differently for each front end (see the writeln-spacing decision below, which
  keys off exactly this). Grep `FRONTEND_KIND_AETHER` to find every such fork.

- **Because the backend is shared, an Aether-motivated fix usually improves the
  whole suite** — but it can also break Rea/CLike/Pascal. Any change gated on
  `frontendGetKind() == FRONTEND_KIND_AETHER` is Aether-only and safe; any
  un-gated change in pscal-core's `src/ast`, `src/compiler`, `src/vm`, or
  `src/backend_ast` is cross-cutting (those live in the `pscal-core` dep, not this
  repo). Know which kind you are writing.

The [known warts](#6-known-warts-be-honest) section below is largely a record
of the retired rewrite-layer era; it is kept because it explains why several
design decisions and tests look the way they do.

### 2.1 Front-end internals: splice groups, temps and hoists

Several Aether shapes have no PSCAL AST node and are lowered to a run of ordinary
statements: array `+` (`buildArrayConcatSteps`, `ast_lower.c`), array append,
slices, `ret T { ... }`, `ret a + b`, record literals in expression position,
tuple destructuring and the range/foreach/`step` loops. Three conventions hold
those lowerings together; a new desugaring should reuse them, not invent a fourth.

- **Splice groups.** A lowering that needs several statements returns an
  `AST_COMPOUND` with `i_val == 1`. `parseBlock` splices such a compound's
  children in as siblings instead of opening a scope, so a `let` produced inside
  one stays visible to the statements after it. It splices **one level only**:
  a splice group nested inside another wrapper survives as a real block and
  scopes its declarations away (the cause of the 2026-07-24 `let r = a[1..3] + b`
  SCOPE-001, fixed in 8ebf8e7 by flattening the statement in `parseStatement`'s
  hoist wrapper too).
- **Hoists.** An expression-position shape that needs a temporary (a record
  literal as an argument, for instance) queues its declaration on
  `pendingObjLits`; `parseStatement` flushes the queue ahead of the statement
  being parsed, as one splice group. A lowering that needs a *statement*
  position and is not given one is the classic failure: before 8ebf8e7,
  `ret a + b` had nowhere to put the copy loop, the raw array `+` reached the VM,
  and it died with "Got ARRAY and ARRAY".
- **Temps.** Synthesized locals are named `__aether_<kind>_...` and must be
  unique per program, not per line: `__aether_concat_other_<line>_<serial>`,
  `__aether_slice_<id>` and `__aether_lit_<id>` (from `nextObjLitId`), and so on.
  Line-only names collide when one line holds two lowerings (`mk() + mk() + mk()`
  failed with "duplicate variable" until the concat temps gained a serial,
  CHANGELOG 2026-08-09-1). A lowering evaluates each operand once, into a temp,
  before it reads it more than once. The `__aether_` prefix is a convention, not
  yet reserved: user code may still declare such a name today.

Ownership: the lowering helpers only read the nodes they are handed and copy what
they keep (`buildLengthCall` copies its target), so passing a node that is
already a child of the tree is safe.

### 2.2 The lowering contract

Most front-end defects found so far are one of four lowering failures: a shape
lowered in some expression positions and not others, a synthesized name that
collides with a user's, a hoist that runs a side effect out of order, and a check
that trusts the AST's resolved types. Every new desugaring (for example
`[v; n]`, `par` return values, a Text-store desugar) follows these rules, and a
review of its commit checks them:

1. **Every position or a code.** An Aether-only shape is lowered in every
   expression position it can appear in, or rejected there with a coded
   diagnostic. A raw shape reaching the VM is a bug. Today this does not hold
   everywhere: array `+` lowers in `let`, assignment and `ret` position, but
   `total(a + b)` with two arrays compiles and fails at run time with "Got ARRAY
   and ARRAY".
2. **Hygienic names.** Compiler-synthesized references resolve to the builtin
   or temp they were built for, never to a user binding of the same name, and
   the `__` prefix is reserved for the compiler. Not yet enforced (see 2.1).
3. **Evaluation order.** A hoisted temp is placed where its expression would
   have been evaluated, so side effects run in source order and exactly once.
4. **The Aether type oracle, not `var_type`.** A check on a builtin's type uses
   Aether's own inference (the front end's builtin tables and declared
   signatures). The shared AST's `var_type` is wrong in exactly the places that
   matter during semantic analysis; CHANGELOG 2026-07-26-4 lists the cases
   (`min(3, 5)` annotated REAL, `sqr(3)` VOID, a call's own `var_type` still 0).
5. **One fixture per position.** A desugaring ships with a fixture for each
   position it supports or rejects: statement, guard, `else if`, loop
   condition, call argument, tuple item, record field and if-expression.
6. **Policy is a named predicate.** Aether-only behaviour in the shared code is
   a named predicate in pscal-core's `frontend_kind.h`, in the style of
   `frontendIsZeroBasedStrings()`, rather than another bare
   `frontendIsAether()` test. A bug that every front end shares is fixed
   ungated.
7. **No Aether IR** (decision D31). The parser keeps building the shared PSCAL
   AST directly, as the parser roadmap decided. Revisit only when three or more
   typed-pass rules have to reconstruct surface shapes that lowering destroyed.
   The first such case is on record: array `+` checks no element types (with
   `a: Int[]` and `t: Text[]`, `let c: Int[] = a + t;` compiles and runs today),
   and a typed pass run after lowering sees only the `setlength` and indexed-copy
   statements, not the `+`. One is not three; the check belongs in the lowering.

---

## 3. The benchmark is the design instrument

This is the most important idea in the project and the least obvious from the
code. Read this section before proposing any "the language should…" change.

### 3.1 What the benchmark is, and the KPIs it reports

`aether_doc_bench.py` (in the PBuild umbrella harness, not this repo) issues a
task, compiles and runs the program the model returns with the `aether` binary,
and scores its stdout byte for byte. With repair on, the compiler's diagnostics
go back to the model for up to two more attempts. The suites in current use are
`tasks_v2_pos` (35 simple tasks, v`2026-07-15-1`), `tasks_hard_v2` (14 large,
v`2026-07-28-1`), `tasks_cs` (19 CS classics, v`2026-06-23-1`) and
`tasks_hard_nontoon` (5 non-TOON hard, v`2026-07-27-1`). The trap suite G and S
need, `tasks_traps` (tasks whose natural reading in another language prints
something else, scored with the expected output hidden on the first attempt,
D37a), is being added by the bench workstream. Each task runs under a
**documentation variant**:

| Variant  | What's in the prompt                         | What it measures                          |
|----------|----------------------------------------------|-------------------------------------------|
| `full`   | the full guide (~29K tokens)                 | ceiling: can the model follow the spec?   |
| `medium` | the working guide (≤15K)                     | **the primary KPI, G**                    |
| `small`  | the concise guide (16K-window tier, D11)     | working memory under a tight budget       |
| `card`   | the frozen card, `docs/aether_card.md` (~2K) | prior alignment, P                        |
| `none`   | no guide                                     | what a model has internalized, N          |
| python   | (baseline) the same task in Python           | task difficulty floor and the token bar, T |

The KPIs (D9), pre-registered here before the boards that report them:

| KPI | Definition | Cadence | Gate |
|---|---|---|---|
| **G** | `medium` on the G panel: 4–5 local open-weight destinations with hashed weights (D37d), suites `v2_pos` + `cs` + `hard_nontoon` + `traps`, sampled per D37b (T=0.2, seed 42+r, 3 repeats), repair 2. Report **G1** = first-attempt exact-pass share and **Gf** = final exact-pass share, each with a confidence interval | each `VERSION` or guide-stamp bump, not nightly | non-inferiority: a change is adopted only if G1 does not fall by more than 3 points, by a bootstrap blocked by task, model and seed (D37d) |
| **P** | `card` on the same panel and suites; **P1**, **Pf** as for G. G1 − P1 is the prior-alignment gap | with G | report |
| **S** | silent-wrong share: attempts that exit 0 with wrong stdout, over all attempts, on `traps` + `v2_pos`, under `card` and `medium` | each release | **no release raises S** |
| **N** | `none` on the fine-tuned reference model: paired replay of its recorded programs on each bump; a retrain and fresh grade only at milestones (D9: the last M5 item if a rig week is free, otherwise M6) | replay per bump | report |
| **R** | replay regressions: recorded programs that passed on the previous `VERSION` and fail on this one, minus those explained in the CHANGELOG entry | each `VERSION` | **R = 0** |
| **T** | tokens to an exact pass, repairs included, against Python on the same model and suite | after one persisted `--python-baseline` run | report only |

Untrained `none` is not a KPI: it measures the floor of a model's priors, and
the idea miner already reports where models reach. The `full` variant on frontier
models is a sanity ceiling, not a KPI: B0-full (two cloud models, `v2_pos` +
`hard_v2`, decision D3) came back at 99–100% first-attempt accuracy, which is a
ceiling effect and cannot rank a language change.

**What the README's "Evidence status" block carries** (filled in from the first
board after the B0/B1 baseline, and refreshed with each release's measurement
row): the latest G1/Gf, P1/Pf, S and N with their dates; the `VERSION` and the
guide and card stamps they were scored on; the suite versions; and the sha256 of
the bench MANIFEST that identifies the harness (D41).

### 3.2 The inversion: a hard task is a language bug, not a model bug

The crucial mental flip:

> If a task can only be solved by a frontier-scale model, that is evidence the
> **language design** is wrong — not that the model is weak.

Aether's premise is that the language is easy for models to learn. So a task that
defeats every small/mid model is a defect *report* about Aether. Recent examples
of tasks that, at one point, **no** in-house tuned model could pass:
`release_board`, `toon_safe_nested_codes`, `config_validator`. Each became a clue,
not a scoreboard entry.

### 3.3 The repair loop

When a model fails a task (any variant; `none` and `card` failures are the most
informative about priors), triage the failing generation
(`tools/none_fail_detail.py <eval>.json none`) into one of two buckets:

1. **Wrong prior** — the model wrote something reasonable from another language
   that Aether rejects (`return`, `elif`, `xs.push(v)`, a bracket-indexed tuple
   `t[0]`). The language is fine; the model guessed. **Fix:** add a verified
   `broken → fixed` repair drill to the corpus (see
   [§7](#7-corpus-core--per-family-overlays)). Look the spelling up in the
   surface registry first (§5.7): a class-1 or class-4 spelling (`new T{}`,
   `..=` once D7 ships, `parse_json`) is accepted, so a drill that "repairs" it
   teaches models to avoid valid code.

2. **Language defect** — the natural, correct-looking program *should* work but
   doesn't (it crashes, needs verbose staging, or lacks an obvious builtin).
   **Fix:** change the language/runtime so the natural program works, then let
   every model benefit for free.

Bucket 2 is where the recent TOON and `clamp` work came from. Those are worked
through in [§5](#5-nitty-gritty-decisions-and-why). The discipline is: **prefer a
language fix over a corpus patch whenever the natural program is the correct one**
— a corpus patch teaches one family to tiptoe around a wart; a language fix
deletes the wart.

---

## 4. Source naming: say what the model expects, lower to what the VM has

A recurring pattern: Aether's *surface* name is chosen to match what a model
trained on mainstream languages will reach for, and the rewrite layer lowers it
to the PSCAL backend spelling.

- `print` / `println` (surface) → `write` / `writeln` (backend).
- `int_to_text(n)` (surface) → `IntToStr` (backend), and the front end infers its
  return type as `Text` (alias lowering in `src/aether/ast_prepasses.c`,
  return-type inference in `src/aether/ast_parser.c`).
- `has_toon()`, `toon_*`, `task_spawn`, `ai_chat(...)` — compact source helpers
  in place of raw backend/extension builtin identifiers.

The rationale is always the same: a model emits the name it learned elsewhere; if
that name compiles and does the obvious thing, the `none` score goes up and the
corpus shrinks. Adding a well-chosen alias is often cheaper and more robust than
training the alias *out* of every model family.

---

## 5. Nitty-gritty decisions, and why

Each subsection states **the problem the benchmark surfaced**, **the decision**,
and **the rationale**. Commit hashes are given so you can read the actual diff.

### 5.1 `writeln` emits arguments verbatim (no magic spaces)

- **Problem.** Pascal/Rea `writeln(a, b)` inserts implementation-defined spacing
  between arguments. A model writing `println("x=", x)` expects `x=5`, but got
  `x= 5` (or worse), so output never matched and tasks failed for a reason that
  had nothing to do with the program's logic.
- **Decision.** `gSuppressWriteSpacing` (`src/rea/main.c:1084` in the `rea` dep):
  ```c
  gSuppressWriteSpacing = (frontendGetKind() == FRONTEND_KIND_AETHER) ? 1 : 0;
  ```
  Aether suppresses inter-argument spacing (verbatim concatenation, the Python
  `print(sep="")` mental model); Rea keeps its historical spacing.
- **Rationale.** Models trained on `print()`/template-string languages expect
  concatenation, not Pascal field formatting. Gated on `FRONTEND_KIND_AETHER`, so
  Rea is untouched. This is the canonical example of the "one binary, two
  personalities" fork.

### 5.2 `int_to_text()` exists at all

- **Problem.** Models reach for an int→string helper by an obvious name; `IntToStr`
  is a Pascal spelling they don't predict, so they invented `int_to_text`,
  `to_text`, `str(...)`, etc.
- **Decision.** Provide `int_to_text` as a first-class surface alias of `IntToStr`
  with `Text` return-type inference.
- **Rationale.** Cheaper to accept the obvious name than to retrain every family
  away from it. (Other guessed spellings are still corpus-repair territory; this
  one was common enough to bake in.)

### 5.3 TOON degrades on invalid handles instead of crashing  (`c30d16260`)

- **Problem.** TOON accessors took an integer handle into a yyjson document. A
  model writing natural defensive data code — read a node that might be missing,
  then ask for a field — would pass an "invalid" handle and the runtime
  **aborted**. The correct-looking program crashed, so models learned to write
  verbose multi-stage guard scaffolding, which then failed for *other* reasons.
- **Decision.** Remove the hard error on invalid handles in pscal-core's
  `src/ext_builtins/yyjson/yyjson_builtins.c`. Accessors now degrade: navigation
  returns "invalid," `toon_has_key` returns false, and the `_or` getters return
  their supplied default.
- **Rationale.** The natural, un-staged program is the *correct* program; punishing
  it with a crash is a language defect (bucket 2). After the fix, the natural
  solution to `toon_safe_nested_codes` compiles and runs, flipping it FAIL→PASS
  for the tuned 7B and turning other models' crashes into runnable output.

### 5.4 TOON dotted-path key access  (`da678a1b6`)

- **Problem.** Reading `server.name` out of nested data required descending one
  level at a time: `toon_key(root,"server")` → `toon_key(that,"name")` →
  `toon_get_text(...)`. Models naturally wrote `toon_get_text_or(root,
  "server.name", def)` and got the default back every time.
- **Decision.** When a key contains `.`, the accessors
  (`toon_get_*_or`, `toon_has_key`, `toon_key`) walk each dotted segment and fall
  back to the default / false on any missing segment. **Plain (non-dotted) keys
  are byte-for-byte unchanged** — the dotted path is an additive fast path. Both
  the existence check (`HasKey`) and the value lookup (`GetKey`) had to learn the
  navigation, or `_or` getters would disagree with themselves.
- **Rationale.** The dotted path is what models write and what humans read; the
  one-level-at-a-time descent was pure ceremony and a reliable `none` failure.

### 5.5 `clamp(x, lo, hi)` (and friends `min`/`max`)  (`efdc178a0`)

- **Problem.** Bounding a value made models write `if x < lo { x = lo }` chains —
  which then collided with the one-liner-`if` wart ([§6](#6-known-warts-be-honest))
  and failed to parse. The obvious builtin simply didn't exist.
- **Decision.** Add `clamp(x, lo, hi)`: all-integer args return `Int`, any real
  arg returns `Real` (mirroring `min`/`max`).
- **Rationale.** A one-call builtin sidesteps the whole error-prone branch-chain.
  See [§8](#8-anatomy-of-a-builtin-worked-example-clamp) — adding it correctly
  touches four sites, and getting that wrong is a classic foot-gun.

### 5.6 `fx { … }` effect blocks

- **Decision / rationale.** Calls to effect-gated builtins (I/O, the clock,
  randomness, processes, the network) must sit lexically inside an `fx` block
  (FX-001). It is a *front-end semantic fence*, not a new VM frame, which is
  consistent with the shared-backend rule.
- **As built.** `fx` is a lexical audit marker for gated builtin call sites. It
  is not transitive: a user function may wrap its own `fx { println(..) }` and
  be called from anywhere, so effects pass unmarked through user functions. The
  checked guarantee is `@pure`, enforced by annotation (ANN-001): a pure
  function may not contain an `fx` block, call an effectful builtin, or call a
  function that is not itself `@pure`. `@pure` does not rely on `fx`; the
  earlier wording here ("the fence that makes `@pure` meaningful: a pure
  function provably contains no `fx`") overstated the link. The cost: `fx`
  shows up a lot in generated code, which is why its interaction with one-liner
  `if` is the most-felt wart. The measured cost in repairs is D12's gate
  (`results/decisions/fx_gate_2026-10-07.md`).

### 5.7 Four synonym classes, not one canonical spelling

This section used to say that Aether admits one form of each construct and
rejects the rest. The shipped surface never matched that (four record-init
forms, `while`/`for` beside `loop`, `+=`, `?:`, `.len()`, Text + number), and
the rule did not survive measurement: a rejected synonym costs a repair turn
that the no-guide tier does not have, and an accepted synonym with an exact
meaning costs nothing. Decision **D7** replaced it with four classes:

1. **Accepted with exact meaning; may be taught.** `while`/`for`, the word
   operators, `Float`/`String`, `itoa`, `len`/`.len`, bare `T { ... }` (with
   `new T { ... }` canonical), Text + number (left-to-right stringify),
   `+= -= *= /= %=`, `Int()`/`Real()`/`Bool()` casts, `exit(n)`; and, decided
   but not shipped yet, `..=`, `xs.length`, `[v; n]`, `x:.2` and a matching tuple
   annotation.
2. **Rejected with a coded diagnostic that names the Aether form.** `elif`,
   `foreach`, `match`/`switch`, `return`/`class`/`import`, comma-separated
   fields, `Int[N]`, `fn new`; still owed a code or a hint: `++`, the bitwise
   compound assignments, `.push`/`.size`/`.contains`, `new Int[n]`, lambdas.
3. **Never accepted, because it means something different in another
   language.** `//` as division, chained comparisons, `not X == Y`, `{}`/`%d`
   placeholders, `int(Text)`, record `==`, each with its decision row (D45, D32,
   D48, D14, D24). Placeholders and f-strings are rejected with FMT-001 (W6-04),
   and `//` is always a comment, coded DIV-002 after an expression (W8-14); the
   others are still silently accepted today.
4. **Tolerated, not taught.** Hidden aliases such as `toon_parse_string`,
   `parse_json`, `root_node` and `lookup_*`, and `?:`.

The list itself lives in [`tests/surface/registry.json`](../tests/surface/registry.json),
one entry per spelling with its class, canonical form, code and decision row, and
a fixture. `tools/check_surface_registry.py` (CTest `aether_surface_registry`)
proves each class: a class-1 or class-4 fixture prints exactly what its canonical
twin prints, and a class-2 or class-3 fixture fails with exactly its code. An
entry decided but not shipped is marked pending and pins what its fixture does
today, so the commit that ships it flips the entry. The guides, the corpus
exporter's non-canonical patterns and the repair-drill polarity should all be
read from this one file rather than kept as four lists that disagree.

Two details from the old text still hold, now verified: a field list is
`name: Type;`, and comma-separated fields are rejected with SYN-001 (not silently
parsed as no fields, which is what this section used to claim).

### 5.8 Contracts: what is enforced

`@pre` and `@post` lower to real checks: an entry guard and a guard before each
return, which stop the program with `Aether @pre failed in f` (or `@post`) on
stdout and exit status 1. `@pure` is checked statically: a pure function may not
contain an `fx` block or call an effectful builtin (ANN-001). The front end
rejects an annotation it can recognise as malformed: `@pre` with no expression,
`@pure noisy`, a `@cost` with a zero budget, an unknown unit or a duplicate, and
any annotation that is not directly above a function (ANN-001), or one written
inside a body (SYN-001). Tuple post-conditions use dot indexing,
`@post result.0 >= result.1`; `result[0]` is ANN-001.

Not enforced, and not to be relied on: `@cost` is decorative by decision (its
syntax is validated, nothing measures the budget), and an unknown `@word`,
`@pure()` and `@cost(5)` are accepted today and ignored. W4 makes those three
errors; until then a contract the compiler does not act on can still sit in the
source.

---

## 6. Known warts (be honest)

A maintainer will hit these; pretending they don't exist wastes their day.

### 6.1 The one-liner `if` / inline-`fx` parse failure (SYN-001) — *fixed*

Historical; kept because it's the canonical illustration of the line-orientation
trap and how to fix that class of bug without touching the shared backend.

A guard written on a single line used to fail:

```aether
if x > 3 { fx { println("big"); } ret; }      // was: SYN-001 / SCOPE-001
```

while the identical multi-line guard worked. The cause was **not** the Rea
grammar (multi-line lowers fine) — it was the line-oriented rewrite layer's
one-construct-per-line assumption: `fx`/`ret` are only rewritten when the keyword
*leads* a line, so a one-liner left the mid-line `fx`/`ret` untranslated (`ret`
reached Rea verbatim → `SCOPE-001`; raw `fx {` → `SYN-001`).

**The fix** (rewriter-era; that code is now deleted) — `expandInlineBlockLine` in `translate.c`. When a line's leading
block construct (`if`/`else`/`while`/`for`/`loop`/`fx`) has its opening brace
matched **on the same line**, it is expanded into the canonical multi-line form.
Each header runs through `translateLineInMethod` (the main loop's header path)
and each **body statement** runs through the *same specialized per-statement
handlers the main rewrite loop applies, in the same priority order* — tuple
return (`translateTupleReturnLine`), return object-init / return-with-post,
array append (`translateArrayAppendLine`), then `translateLineInMethod` as the
fallback — followed by `applyJsonAliasesToLine` (so `toon_*` calls in conditions
still resolve). This is what makes a one-liner body lower **identically** to its
multi-line form: an earlier version translated body statements with bare
`translateLine`, which skipped those handlers, so a one-liner `ret (a, b);`
leaked `return (a, b);` (SYN-001) and `xs = xs + [v];` leaked an `ARRAY + ARRAY`
runtime error, even though the multi-line bodies lowered fine. The dispatch reads
`fnState`/`typeState` (the enclosing function/type context) but does not mutate
them, preserving the expander's self-contained property. The expanded chunk is
emitted via `trackRewriteOutputLines`, which maps every produced line back to the
**single source line** — so diagnostics keep their original Aether line numbers
(regression-tested). It is brace-balanced (net depth delta 0) and a strict no-op
for already-multi-line code: a differential over all 183 corpus candidates was
byte-identical before/after, and collapsing those candidates to one-liner form
(`Tools/aether_collapse_oneliners.py`) and recompiling reproduces each one's
multi-line output. It does **not** touch the shared Rea grammar.

This is why a pre-pass was the *wrong* shape (see §5-style reasoning): `lineNumber`
advances once per preprocessed line, so adding lines before the main loop would
desync the diagnostic line-map. Doing it *inside* the loop — one input line in,
many output lines out, all mapped to that input line — is line-map-correct by
construction, reusing the same machinery contract-annotation expansion already
relies on.

**The one-liner `type` gap is closed too** (verified on `2026-07-26-9`). It used
to be real: `type` was deliberately excluded from the expander — a declaration,
not a control-flow body — so a one-liner `type Point { x: Int; y: Int; }` never
registered its fields. That was a property of the line-oriented rewriter, and it
went away with it: the AST parser has no one-construct-per-line assumption to
opt a declaration out of. Both forms now compile and both register fields, so
`FIELD-002` still fires on an unknown field either way:

```aether
type Point { x: Int; y: Int; }               // compiles; p.x and p.y resolve
type Boxed { n: Int = 0; label: Text = ""; } // defaults on one line are fine too
```

Repair drills for the one-liner *guard* and one-liner *type* shapes can both be
dropped from the family overlays. Nothing in the LLM-facing guides told models to
declare types multi-line, so no guide change was owed — but this section said so
for longer than it was true, which is the failure mode §6 exists to prevent. A
wart list is only useful if entries leave it when they are fixed.

### 6.2 The bootstrap is the wart-factory

More generally: any surprising parse error that isn't obviously Aether-specific
is probably Rea grammar showing through. Reproduce it in a `.rea` file before
assuming it's an Aether bug.

---

## 7. Corpus: core + per-family overlays

The fine-tuning corpus and benchmark harness live in the **PBuild umbrella** (not
this standalone repo); see its `Tests/aether_specialization/README_corpus_structure.md`.

- **Core** (`corpus_candidates/`): model-agnostic verified positives. Everyone
  trains on these.
- **Per-family overlays** (`seed_repair_pairs.<family>.json`): remedial
  `broken → fixed` drills authored by probing *that family's actual `none`
  failures*. Different families fall back to different wrong priors, so a corpus
  tuned to one under-serves another. Empirically (early v1 29-task suite, fixed compiler):
  Qwen2.5-Coder 24 > Qwen3-4B 23 > Granite-8B 20 — the score falls with distance
  from the tuned family.

**Rule:** every `fixed_source` in an overlay must compile to its
`expected_stdout`. There is a verification step (run the fixed sources, diff
stdout) and it is non-negotiable — a broken "correct" example teaches the wrong
thing. When you add a language feature that obsoletes a wrong prior (e.g. dotted
paths), prefer deleting the prior at the language level over adding a drill that
teaches models to avoid it.

---

## 8. Anatomy of a builtin (worked example: `clamp`)

Adding a builtin **looks** like one edit and is actually **four**, plus a trap.
This tripped up the `clamp` work and will trip up the next person.

A builtin is recognized and executed through *separate* mechanisms. **All five
sites in the table below live in the `pscal-core` dependency**, not this repo; the
Aether-side surface alias and type/effect inference live in
`src/aether/ast_prepasses.c` / `ast_parser.c` and `src/aether/semantic.c`, so adding a model-facing
builtin usually spans two repos:

| Site | File | Purpose | Symptom if you forget it |
|------|------|---------|--------------------------|
| 1. Definition | `src/backend_ast/builtin.c` (`vmBuiltinClamp`) | the C implementation | link/declare errors |
| 2. Header decl | `src/backend_ast/builtin.h` | visibility | compile error |
| 3. VM dispatch table | `vmBuiltinDispatchTable[]` in `builtin.c` | **runtime** name→handler | builtin "not found" at execution |
| 4. Builtin registry | `populateBuiltinRegistry()` in `builtin.c` (next to `"Max"`/`"Min"`) | **compile-time** "is this a builtin?" | **`Runtime Error: Undefined global variable 'clamp'`** |
| 5. Return-type inference | `getBuiltinReturnType()` in `src/ast/ast.c` | typed-context inference | `TYPE_VOID`/`UNKNOWN_VAR_TYPE` in typed bindings |

**The trap.** The dispatch table (site 3) drives *execution*; the registry (site
4) drives *compile-time name resolution*. They are not the same list. A builtin
present only in the dispatch table still resolves as an **undefined global** at
the front end — the symptom is a *runtime* "Undefined global variable" because the
compiler emitted a global load instead of a builtin call. `clamp` was in the
dispatch table and the return-type list and *still* failed until it was also
registered in `populateBuiltinRegistry` (where `max`/`min` live as `"Max"`/`"Min"`
— note the capitalization; lookups canonicalize to lowercase, so surface `clamp`
matches a `"clamp"` or `"Clamp"` registration equally).

Checklist for the next builtin: **define → declare → dispatch → register →
infer-type → rebuild → test a typed binding** (`let n: Int = clamp(105,0,100);`,
not just a bare call, so you exercise the return-type path).

---

## 9. Map of the territory

Paths are tagged by repo: unmarked = **this repo** (`emkey1/aether`);
**(pscal-core)** / **(rea)** = fetched deps (under `build/_deps/` after a build);
**(umbrella)** = the PBuild monorepo harness.

| You want to… | Go to |
|---|---|
| Understand the *vision* / phases | `src/aether/DESIGN.md` |
| *Write* Aether (reference) | `docs/aether_for_llms_and_others.md` (and the medium and small guides); what is accepted: `docs/aether_spec.md` |
| Run the KPI benchmark | *(umbrella)* `tools/aether_doc_bench.py` (variants `full`/`medium`/`small`/`card`/`none`, `--python-baseline`; KPIs in §3.1) |
| Triage a `none` failure | *(umbrella)* `tools/none_fail_detail.py <eval>.json none` |
| See the surface↔backend alias lowering | `src/aether/ast_prepasses.c` (pre-passes) + `src/aether/ast_parser.c` |
| Find Aether-only behavior forks | grep `FRONTEND_KIND_AETHER` |
| Add/modify a builtin | *(pscal-core)* `src/backend_ast/builtin.{c,h}`, `src/ast/ast.c` (+ Aether surface in `src/aether/`) — see [§8](#8-anatomy-of-a-builtin-worked-example-clamp) |
| TOON / yyjson bindings | *(pscal-core)* `src/ext_builtins/yyjson/yyjson_builtins.c` |
| Corpus structure & overlays | *(umbrella)* `Tests/aether_specialization/README_corpus_structure.md` |
| Run the language regression suite | `ctest -R aether_tests` here, or *(umbrella)* `Tests/run_aether_tests.sh` |

---

## 10. The through-line

Aether is a thin, opinionated front end over the PSCAL backend whose every design
choice is answerable to one question: **did the measured KPI go up, and did the
silent-wrong share stay down?** Concretely: does G (the medium guide on the local
panel) hold or rise, does the G − P gap shrink, does S not rise, and does replay
show R = 0 (§3.1). When a small model fails a task, the first hypothesis is still
that *Aether* is wrong — too many spellings that mean different things, a
missing obvious builtin, a crash where degradation belonged, ceremony where a
dotted path belonged. Fix the language and every model gets better for free;
patch the corpus or the guide only when the model's prior, not the language, is
the thing at fault, and check the surface registry (§5.7) before calling a
spelling a wrong prior. The warts were debts owed to the rewrite-era bootstrap;
the AST parser paid most of them, and the lowering contract (§2.2) is how the
next desugaring avoids new ones.
