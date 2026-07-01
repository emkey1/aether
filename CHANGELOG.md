# Aether language changelog

Aether language versions use the date-stamp scheme `YYYY-MM-DD-N` (the same scheme
as the guides): the date of the last language-affecting change, with `N`
distinguishing multiple such changes on a single day. The value lives in
[`VERSION`](VERSION), is reported by `aether --version`, and is stamped into every
benchmark result for traceability.

Bump it with `tools/bump_version.py` **only** when a change alters what programs
compile or how they run — a syntax, semantic, or builtin change — and never on a
plain rebuild. Because the stamp is checked in, every node that builds a given
commit reports the same version, so a real mismatch between nodes means one is
genuinely behind. Each bump should add an entry below.

## 2026-07-01-6

**Every frontend diagnostic is now coded, and name/type tracking is scoped per
function.** Two hardening changes from the same review batch as -5:

- **Coded diagnostics everywhere.** The 44 remaining raw parser errors
  (`L<n>: ...` with no code, path, hint, or guide pointer) now emit the
  standard `path:line: [CODE] ...` format: syntax errors as `SYN-001`, tuple
  limitations as `TUP-001`, imports as `IMP-001`, field defaults as
  `FIELD-003`, inference as `TYPE-001`. The semantic scalar/TOON type-flow
  family (cross-assigns, helper argument mismatches, opaque-handle arithmetic,
  typed-binding rules) emits explicit `[TYPE-001]`/`[TOON-001]` codes at the
  site instead of relying on message-text inference, and the overbroad
  `" first argument"` -> TOON-001 inference pattern now requires a
  `call to 'toon_*'` context. `--diagnostics-json` code assertions added for
  the cross-assign fixtures.
- **Function-scoped binding tables.** The parser's inference bindings and the
  semantic scalar/TOON tables were program-global: one function's locals
  leaked into every later function (last-write-wins), mis-typing same-name
  locals and masking real misuse. Locals (and parameters) are now scoped to
  their function, with globals/imports/function signatures still visible
  program-wide and shadowed globals restored on function exit. Programs that
  accidentally relied on cross-function leakage now get the normal
  `[TYPE-001] cannot infer` / scope diagnostics; same-name locals of
  different types in different functions now each infer correctly, and
  previously-masked TOON handle misuse is reported. Regression fixtures:
  `scoped_bindings_pass` / `scoped_bindings_fail`.

## 2026-07-01-5

**The fx effect fence (FX-001) and @pure purity checks (ANN-001) now run on the
real AST instead of scanning physical source lines.** The parser registers the
facts it used to erase (fx blocks, @pure declarations, builtin-alias surface
spellings) in side registries, and semantic analysis walks the parsed tree.
Three verified line-scan defects are fixed:

- **Escape closed:** an effectful call split across lines (`println` on one
  line, `("hi");` on the next) compiled and ran with no fx anywhere; it is now
  rejected with the standard `[FX-001]` diagnostic at the call's true line.
- **False positive removed:** `fx` with its `{` on the following line parsed
  fine but drew a spurious `[FX-001]` from the text scan; it now compiles and
  runs.
- **Purity hole closed:** a `@pure` function containing an `fx { ... }` block
  compiled (only the calls inside were checked). Per the guide, @pure functions
  may not contain fx blocks at all: the block itself is now rejected with
  `[ANN-001] ... pure function 'f' contains an fx block.`, independent of its
  contents.

Diagnostics keep their codes, file:line format, and surface spellings (a call
written `println`/`sleep`/`task_spawn`/`ai_chat` is quoted that way even though
the AST carries the canonical builtin). Compiler-injected @pre/@post guard
bodies (writeln/halt) are exempt from the fence, as before. Also
`toon_parse_file`'s canonical lowering (`YyjsonReadFile`) is now classified
effectful in pscal-core, keeping file I/O behind the fence on the AST path.

Two more changes ride in this version:

