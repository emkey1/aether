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