- **The legacy text rewriter is retired.** `translate.c`/`translate.h`
  (~10k lines) are deleted; the AST parser is the only frontend. The
  `AETHER_PARSER` environment variable is no longer consulted and the
  `--dump-rewrite` CLI flag is gone. `docs/parser_roadmap.md` records the
  migration history.
- **Alias lowering no longer rewrites string literals or comments.** The
  per-line alias pass rewrote builtin names inside user strings, silently
  corrupting program output (`println("call sleep(5) now")` printed
  `call delay(5) now`). String literals (with `\"` escapes) and `//` comments
  are now copied verbatim; aliases outside strings lower unchanged.
  Regression: `tests/alias_string_literal_pass.aether`.
- **New examples compile lap.** `tests/run_examples.sh` compile-checks every
  shipped example (capability-gated), registered as the `aether_examples`
  CTest, so the examples tree cannot rot silently.

## 2026-07-01-4

**Record/type fields may now declare a constant default value:
`type Counter { value: Int = 7; }`.** Models reach for field defaults, and a
declared default previously hit the raw `L1: unexpected token in type body.`
parse error. This lands it as a coherent record-initialization improvement.

- **Constant field defaults (`field: Type = <const>`).** After a field's type,
  an `=` introduces a compile-time-constant default: `count: Int = 0`,
  `ratio: Real = 1.5`, `name: Text = ""`, `on: Bool = true`, `xs: Int[] = []`,
  and trivial constant expressions (`-3`, `2 + 3`). The default is applied at
  construction for `new T()` and for the unset fields of `new T { ... }`; an
  explicit construction-site value always overrides it. Parsed on the AST path
  (`ast_parser.c`); the frontend attaches the constant to the field's
  `AST_VAR_DECL`, and the shared engine's `emitDefaultFieldInitializers`
  (pscal-core `compiler.c`) applies it on top of the type-zero so string
  capacity and int→real widening match the record-literal path.
- **New coded diagnostic `FIELD-003`.** A non-constant default (references
  another field, `self`, or calls a function) or a populated array default is
  rejected with `[FIELD-003]` and a hint pointing at construction-time
  `new T { field: value }`, replacing the generic "unexpected token" error. A
  type-mismatched default (`value: Int = "x"`) is a `[TYPE-001]` type error.
- **Guides:** both guides now document constant field defaults (FIELD-003) and
  surface `new T { field: value }` as the recommended way to initialize with
  values (partial sets keep unset fields' defaults), while keeping the
  no-`__init__`/no-`fn new()` stance.

## 2026-07-01-3

**Three compiler errors that were still raw (no `code` in `--diagnostics-json`,
plain stderr) now join the coded set, so models and the repair loop can map each
to its guide section.** All fixes are on the default AST path (`ast_parser.c` /
the shared engine); the legacy `translate.c` rewriter (`AETHER_PARSER=rewriter`)
is untouched. A generative-testing pass 2 flagged these as the remaining uncoded
tier after the 2026-07-01 diagnostic work.

1. **Bare `ret;` in a non-`Void` function -> `FLOW-002`.** An empty `ret;` where a
   value is required used to emit a raw `L<n>: return requires a value.` with no
   code. It now routes through `reportAetherAstError` (kind `function`), emitting
   `[FLOW-002]`, a hint (`ret <expr>;`, or declare `-> Void`), and the guide-help
   pointer. `FLOW-002` is distinct from `FLOW-001` (the fallthrough rule) because
   the fix differs: give the existing return a value vs add a return.
2. **Non-call statement inside `par { ... }` -> `PAR-002`.** The par-arity rule
   ("only direct call statements are allowed inside par blocks") already routed
   through `reportAetherAstError` (kind `par`) but was uncoded because no `par`
   case existed in `aetherInferDiagnosticCode`. It now maps to `PAR-002`, distinct
   from `PAR-001` (the shared-record data-race crash, which is emitted separately).
3. **Backend `Unknown field` -> `FIELD-002` via a collector backstop.** rea's
   semantic pass already codes undefined-field access as `FIELD-002` for a
   resolvable receiver (the `state.state` case the pass flagged is caught here,
   already coded). But a receiver whose record type is *unresolvable* (e.g. a
   method call on a variable of an undefined type) slips past semantics to
   codegen, which emits a raw, uncoded `L<n>: Compiler error: Unknown field
   'T.m'.` The `--diagnostics-json` collector (`rea/src/rea/main.c`) now backfills
   the code for any bracket-less line via the registered frontend's inference, so
   that backend string (and any other uncoded-but-recognized message) carries its
   code in the JSON rather than `code: null`.

`aetherInferDiagnosticCode` (the single source of truth for the message-to-code
map) gains the `return requires a value` -> `FLOW-002` and `only direct call
statements` -> `PAR-002` entries. Both LLM guides gain `FLOW-002` and `PAR-002`
troubleshooting rows (and the enumerated code list in the long guide now includes
`PAR-001`/`PAR-002`, previously omitted). Locked in by `--diagnostics-json`
`"code"` assertions in `tests/run.sh` for all three (`function_empty_return_fail`,
`par_fail_non_call`, `backend_unknown_field_coded_fail`). No language capability
changed.

## 2026-07-01-2

**The tuple-destructuring diagnostic now carries the real `TUP-001` code (was the
placeholder `feature`) and, on the default AST path, an actionable hint.** A
generative-testing pass caught models reaching for multi-value destructuring
(`let (name, age, score) = parseLine(line);`) against a callee that is not a
defined top-level tuple-return function. That correctly fails, but the diagnostic
reported `code: "feature"` in `--diagnostics-json` and emitted no hint, so it did
not feed the code-to-guide-section map that the compiler and guide self-correction
loop rely on.

1. **Every tuple feature-limitation diagnostic now resolves to `TUP-001`:**
   destructure target is not a known tuple-return function, arity mismatch,
   "requires a direct call", binding a tuple-return call to a single name, and
   tuple-return methods. The former placeholder kind `feature` is now the semantic
   kind `tuple`, mapped to `TUP-001` in `aetherInferDiagnosticCode` (the single
   source of truth), so both the default AST path (`ast_parser.c`) and the legacy
   rewriter (`AETHER_PARSER=rewriter`, `translate.c`) agree.
2. **The AST-path destructuring diagnostics gained a hint.** The three were raw
   `aetherDiagf` calls with a hardcoded `[feature]` bracket and no hint; they now
   route through `reportAetherAstError`, emitting `TUP-001`, a hint (previously
   `null` in `--diagnostics-json`), and the guide-help pointer, byte-for-byte
   identical to the rewriter path.
3. **No language capability changed.** Destructuring a direct call to a defined
   top-level tuple-return function (`let (a, b) = pair();` where `pair -> (Int,
   Int)`) still compiles. Both LLM guides now document the alternative for the
   cases that legitimately cannot destructure (a method, an undefined helper, or a
   nested expression): return a record/object and read its fields.

## 2026-07-01-1

**Three silent-failure gaps in the AST frontend now produce coded diagnostics
instead of an empty exit 1 or a crash.** The common thread: a program that failed
with a nonzero exit code but *no message at all* (empty stderr, empty
`--diagnostics-json`) — or that crashed the VM — is the worst case for both humans
and the LLM repair loop, because there is nothing to react to. All three are on the
default AST path (`ast_parser.c`).

1. **Silent parse failures → `[SYN-001]` backstop.** A parse error that set the
   parser's error flag but reported nothing (a `NULL` propagating up a chain that
   only set `p->hadError`) used to exit 1 with empty output. Every parser
   diagnostic now funnels through one counting sink (`aetherDiagf`); after the
   authoritative parse, if it bailed with `hadError` but emitted nothing, a coded
   `[SYN-001]` is emitted, anchored at the token where parsing stalled. Example: a
   non-keyword top-level construct such as `rec Point { ... }` (a common slip for
   `type`) now reports `unexpected token ':'` instead of nothing.

2. **Undefined method on a record → `[SCOPE-001]` at compile time.** The parser
   lowers `recv.method()` on a user record to a call to the mangled global
   `Type.method`; with no such method it silently degraded to an undefined-global
   read that failed only at runtime (`Undefined global variable 'Point.distance'`).
   A post-parse walk (`aetherCheckMemberCalls`) verifies each `Type.method` call
   resolves to a defined method, mirroring the field-read check that already emits
   `FIELD-002`. Conservative by construction (only a real user record type with no
   parent, and only when the name is neither a method nor a field), so it does not
   reject valid corpus programs.

   ```
   let p: Point = Point { x: 1, y: 2 };
   fx { println(p.distance()); }   // before: runtime "Undefined global variable 'Point.distance'"
                                   // now:    [SCOPE-001] method 'distance' is not defined on type 'Point'
   ```

   (Code note: `SCOPE-001`, not `METH-001` — the latter already denotes the
   "methods do not capture outer locals" rule in the guide; the hint cross-references
   it.)

3. **`par` branches sharing a record → `[PAR-001]` at compile time.** Handing the
   *same* pointer-backed record to more than one `par` branch makes the spawned
   threads write it concurrently, a heap double-free that aborts the VM
   (SIGABRT/SIGTRAP) with no diagnostic. `parseParBlock` now rejects the aliased
   record at compile time; the safe idiom (each branch its own record, combined
   after the block) is unaffected. Only user aggregate args are flagged — scalars
   are value-copied per thread and are never shared.

   ```
   par {
       setname(a, "one");   // before: intermittent SIGABRT (concurrent double-free)
       setname(a, "two");   // now:    [PAR-001] record 'a' is shared by more than one par branch
   }
   ```

Regressions: `tests/method_undefined_fail.aether`, `tests/par_shared_record_fail.aether`,
`tests/unknown_construct_fail.aether` (full `tests/run.sh` green under the AST frontend).

## 2026-06-30-4

**A `@pre`/`@post` contract predicate that compares a whole collection to a
scalar is now rejected at compile time (`ANN-001`) instead of crashing the VM at
runtime.** A contract like `@post result > 0` on a `T[]`-returning function lowers
to a runtime guard `if (!(result > 0)) ...`; because pscal-core cannot compare an
array to an integer, that guard aborted with
`Runtime Error: Operands not comparable for operator '>'. Left operand: ARRAY,
Right operand: Int` — a runtime failure for what is really a type error in the
annotation:

```
@post result > 0
fn make(n: Int) -> Int[] { ... ret xs; }   // before: compiled, crashed at runtime
                                            // now:    ANN-001 at compile time
```

The AST frontend now type-checks each contract comparison as it lowers the guard
(`checkContractComparisons` in `ast_parser.c`): a bare `result` in a `@post`
resolves to the function's return-type name, every other operand goes through the
existing binding/return-type inference, and a comparison with exactly one array
operand and one scalar operand is reported with a coded diagnostic that points at
the fix:

```
prog.aether:7: [ANN-001] Aether contract rewrite error: @post predicate compares
an array (`Int[]`) to a scalar (`Int`) with `>`; arrays and scalars are not comparable.
hint: use a collection predicate, for example `length(result) > 0`.
```

The check is deliberately conservative to avoid false positives on the benchmark
corpus: it fires only for the unambiguous array-vs-scalar case (`< > <= >= == !=`)
and leaves array-vs-array and any operand whose type cannot be named untouched.
The documented fix — a collection predicate such as `length(result) > 0` — and all
scalar contracts (`@post result > value` on an `Int` return) compile unchanged.
The same check covers `@pre` predicates over array parameters. Reported by
`qwen3-coder-next`, which hit the runtime crash from a Sieve of Eratosthenes whose
`@post` compared the returned array to `0`.

## 2026-06-30-3

**A `-> Void` method now compiles on an object whose type was *inferred* from
`new`, not only when the instance is explicitly annotated.** Records are
pointer-backed, so a statement-level method call passes the receiver as the
method's `POINTER` `self` argument. The inferred-`let` path built the declared
type node by hand and, for any user type, emitted a bare `AST_TYPE_REFERENCE` at
`TYPE_UNKNOWN` — it never consulted `lookupType` to detect a record and wrap it in
`AST_POINTER_TYPE`. So the variable stayed untyped and a later statement-level
Void-method call failed:

```
type C { value: Int; fn inc() -> Void { self.value = self.value + 1; } }
let c = new C();   // inferred -> argument 1 to 'c.inc' expects type POINTER but got VOID
let c: C = new C(); // explicit -> OK
c.inc();
```

The inferred binding now routes through the same `buildTypeNode` helper the
explicit `: T` annotation uses, which resolves a record/class name to
`AST_POINTER_TYPE -> AST_TYPE_REFERENCE(RECORD)` (var_type `POINTER`). The inferred
and annotated forms now produce a byte-identical declaration, so `let c = new C();`
and `let c: C = new C();` behave the same. Builtin and un-inferable types are
unchanged (the "cannot infer" diagnostic still fires). The same fix covers an
inline object-literal method chain that returns a record
(`let x = Foo { ... }.makeBar();`). Reported by `mistral-small-24b`, which could
not recover from the raw `POINTER`/`VOID` message.

## 2026-06-30-2

**Trailing `//` comments on `@pre`/`@post` annotations no longer leak into the
contract guard.** The AST frontend captured the whole physical remainder of an
annotation line as the contract expression, so a trailing line comment was
lowered into the runtime guard and parsed as code. A comment that happened to
contain an undeclared word failed compilation, e.g.

```
@post result >= 1 // Factorial of 0 is 1, otherwise positive
   -> [SCOPE-001] identifier 'Factorial' not in scope.
```

whereas a comment whose words all resolved compiled silently — the diagnostic
depended on the *prose*, not the code. `collectPendingAnnotations` now stops the
expression at the first unquoted `//`, so comment text is stripped unconditionally
before lowering; a `//` inside a string literal in the expression (e.g.
`@post result != "http://none"`) is preserved. The guard itself is unchanged, so
a contract that should fire still fires. The legacy text-rewriter fallback
(`AETHER_PARSER=rewriter`, `extractAnnotationExpr`) got the symmetric fix, so both
frontends agree. **Not breaking:** programs that compiled
before still compile; programs that erred only because of an annotation comment
now compile. Companion to the still-open "malformed `@cost` is silently dropped"
gap. (Surfaced by generative-model testing; `gemini-3.1-pro-preview`.)

## 2026-06-30-1

**Reserved-word / member-name collisions now name the collision (SYN-001).** A
`type` field or a `fn` method named after a reserved word previously failed with
a generic parse error that never identified the cause: a field colliding with a
type-name keyword (`word`, `text`, `int`, `byte`, `bool`, ...) reported
`unexpected token in type body`, and a method colliding with an operator word
(`mul`/`div`/`mod`/`xor`) or a value/structure keyword (`new`, `for`, ...)
reported `expected function name after 'fn'`. This was the single broadest gap
generative-model testing surfaced (several distinct LLMs hit it) — neither the
model nor a human could act on the message. The AST frontend now emits a SYN-001
diagnostic that names the offending word and its class, e.g. `'word' is a
reserved type name and cannot be used as a field name` and `'mul' is a reserved
operator word and cannot be used as a method name`, each with a rename hint;
`fn new()` additionally points at the (absent) constructor idiom (`new T()` +
field assignment, or a top-level factory `fn`). **Not breaking:** these programs
were already rejected — only the diagnostic improved, on the default (AST) path;
the legacy rewriter path is unchanged. Both LLM guides gained a "reserved words
to avoid as member names" note.

## 2026-06-27-3

**Effect model now covers host interaction (FX-001 / `@pure` soundness).** The
effectful-builtin set that FX-001 enforces (and that `@pure` rejects) previously
covered only output, `read`/`readln`, `ai_chat`, tasks/threads, and `sleep`.
Filesystem (`mkdir`/`rmdir`/`fileexists`/file-handle I/O), environment
(`getenv`), CLI (`paramstr`/`paramcount`), `random`/`randomize`, the external
clock (`gettime`/`getdate`/`realtimeclock`), console input (`readkey`/
`keypressed`), networking (`http*`/`socket*`/`dnslookup`), database (`sqlite*`),
and the `task_wait` family all escaped both checks — so a `@pure` function could
read the environment or call `random` and still compile. These are now
effectful: they require an `fx { ... }` block and are rejected inside `@pure`
functions. Pure builtins (math, string, conversion, in-memory streams,
`toon_parse` on a string) are unaffected. The name match is now
case-insensitive, and the rule is enforced identically on the AST and rewriter
paths. **Breaking:** code that called these outside `fx` (or inside `@pure`)
must move the call into an `fx` block. SDL/graphics and terminal screen-control
builtins are intentionally left ungated.

## 2026-06-27-2

**AST frontend is now the default (P7 cutover).** `parseAether()` parses straight
to the shared AST by default; the legacy text rewriter is retained as a runtime-
reversible fallback via `AETHER_PARSER=rewriter` (or `=legacy`), and
`AETHER_PARSER=ast` remains an explicit synonym for the default. Gate met:
RW-vs-AST parity 1819/1851 on the model corpus with **zero AST-worse** (the 32
divergences are AST-better or out-differs, where the AST path is the more correct
one -- correct error line numbers, no compound-line mis-parses). Three negative-case
diagnostics were aligned to the rewriter's wording so the curated suite stays green
under the new default: the declaration `cannot infer the type of '<x>'` error
(mixed string/number initializers like `tag + 1` now bail instead of mis-inferring
Int), the `type fields must end with ';', not ','.` type error, and the
`non-Void functions have a fallthrough path with no return value.` FLOW-001 check.

## 2026-06-27-1

**Digit separators in numeric literals.** Underscores may now appear between
digits in integer, hexadecimal, and floating-point literals (`1_000_000`,
`0xFF_FF`, `3.141_592`); they are cosmetic and stripped before the value is
parsed. A `_` is only part of the number when it sits between two digits, so a
leading/trailing `_` still begins/ends a separate token. Both frontends (the
legacy rewriter and the AST parser) accept them, via the shared rea lexer.

## 2026-06-26-1

**Compound-line parsing** (frontend rewriter): multiple constructs sharing a
single physical line now parse correctly. Previously a `type` field sharing a
line with the next method (`budgeted: Real;fn ingest(...)`) silently dropped the
method, orphaning its body, and a function body inlined on its signature line
(`fn main() { let x: T = ... }`) was left untranslated — both surfaced as
misleading `SYN-001` errors (`Unexpected token ELSE` / `COLON`) at the wrong
line. The Aether-to-Rea rewriter now normalizes run-on `type` members and inline
`fn`/`type` bodies onto their own lines before lowering. Value braces
(`new T {}`), if-expressions (`if c { a } else { b }`), and control one-liners
are left untouched, so already-canonical code is unaffected.

## 2026-06-23-1

First versioned release; baseline of the current language. Notable recent
language-affecting fixes already folded into this baseline:

- **Method-to-method receiver fix** (pscal-core): a method that calls another
  method on `self` while passing an argument no longer passes the receiver onto
  the first parameter. Restored quicksort and other method-heavy programs.
- **2D chained array indexing** (`dp[i][j]`) now returns the element rather than
  the inner row.
- **Integer-recursion argument coercion** (`n % 2`, `n / 2`): integer arguments
  to recursive calls no longer coerce through Real, fixing collatz-style programs.
