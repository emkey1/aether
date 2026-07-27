# Aether Ideas & TODO

A living backlog for the Aether language and toolchain: feature ideas, known
limitations, and design decisions (with rationale). When a new idea, gap, or
limitation surfaces — especially from model reviews or codegen testing — record
it here with a status rather than letting it evaporate into a chat log.

This is the *forward-looking* companion to:
- `parser_roadmap.md` — the AST-frontend cutover (owns the rewriter bug class:
  compound-line mis-parses, unreliable error line numbers, etc.).
- `CHANGELOG.md` — what already shipped.
- `aether_for_llms_and_others.md` / `..._with_small_contexts.md` — the language
  as it is today.

Status legend: **idea** (not decided) · **gap** (confirmed limitation) ·
**in-flight** (being worked) · **decided** (resolved; rationale recorded).

---

## Open ideas

### Tuples have no `.0`/`.1`/`.2` field access, only destructuring — *fixed 2026-07-19-10*
`let t = f(); let a: Text = t.0;` used to fail with `[SYN-001] unexpected
token in block; expected a statement` -- the rea lexer folds a `.` followed
by a digit into a single NUMBER token (`t.0` lexes as IDENT + NUMBER(".0"),
never DOT + NUMBER), so `parsePostfix` never even saw a `.` to react to; the
only working way to pull values out of a tuple was destructuring at the
binding site (`let (a, b, c) = f();`). Fixed by recognizing that folded
`.<digits>` token shape directly in `parsePostfix` and lowering it to the
same `AST_FIELD_ACCESS(recv, "item<N>")` shape `let (a, b) = f();`
destructuring already builds, with a compile-time (not runtime) bounds
check: `t.2` on a 2-element tuple is a TUP-001 "tuple index .2 is out of
range (tuple has 2 elements)" parser error, not a crash. This also required
loosening the old blanket rejection of `let v = pair();` (binding a
tuple-return call to one name) -- that restriction predated `.N` access and
existed only because a bound-but-undestructured tuple was otherwise useless;
`v` is now an ordinary variable of the synthesized tuple record type, same
as any other `let`.

Chaining an index directly onto a call result (`pair().0`, no intermediate
variable) is still explicitly rejected, with a message pointing at the
`let t = pair(); t.0;` workaround -- not because the parser can't build the
AST (it can, and rea's shared semantic analyzer can even resolve the
receiver's class), but because pscal-core's codegen (`getRecordTypeFromExpr`
in compiler.c) has no path from a call expression to its record type, and
extending that touches how *all* record-returning calls carry their return
type through codegen, not just tuples -- out of scope for this pass. Landing
that would remove this fixture's restriction:
`tests/tuple_index_chained_call_unsupported_fail.aether`.

### Bitwise operations don't exist at all, despite `xor` being reserved as if they did — *fixed 2026-07-19-9*
Aether has no way to perform bitwise AND/OR/XOR/shift. Confirmed absent as
symbol operators (`<<`, `>>`, `|`, `^` all fail with `[SYN-001] unexpected
token in block; expected a statement`), as Pascal-style word operators
(`shl`, `shr`, `and`, `or`, `xor` all fail with `[SCOPE-001] identifier
'...' not in scope` -- i.e. not even recognized as keywords), and as
builtin functions (`builtin_info` returns `null` for every plausible name:
`bitand`, `bitor`, `bitxor`, `bitshift`, etc. -- confirmed via a full dump
of `builtins_json(true)`, no bit-* or shl/shr-* entries exist anywhere).
Yet `xor` is explicitly reserved as an operator word models cannot use as a
field/method name (FIELD-002-adjacent rule, grouped with `mul`/`div`/`mod`
in both guides), which strongly implies bitwise xor was *meant* to exist as
a word operator like `div`/`mod` do, but was never wired up -- the
reservation is real, the functionality behind it isn't. Found because a
generated batch included an XOR cipher and a custom bit-mixing hash
function, both fundamentally impossible to write in Aether as a result;
both were dropped from that batch rather than force-fitting a
non-equivalent arithmetic substitute. Not fixed this round -- worth a
decision on whether to add real bitwise support (the underlying PSCAL VM
likely already has bitwise opcodes/builtins for its own use; the gap may
just be in what Aether's frontend exposes) or to soften the guide's
reservation of `xor` if it's never going to back anything.

**Fixed 2026-07-19-9.** Investigated per the "worth a decision" note above:
rea's own grammar (`external/rea/src/rea/lexer.c`/`parser.c`) already
tokenizes and parses all of this -- `REA_TOKEN_SHIFT_LEFT`/`SHIFT_RIGHT` for
`<<`/`>>`, `REA_TOKEN_AND`/`OR`/`XOR` for `&`/`|`/`^` (and `xor` lexes to the
*same* `REA_TOKEN_XOR` as `^`, which is exactly why the word was already
reserved) -- and the underlying PSCAL VM (`external/pscal-core/src/vm/vm.c`)
already has real opcodes behind every one of them: `case SHL: case SHR:`
and `case AND: case OR: case XOR:`, the latter confirmed to do double duty
as bitwise-on-`Int` and eager (non-short-circuit) boolean-on-`Bool`,
dispatched by the compiler (`compiler.c`'s `AST_BINARY_OP` case) off the
node's annotated result type. Aether's own frontend (`ast_parser.c`) just
never had a grammar rule that reached any of it -- purely a "wire up
existing plumbing" gap, the same class as the recent File-type work.

Fix: added `<<`/`>>`/`&`/`|`/`^` (and word `xor`) to Aether's own precedence
ladder in `ast_parser.c`, at the same points rea's own parser uses them --
`parseShift` between `parseAdd` and `parseComparison`; `parseBitwiseAnd`/
`Xor`/`Or` between `parseEquality` and `parseLogicalAnd` -- each emitting
`AST_BINARY_OP` with the matching `TOKEN_SHL`/`SHR`/`AND`/`OR`/`XOR` token to
reach the opcodes above, with type-promotion logic mirrored verbatim from
rea's parser (matching the "verbatim from rea parser.c" convention already
used for `+`/`-`/`*`/`/`/`%`). Deliberately did *not* add word forms `and`/
`or`: `&&`/`||` already own logical conjunction/disjunction throughout the
~500-example corpus, and a second, non-short-circuiting word spelling of the
same thing risks exactly the confusion this entry's own "worth a decision"
note warned against; `&`/`|`/`^` are symbol-only, standard C/Rust-style
bitwise operators that happen to also do eager (not short-circuit) boolean
combination on `Bool` operands, distinct in both spelling and evaluation
order from `&&`/`||`. `xor` keeps working as a word too, at no extra cost --
rea's lexer already produces the identical token for `xor` and `^`, so this
resolves the "reserved but backs nothing" half of the original gap for
free. Also deliberately did *not* add a parse-time Int-only type check (an
earlier draft of this fix did, and it broke on the very first non-literal
test): a plain identifier's `var_type` is still `TYPE_UNKNOWN` at the point
`ast_parser.c` parses a binary expression -- real type resolution happens in
a later pass, before codegen -- so a parse-time check rejected ordinary
variable operands like `x & y` outright. The existing VM-level type guards
(the `AND`/`OR`/`XOR` and `SHL`/`SHR` opcode handlers in `vm.c`) are the
real safety net, exactly as for every other Aether binary operator, none of
which validate operand types at parse time either.

Verified: `6 << 1` = `12`, `6 >> 1` = `3`, `6 & 3` = `2`, `6 | 1` = `7`,
`6 ^ 3` = `6 xor 3` = `5`; `&&`/`||` short-circuit logic and ordinary
arithmetic (`+ - * / %`) are unaffected; the full precedence ladder
(`+ - `, then `<< >>`, then comparison, then `== !=`, then `&`, then `^`,
then `|`, then `&&`, then `||`, tightest to loosest) composes correctly --
including a right-hand-side-of-`&&` expression parsing all the way through
the new bitwise layers, a regression the first draft of this fix introduced
and testing caught before landing (the `&&` loop's right-operand recursion
target wasn't updated when its base case was, silently truncating parses
like `a && x & y`). The full existing suite (`tests/run.sh`, ~2000
fixtures) passes unchanged. New regression fixtures
`tests/bitwise_shift_ops_pass.aether` (each operator plus precedence
composition) and `tests/bitwise_shift_type_mismatch_fail.aether` (confirms
the VM-level type guard still fires on real misuse), both wired into
`run.sh`. Both LLM guides updated with the new operator row/line and a
precedence note. Language version bumped to `2026-07-19-9`.

### A method call chained onto another method call's result falls back to name-only builtin matching for FX-001 — *fixed 2026-07-19-7*
`receiver.someMethod()` correctly resolves against the receiver's static
type and doesn't false-positive against a same-named builtin. But
`receiver.otherMethod().someMethod()` -- chaining a second method call onto
the *result* of a first one -- loses that type resolution and falls back to
matching `someMethod`'s name against the builtin table, so a user-defined
method that happens to share a name with an effectful builtin (`mkdir` is
the confirmed case) incorrectly trips `[FX-001] call to 'mkdir' requires an
fx block`, even though a human reader has no ambiguity about which `mkdir`
is meant. Minimal repro:

```aether
type Foo {
    fn self_ref() -> Foo { ret self; }
    fn mkdir(name: Text) -> Bool { ret true; }
}
fn main() -> Void {
    let f: Foo = new Foo {};
    f.mkdir("test");           // fine
    f.self_ref().mkdir("test"); // [FX-001] false positive
}
```

This is a different bug from "`swap` collides with the `swap` PSCAL
vm_builtin" above (that one was about a bare top-level function call
falling back to builtin lookup, fixed 2026-07-19-3) -- this is specifically
about *method* calls losing receiver-type information across a chain, a
resolution-path gap the earlier fix didn't cover. Found via a generated
"virtual filesystem" example chaining `root.find_child(name).mkdir(...)`.
Worked around there by binding the intermediate result to a local variable
first (`let parent: FSNode = root.find_child(name); parent.mkdir(...);`).

**Resolved 2026-07-19-7.** Root cause: `parsePostfix` (`ast_parser.c`)
mangles a method call's name to `Type.method` (so the effect checker judges
it against the receiver's real declaration, not a same-named builtin) by
inspecting `node`, the receiver expression it just finished parsing. Before
this fix it only knew how to pull a class name out of three receiver shapes
-- `myself`/`my`, `new Type {...}`, and a plain variable resolved through
the binding table -- so `f.mkdir(...)` mangled correctly (`node` is the
`AST_VARIABLE` for `f`) but `f.self_ref().mkdir(...)` did not: `node` there
is the `AST_PROCEDURE_CALL` for `f.self_ref()`, which matched none of the
three cases, so `cls` stayed `NULL`, the call stayed unmangled `mkdir`, and
`aetherCheckCallNode` (`semantic.c`) fell through to a bare-name match
against the effectful-builtin table.

Fix: added a fourth case to that same `if`/`else if` chain -- when `node` is
itself an `AST_PROCEDURE_CALL`, look its (possibly-already-mangled) call
name up in `p->funcReturns`, the binding table `parseFnDecl` already
populates with every fn/method's declared Aether return type (keyed under
the same mangled `Type.method` name used at the call site, `ast_parser.c`
~line 5717). Because the whole file is forward-scanned into `funcReturns`
before the real parse pass runs (`aetherRunForwardScan`), this lookup
succeeds regardless of declaration order. The result becomes `cls` for
*this* call, so the same code path that already mangles `f.mkdir(...)` now
also mangles the second link in the chain -- and because `parsePostfix`
processes a chain left to right, reusing this same branch on each iteration,
resolution recurses naturally through any chain depth: `f.self_ref().mkdir()`
resolves via one recursion, `f.self_ref().self_ref().mkdir()` via two, etc.,
with no depth-specific code needed.

Verified: the exact repro above (`f.self_ref().mkdir("test")`) now compiles
and runs, printing `true`; a 3-deep chain
(`f.self_ref().self_ref().mkdir("test")`) also resolves correctly; and a
genuine unshadowed builtin call (bare `mkdir("somepath")`, no user type
involved, outside `fx`) still correctly faults `[FX-001] call to 'mkdir'
requires an fx block` -- confirming the fix is additive resolution, not
blanket permissiveness. Full existing suite (`tests/run.sh`) passes
unchanged. New regression fixture
`tests/chained_method_call_shadow_builtin_pass.aether` (direct call plus
2-deep and 3-deep chains, all printing `true`), wired into `run.sh`.
Language version bumped to `2026-07-19-7`.

### `while` and `for` are fully accepted, working synonyms for `loop`, despite the guide banning both outright — *resolved (docs-only), 2026-07-19*
Both guides state, prominently and repeatedly, that `while`/`for` must
never be generated: Highest-Value Rule #2 ("Use Aether keywords: `fn`,
`let`, `const`, `ret`, `if`, `loop`, `type`, `mod`, `use`. Do not import
syntax from Python, Rust, JavaScript, Go, or Pascal.") and the *Never
Generate These* list ("`for`, `while`, `var`, `func`, `def`, `=>`,
Python-style colons (SYN-001)") both frame this as a hard rejection.
Empirically, both compile and run identically to `loop`:

```aether
fn main() -> Void {
    let i: Int = 0;
    while i < 5 {
        fx { println("i=", i); }
        i = i + 1;
    }
    ret;
}
```

prints `i=0` through `i=4` and exits 0 — no diagnostic, no difference from
the `loop i < 5 { ... }` form. Same for `for i in 0..5 { ... }` against
`loop i in 0..5 { ... }`. Found because a generated batch used `while` for
three of its twelve programs and every one compiled and ran fine on the
first try, unlike every other guide-violation this session has produced
(which reliably error).

This is likely Pascal/PSCAL heritage the guide's ban was never actually
wired into the parser to enforce -- the rule appears to be aspirational
style guidance dressed as a hard compile-time restriction. Two ways to
close the gap, in order of preference: (1) if `while`/`for` should really
be rejected, add the enforcement (cheap: reject these two keywords at the
statement-parse level with a SYN-001 pointing at `loop`); (2) if lenient
parsing is intentional/desired for robustness, downgrade the guide's
language from a hard "never generate, do not import" ban to the existing
**Accepted** tier used elsewhere (`itoa` vs `int_to_text`, etc.) --
"accepted, but `loop` is canonical" -- so the guide stops overclaiming
strictness it doesn't have. Not fixed either way this round; corpus
examples using `while`/`for` were rewritten to `loop` before import
regardless, since the raw-corpus exporter's own `NON_CANONICAL_PATTERNS`
filter already treats both as non-canonical and would silently drop them.

**Resolved (docs-only) 2026-07-19.** Investigated path (1) first, since it
was the stated preference. `ast_parser.c`'s statement dispatcher routes
`while`/`for` to their own `parseWhileLoop`/`parseForLoop` functions (not a
byproduct of `loop` parsing), each carrying an explicit comment describing
itself as "a spelling of" the corresponding `loop` form that "the rewriter
lowers ... identically" -- this reads as a deliberate design choice (lenient
Pascal-heritage acceptance), not an accidental gap, and mechanically
rejecting it would be cheap on its own. But `tests/for_range_pass.aether` --
an existing, wired-in **PASS** fixture (`tests/run.sh`, `FOR_RANGE_PASS_FIXTURE`)
-- asserts `for i in 0..5 { ... }` compiles and produces the correct summed
output. Enforcement would turn this already-covered, intentionally-tested
behavior into a regression, which is exactly the "real conflict" case the
task called out rather than something to silently work around. That
resolved the choice: took path (2). Softened both guides from a hard
"never generate" ban to the existing **Accepted** tier (mirroring the
`itoa`/`int_to_text` pattern): `while`/`for` still work and are documented
as such (new row in the quick-reference table; `Never Generate These` /
Rule #2 / the small-context guide's SYN-001 rule all reworded), but `loop`
remains canonical and is called out as the thing to generate in new code.
Also fixed two now-inaccurate troubleshooting entries in both guides'
`[SYN-001]` diagnostic-code writeups that told readers a `for`/`while`
program would be rejected with a `loop not for/while` fix -- it isn't, so
that line was actively misleading and is removed. No VERSION bump (docs
only, no compiler behavior changed; consistent with the precedent set by
the `itoa`/`int_to_text` docs-only fix and the `socket*` docs-only entry
above, neither of which bumped the guide version tag either).

### `s[i]`'s single-character result isn't accepted everywhere a `Text` is — *fixed 2026-07-19-8*
(Historical: `Text` was 1-based when this was written; it is 0-based as of
2026-07-25, so `s[0]` is now the first character. The bug and fix below are
unaffected -- they are about the *type* of `s[i]`'s result, not its index.)

`Text` supports 1-based bracket indexing (`s[1]` is the first character,
matching `copy`'s convention) and the result concatenates fine with `+`
(`reversed = reversed + s[i];` works), but was rejected by at least
`parse_int`: `parse_int(s[i])` failed at runtime with `parse_int argument
must be a string`, while `parse_int(copy(s, i, 1))` (same character,
extracted via `copy` instead of `[]`) worked.

**Root cause:** `s[i]` on a `Text` (VM type `TYPE_UNICODE_STRING`) lowers to
the `GET_CHAR_FROM_STRING` opcode (`external/pscal-core/src/vm/vm.c`, case
`GET_CHAR_FROM_STRING`), which decodes the UTF-8 codepoint at that index and
pushes it via `makeWideChar()` -- producing a `TYPE_WIDECHAR` value. This is
a *third* scalar type, distinct from both `TYPE_STRING`/`TYPE_UNICODE_STRING`
(what `copy()`/`split()` return) and `TYPE_CHAR` (what indexing a narrow
single-byte `TYPE_STRING` literal produces, or what a `'x'`-style single-char
literal is). `parse_int`/`parse_float`/`parse_bool`/`split`
(`external/pscal-core/src/ext_builtins/strings/parse.c`) shared a local
`string_arg()` helper that special-cased `TYPE_STRING`, `TYPE_UNICODE_STRING`,
and `TYPE_CHAR`, but had no branch for `TYPE_WIDECHAR` -- so it fell through
to `NULL` and every one of those builtins rejected an `s[i]` result. `trim`
had the identical gap via a separate helper pair
(`builtinValueIsStringLike`/`builtinValueToCString` in
`external/pscal-core/src/backend_ast/builtin.c`) that never learned about
either char type. Meanwhile `copy`, `pos`, `upcase`, and `length`/`string_len`
each handle `TYPE_CHAR`/`TYPE_WIDECHAR` inline (encoding the widechar back to
UTF-8 via `encodeUtf8Codepoint()`), which is why those already worked.

**Fix:** widened `string_arg()` in `parse.c` to add a `TYPE_WIDECHAR` branch
(encoding the codepoint into a 5-byte scratch buffer, matching the pattern
already used by `copy`/`pos`), which fixes `parse_int`, `parse_float`,
`parse_bool`, and `split` in one place since they all share the helper.
Widened `trim` (`builtin.c`) the same way, inline, rather than touching the
shared `builtinValueIsStringLike`/`builtinValueToCString` pair -- that pair
has 6 call sites and no scratch-buffer parameter, so generalizing it would be
a larger API change than this bug warranted. Did not touch `atoi`, which has
the same narrow check but wasn't part of this pass's `Text`-builtin survey
and is a distinct/legacy entry point from `parse_int`.

**Verified:** `pos`, `string_len`/`length`, and `upcase` were spot-checked
against an `s[i]` argument and already worked (no change needed). Added
`tests/text_index_char_builtins_pass.aether` (wired into `tests/run.sh`),
covering `s[i]` passed into `parse_int`/`parse_float`/`parse_bool`/`trim`,
plus a multi-byte UTF-8 codepoint index and a whitespace char trimmed to
empty. Confirmed the fixture fails with the pre-fix code (reverted the
`external/pscal-core` changes, rebuilt, reproduced the `parse_float argument
must be a string` failure) and passes after restoring the fix. Full
`tests/run.sh` suite passes.

### A top-level function named `<TypeName>_word` is unconditionally rejected, even when correctly declared — *fixed 2026-07-19-5*
Any unqualified call to a function whose name is `prefix_rest` was rejected
outright with `Legacy method call 'prefix_rest' is no longer supported; use
instance.rest() instead` if `prefix` case-insensitively matched ANY declared
type/class name in scope — regardless of whether `prefix_rest` was a
perfectly valid, normally-resolvable top-level `fn`. This was a real trap
for an extremely common naming convention (prefixing a free function with an
abbreviation of the type it operates on -- `db_open`/`db_put`/`db_get`,
`list_append`, `queue_push`, etc.), which is exactly the shape a generated
"Simple Database" example reached for and failed on. Confirmed with a
minimal repro: `type DB { data: Int; }` plus a plain `fn db_open() -> Int {
ret 42; }` called as `db_open()` failed to compile, with `type Db`
(different casing) failing identically -- `lookupClass` matches
case-insensitively.

**Root cause** (`external/rea/src/rea/semantic.c:3932`, in the Rea semantic
analyzer): the check did `strchr(name, '_')`, took everything before the
first underscore, and called `lookupClass()` on it; if that matched, it
emitted the hard error unconditionally, *without first checking whether the
call already resolved to a real declared function*. The diagnostic text
("no longer supported") implies this used to be real sugar for
`instance.method()` dispatch that had since been removed, with this check
left behind as a migration hint -- but it fired as a blanket veto ahead of
normal call resolution rather than as a fallback for names that don't
otherwise resolve, so it also rejected code that was never using the
removed feature at all. A few lines below it, a separate `!callDecl`-gated
check (same function) got this right: `callDecl` is resolved once, earlier
in the same `AST_PROCEDURE_CALL` handling block (`findStaticDeclarationInAST`
/ `findGlobalFunctionDecl` / `findFunctionInSubtree`, in that order), as the
authoritative "did this call name already resolve to a real declared
function or procedure" answer -- the legacy-method-call check just never
consulted it.

**Fix (`external/rea/src/rea/semantic.c:3932`):** added `!callDecl` to the
legacy-method-call check's guard condition, mirroring the adjacent
`callDecl`-gated check. The check is now a fallback: it only fires when
normal call resolution has already failed to find a matching declared
function or procedure for that exact name, so it no longer overrides a
valid declaration -- it just stops being consulted at all once `callDecl`
is non-NULL.

**Verified:** both repro casings (`type DB` and `type Db`) now compile and
run, printing `42`. A control case with `DB` in scope and an
underscore-prefixed call to a name that is NOT declared anywhere
(`db_missing()`) still correctly fails, now surfacing both the legacy-method
hint and the normal `[SCOPE-001] identifier 'db_missing' not in scope`
diagnostic (previously only the legacy-method hint fired, unconditionally,
for any prefix match) -- confirming the fix is gating, not blanket
suppression, and undefined-identifier errors in this shape still surface.
Full existing suite (`tests/run.sh`) passes unchanged; new regression
fixtures `tests/legacy_method_call_shadow_pass.aether` (the `db_open` case)
and `tests/legacy_method_call_undefined_fail.aether` (the still-must-fault
control), wired into `run.sh`. Language version bumped to `2026-07-19-5`
(real compiler behavior change, not docs-only).

### `xs + [a, b]` / `arr1 + arr2` (array concatenation beyond single-element append) has no VM operator — *fixed 2026-07-19-6*
The single-element append idiom (`xs = xs + [v];`, used throughout this
corpus) works because the Aether parser specifically pattern-matches
`target = src + [item]` with a **one-element** array literal and lowers it
to a setlength + indexed-assign expansion
(`ast_parser.c:4752`, `buildArrayAppend`) -- the comment there is explicit:
"target = src + [item] (single-element literal)". Any other array `+` shape
-- a literal with 2+ elements (`xs + [1, 2]`), or `+` between two
already-built arrays/array-valued expressions (`ys + two`) -- isn't
special-cased by that lowering and falls through to a raw `ARRAY + ARRAY`
VM binary op that has no arithmetic meaning, producing `Runtime Error:
Operands must be numbers for arithmetic operation '+' ... Got ARRAY and
ARRAY`. Minimal repro: `let b: Int[] = []; b = b + [1, 2];` fails (`[] +
[1]` succeeds); `ys + two` where `two: Int[] = [1, 2]` fails identically --
confirms it's about the right operand's *element count*, not
literal-vs-variable. Found via a generated "Simple Database" example
appending a `[key, value]` pair in one `+` (worked around here with two
sequential single-element appends).

Neither guide documents this restriction -- the *Dynamic arrays* section
shows `xs = xs + [7]; // append pattern` with no caveat that only exactly
one element is supported, so a model (or a human) has every reason to
expect `xs + [a, b]` or `arr1 + arr2` to just concatenate.

**Suggested fix, in order of value:** (1) generalize the literal case in
`buildArrayAppend`'s call site to expand an N-element literal into N
sequential single-element append lowerings (cheap, covers the common case
above); (2) for genuine `array_expr + array_expr` concatenation (both sides
arbitrary, not necessarily literals), either add a real VM opcode/builtin
for array concatenation, or at minimum have the semantic analyzer reject it
with a clear diagnostic pointing at the loop-and-append idiom instead of
falling through to the generic arithmetic error. Needs a regression test for
each shape once fixed.

**Resolved 2026-07-19-6.** Fixed both cases, entirely at the `ast_parser.c`
parse-time lowering level (no VM changes):

1. **N-element literal append.** `buildArrayAppend`'s call sites (the
   `target = src + [items...];` statement lowering and the `let x: T[] =
   src + [items...];` initializer lowering) now loop over however many
   elements the array literal has -- 0, 1, or more -- emitting one
   setlength+indexed-assign pair per item, instead of requiring exactly one.
   An empty literal (`src + []`) is a no-op (just the copy, if any). `xs =
   xs + [v];` (the pre-existing single-element idiom used throughout this
   corpus) is unchanged since N==1 is just the first iteration of the same
   loop.
2. **General `array_expr + array_expr` concatenation.** Investigated a real
   VM-level opcode first: `ArrayObj` (`pscal-core/src/core/types.h:216`) has
   non-uniform ownership (refcounted dynamic arrays vs. deep-copied static
   arrays, guarded by `dynamic_array_refcount_mutex`) that the codebase's own
   in-flight "VM 2.0" rewrite plan flags as its highest-risk type family, and
   no existing runtime helper concatenates two already-built arrays -- so a
   VM opcode was medium-to-high risk, not low. Went with the frontend-only
   approach instead: `target = src + other;` / `let x: T[] = src + other;`,
   where `other`'s inferred type is array-shaped ("[]"-suffixed, so ordinary
   Text/Int/Real `+` is untouched), now lowers to `other` hoisted into a temp
   local (evaluated exactly once, even if it's a call with side effects),
   then `setlength(target, oldLen + length(other))` followed by an index loop
   copying `other`'s elements in -- reusing the same hardened
   setlength/indexed-assign primitives the append idiom already relies on,
   with zero VM/runtime array-ownership changes.

Verified: the multi-element literal case (2- and 3-element self-reassignment
and `let`-initializer appends, plus an empty-literal no-op), the general
concatenation case (`ys = ys + two;` for `Text[]`, `let zs: Int[] = xs + ys;`,
and a call-valued RHS confirmed evaluated exactly once via a side-effecting
counter), and that ordinary string concatenation (`Text + Text`) and the
original single-element append idiom are both unchanged. Full existing suite
(`tests/run.sh`) passes unchanged. New regression fixtures
`tests/dynamic_array_append_multi_pass.aether`,
`tests/dynamic_array_concat_pass.aether`, and
`tests/dynamic_array_concat_let_pass.aether`, wired into `run.sh`. Both
guides' *Dynamic arrays* sections updated. Language version bumped to
`2026-07-19-6` (real compiler behavior change, not docs-only).

### Real socket API (`socket*`) exists and works but was completely undocumented; models invent a fictional `tcpsocket*`/`udpsocket*` namespace — *documented (docs-only), 2026-07-19*
A generated batch of 12 examples included 5 built entirely around
`tcpsocketlisten`/`tcpsocketaccept`/`tcpsocketread`/`tcpsocketsend`/
`tcpsocketclose`, `udpsocket`/`udpsend`/`udpreceive`/`udplisten`/
`udpreceivefrom`, and `resolve` -- **none of these exist** (`builtin_info`
returns `null` for all of them). The real, working API is a unified
BSD-socket-style `socket*` family (`socketcreate`, `socketbind`,
`socketbindaddr`, `socketlisten`, `socketaccept`, `socketconnect`,
`socketsend`, `socketreceive`, `socketclose`, `socketpoll`,
`socketsetblocking`, `socketpeeraddr`, `socketlasterror`) plus `dnslookup`
(forward hostname->IP only, single `Text` arg -- no reverse lookup). Neither
guide mentioned any of this; the small guide's *Files and environment* section
said only "Sockets (`socket*`) ... discover them via `builtin_info(...)`",
which is not enough for a model to reconstruct argument order or blocking
semantics.

**Resolved (docs-only) 2026-07-19.** Read the full implementation of every `socket*`
function plus `dnslookup` in `builtin_network_api.c` (each `runtimeError()`
call at the top of a `vmBuiltin*` gives the exact expected argument shape) and
confirmed all 14 signatures, return-value meanings, and the blocking calls:
`socketaccept`/`socketreceive` block the calling task until a peer
connects/sends (a `0`-length `socketreceive` result means the peer closed the
connection, not failure), and a failed `dnslookup` sets
`vm->abort_requested` on the owning task thread. Added a new **Sockets**
section to both guides (signatures table, the `type`/`family` int constants,
the blocking-accept/receive gotcha and its two fixes) plus a verified,
runnable same-process TCP client+server example: the listening socket is
created/bound/put into listen state *before* a `par` block (not inside one of
its branches), so the client branch can never race ahead of a
not-yet-listening server, and `par`'s existing join semantics mean no
extra synchronization is needed. Confirmed it runs to completion in well
under a second (not a hang) across repeated runs via a background-run +
`kill -0`-poll + hard-kill-after-20s harness, both standalone and now wired
into `tests/run.sh` as `tests/socket_echo_pass.aether` (exact-stdout
regression check, same 20s-hang guard). Full existing suite
(`tests/run.sh`) passes unchanged. Doc/example changes only -- no compiler or
language change, so no language-version bump.

### No file-content read/write API reachable from Aether's own type system — *fixed 2026-07-19-4*
A generated "File Line Counter" example invented `fileopen`/`fileeof`/
`filereadline`/`fileclose` (none exist). The *real* file I/O is classic
Pascal-style (`assign`, `reset`, `rewrite`, `append`, `read`, `readln`,
`write`, `close`, `eof`, `erase`, `rename` -- all real `vm_builtin`s,
confirmed via `builtins_json(true)`), but it requires a Pascal "file
variable" binding that Aether's own declared-type surface (`Int`, `Real`,
`Text`, `Bool`, `Void`, records, arrays, tuples, `ToonDoc`/`ToonNode`,
`MStream`) had no syntax for: `assign(f, path)` with `f` declared as plain
`Text` failed outright with `"First arg to Assign must be a file variable."`,
and no `File`/`TextFile`-shaped type existed to declare `f` as instead.

**Root cause / investigation:** the error string lives in pscal-core's
`backend_ast/builtin.c` (`assign`/`reset`/`rewrite`/etc. all guard on
`VALUE_TYPE(*fileVarLValue) != TYPE_FILE`), and `TYPE_FILE` is a real
`VarType` (`core/var_type.h`). Rea -- the C-like frontend Aether's
`ast_parser.c` lowers into -- already has a working surface for it: rea's
*lowercase* `text` keyword (`external/rea/src/rea/parser.c`'s `mapType`)
maps to `TYPE_FILE`, distinct from Aether's own *capitalized* `Text` string
type (which maps to rea's `str` / `TYPE_UNICODE_STRING`). rea's own examples
(`external/rea/examples/base/hangman5`, `sqlite_spotify_demo`,
`archive/openweather_forecast`) already exercise the full `text f;
assign(f, path); reset(f); readln(f, line); eof(f); close(f);` idiom end to
end. Critically, `readln(f, line)`'s write-back into `line` needs no general
by-reference-parameter support: it's handled entirely inside pscal-core's
shared `compiler.c`, which special-cases argument-lvalue evaluation by
builtin *name* (`assign`/`reset`/`readln`/... around the `is_read_proc` /
`hasFileArg` checks), keyed on `arg_node->var_type == TYPE_FILE` -- a
compiler-level mechanism, invisible to and unaffected by whichever frontend
(rea or Aether) produced the AST. So the entire gap reduced to: Aether's
frontend had no binding-table entry that would produce a `TYPE_FILE` AST
node for a declared variable.

**Fix (`src/aether/ast_parser.c`):** added one entry to the `mapAetherType`
binding table -- `{ "File", "text", TYPE_FILE }` -- so `let f: File;` lowers
to the exact same `AST_TYPE_IDENTIFIER` shape rea's own parser builds for
`text f;`, and a matching `TYPE_FILE -> "File"` case in
`aetherTypeNameForVarType` for the reverse direction. No parser, semantic, or
VM-boundary changes were needed beyond that single table row -- every
compiler-level file-builtin mechanism (declaration bytecode, argument lvalue
evaluation, runtime dispatch) is pscal-core code shared by both frontends and
already worked on `TYPE_FILE` nodes regardless of origin.

**Verified:** a hand-written program declaring `let f: File;` inside `fx`,
doing `assign`/`rewrite`/`writeln`/`close` then `reset`/`readln`+`eof` loop/
`close` then `erase`, ran correctly end to end (timeout-guarded) against a
real temp file -- both the write and read-back paths, plus cleanup. New
regression fixture `tests/file_io_pass.aether` pins the same sequence
(exact-stdout check), wired into `tests/run.sh`; full existing suite passes
unchanged. Both guides' "Files and environment" sections now document `File`
and the content-I/O builtin table/example alongside the pre-existing
existence/metadata-only builtins. Language version bumped to `2026-07-19-4`
(real type-system addition, not docs-only).

### Three silent-failure gaps in the AST frontend now emit coded diagnostics — *fixed 2026-07-01-1*
A program that exits non-zero with **no message at all** (empty stderr, empty
`--diagnostics-json`) — or that crashes the VM outright — is the worst case for
both humans and the LLM repair loop: there is nothing to react to. Three such
cases on the default AST path were closed (all in `ast_parser.c`):

1. **Silent parse failures → `[SYN-001]` backstop.** Only 11 of the parser's ~50
   diagnostic sites emitted a message; the rest set `p->hadError` and returned
   `NULL`, so any failure that propagated up such a chain exited 1 silently (e.g.
   `rec Point { ... }` — `rec` is not a keyword, a common slip for `type`). Every
   parser diagnostic now funnels through one counting sink (`aetherDiagf`); after
   the authoritative parse, `hadError` with a zero count emits a coded `[SYN-001]`
   at the stalled token. This is a *class* fix — it backstops every current and
   future silent parse path, not just the ones found.
2. **Undefined method on a record → `[SCOPE-001]` (compile time).** `recv.method()`
   on a user record lowers to a mangled `Type.method` global call; with no such
   method it degraded to an undefined-global read that failed only at runtime
   (`Undefined global variable 'Point.distance'`). `aetherCheckMemberCalls` walks
   the parsed program and verifies each `Type.method` resolves, mirroring the
   existing `FIELD-002` field check. Conservative (real record type, no parent,
   not a field) — no corpus false positives. Uses `SCOPE-001` because `METH-001`
   already names the "no outer-local capture" rule.
3. **`par` branches sharing a record → `[PAR-001]` (compile time).** Reproduced
   the crash the sweeps flagged (below): the *same* pointer-backed record handed
   to two `par` branches is written concurrently → heap double-free
   (`malloc: pointer being freed was not allocated`), aborting with SIGABRT/SIGTRAP
   and no message (30/30 on a 2-branch shared-Text-field repro). Making the VM
   heap thread-safe is out of scope (and leaves write-ordering undefined), so
   `parseParBlock` rejects the aliased record at compile time; each-branch-its-own-
   record (the documented idiom) is unaffected, and scalar args are never flagged.

Regressions: `tests/{method_undefined_fail,par_shared_record_fail,unknown_construct_fail}.aether`.
Guides updated (PAR-001 rule + the SCOPE-001 method line in both). **Follow-up idea:**
the `--diagnostics-json` collector (`rea/src/rea/main.c`) turns each `help: see <CODE>`
line into a spurious `code:null` entry (pre-existing for every coded diagnostic);
folding `help:` like `hint:` would clean the JSON for the repair loop.

### Builtin reference shipped into the training corpus — *done 2026-06-28*
The specialization reference corpus shipped only the prose guide, so trained
models never saw the real builtin surface (the guide documents a curated subset
and says "discover the rest"). Closed the gap:
`tools/aether_export_builtins_reference.py` generates a non-SDL builtin reference
from the compiler's own `builtins_json(true)` (the ~52 fully-documented
builtins with signature/usage today + the rest as a categorized name inventory +
a prominent "Discovering builtins" queryability section), and
`aether_specialization_export_reference_corpus.py` now ships it alongside the
guide (binary-driven, regenerated each build, graceful skip if the binary is
absent). ~3.1k tokens.
**Exclusions (auditable, curated in the generator):** SDL graphics/3d/demo by
category (99) + by name the `landscape*` leak, legacy Pascal CRT (~40), DOS unit,
VM/introspection plumbing, redundant non-canonical alias spellings, and the
`to be filled` registry junk (182 hidden → 240 kept). Scope chosen with the
owner: the clean data-automation surface, not just "everything but SDL".
**Synergy:** binary-driven, so as the *Enrich builtin metadata* work below adds
signatures, the corpus reference improves automatically on the next regen.
**Follow-ups:** (1) rebuild + dataset version-bump for it to actually land in
training; (2) `to be filled` is a real placeholder builtin registered in
pscal-core — worth fixing at source.

### Enrich builtin metadata so discovery teaches *how to call* — *in progress 2026-06-27*
**Done:** single-sourced effectfulness in `pscal-core` via
`pscalBuiltinNameIsEffectful()` (builtin.h/.c) — aether's FX-001 gate delegates
to it, and `builtins_json`/`builtin_info` derive their `effectful` flag from it
(so every builtin, even un-enriched, now reports correct effectfulness; fixed
`toon_parse` which was mislabeled effectful). Added a `source` field to the
metadata struct (raw builtins now labeled `vm_builtin`, not `aether_alias`).
Added curated signatures for the first batch: `fileexists`, `getcurrentdir`,
`getenv`, `getenvint`, `paramcount`, `paramstr`, `copy`, `pos`, `trim`,
`stringofchar`, `realtostr`, `formatfloat`, `random`, `randomize`, and HTTP core
(`httpsession`/`httpclose`/`httpsetheader`/`httprequest`/`httperrorcode`/
`httplasterror`). Verified no breakage: clike/rea/pascal/exsh/aether all build
and run. No language VERSION bump (gating set unchanged; this is introspection +
internal refactor).
**Batch 2 done (2026-06-27):** added signatures for all of `sqlite*` (21),
remaining `http*` (getheader/getlastheaders/clearheaders/setoption/requesttofile/
requestasync/isdone/cancel), `mkdir`/`rmdir`, and `mstreamloadfromfile`/
`mstreamsavetofile`. Fixed a real bug found while chasing `mkdir`: Aether `Text`
**variables** are `TYPE_UNICODE_STRING` at runtime (literals are `TYPE_STRING`),
so builtins doing a strict `args[i].type == TYPE_STRING` check rejected variable
args (worked with literals). Fixed `fileexists`, `dosMkdir`, `dosRmdir` to use
`isPascalStringType()` (accepts both; strictly more permissive, safe for all
frontends). Side effect: the base example suite went 49/51 -> 50/51 (the
`ai_helpers` example now passes). All five frontends rebuild + run.

**Batch 3 done (2026-06-27):** async-HTTP tail (`httpawait`/`httptryawait`/
`httprequestasynctofile`/`httpgetasyncprogress`/`httpgetasynctotal`). Discovery
now exposes full signatures for **58 vm_builtins** (all sqlite + http, fs/env,
strings, Real→Text, mkdir/rmdir, mstream file ops). The strict-TYPE_STRING sweep
turned out narrow: no `!= TYPE_STRING` arg-reject patterns remain in non-shell
code (the 3 fixed were the lot); remaining `== TYPE_STRING` hits are benign
value-dispatch that already handle unicode strings. Only exsh's
`shell_builtins.inc` (~102 checks) is unverified — see the spawned task to test
whether exsh string vars are TYPE_UNICODE_STRING before touching them.

**Optional remaining (low value, niche):** raw Pascal file-handle I/O
(`fopen`/`fclose`/`blockread`/`blockwrite`/`filesize`/`assignfile`/`closefile`/
`ioresult`/`eof`), console input (`readkey`/`keypressed` — return types live in a
CRT module, not builtin.c), and the clock (`gettime`/`getdate`/`realtimeclock` —
var-param, awkward to express as a signature). All already report correct
`effectful` in discovery via the predicate; only full signatures are missing, and
these are rarely used in Aether automation.

**Known bug class to sweep — strict `== TYPE_STRING` rejects Aether Text vars:**
the three fixed above are unlikely to be the only ones. Any builtin that checks
`args[i].type == TYPE_STRING` (rather than `isPascalStringType`) will reject a
`Text` *variable* while accepting a literal. Worth a grep-and-fix sweep across
pscal-core builtins; the fix is always `== TYPE_STRING` -> `isPascalStringType(...)`
(and `!= TYPE_STRING` -> `!isPascalStringType(...)`), which only widens acceptance.

Remaining-work mechanism: keep populating the metadata table in
`pscal-core/src/ext_builtins/query_builtin.c` (the `{name, backend_name, kind,
return_type, signature, usage, category, effectful, source}` array). Signatures
already exist in the C as arg-check error strings — promote them. The
single-source effectful accessor and the breakage analysis (introspection-only,
no dispatch/handler/compile change; all five frontends rebuild + run) are done
and verified.

### TOON output / serializer API — *idea*
Aether can **parse** TOON (`toon_parse`, `toon_root`, `toon_at`, `toon_key`,
`toon_get_*`) but cannot **emit** it. There is no `toon_create_doc` /
`toon_set_*` / serialize surface. Today, machine-readable output must be
hand-built as a `Text` string. A small writer/serializer surface would close the
read/write asymmetry and is a natural fit for the "parse → transform → emit"
domain. (Surfaced repeatedly when models reach for a TOON builder that doesn't
exist, 2026-06-27.)

### Higher-order via named dispatch (capture-free) — *idea, only on demand*
Aether deliberately has no closures or first-class functions (see **decided:
no closures** below). If collection-transform ergonomics ever justify it, the
principled extension is to reuse the existing string-name dispatch pattern that
`task_spawn("delay", ...)` already uses: e.g. `map_named(xs, "double")` where
`"double"` names a top-level `@pure` function. No anonymous functions, no
lexical capture, statically checkable — preserves the METH-001 / FUNC-001
invariant. Add only if real demand appears; first-order `loop` covers the domain
today.

### Fully support type-keyword names as record members — *idea / parser-roadmap*
Follow-up to *Reserved words collide with member names* (now diagnosed, not
supported). The AST parser's member-**access** path already accepts type-keyword
tokens after `.` (`ast_parser.c` ~1281 uses `aetherTokenIsIdentifierLike`), and
the design comment on that predicate explicitly lists a *field name* as a
position where lowercase type-name keywords (`word`, `text`, `int`, `bool`, …)
"must be valid" — Aether spells its own types capitalized, so these lowercase
spellings are ordinary identifiers. But the field-**declaration** branch tests
`== REA_TOKEN_IDENTIFIER` exactly, so `word: Text;` is rejected today. Widening
that one branch to `aetherTokenIsIdentifierLike` would let type-keyword field
names parse *and* be read/written (access already works), making the current
`'word' is a reserved type name` diagnostic unnecessary for that subclass — a
strict AST-better improvement, and it would realize the documented invariant.
Deliberately NOT done in the diagnostics pass: (a) it is a language-surface
widening the parser-roadmap owner should scope (coordinate, don't duplicate —
[[aether-frontend-rewriter]] / `parser_roadmap.md`); (b) it only helps
identifier-like keywords — operator words (`mul`) and value/structure keywords
(`new`, `for`) still can't be member names, so the "avoid reserved words"
guidance stays; (c) needs its own end-to-end + suite validation (decl + every
`.access` + method mangling). A simple, teachable "don't name members after
reserved words" rule was preferred for now.

---

## Known gaps

### `@cost` is decorative — *decided 2026-07-01: documented as non-binding*
`@cost <n><unit>` (units: `ns us ms s op ops step steps`) is syntax-validated
but **not enforced or tracked** — it carries no codegen and no runtime check.
Decision: option (b) for now — both guides state explicitly that `@cost` is
syntax-checked but non-binding (machine-readable intent, not enforcement), so
it can no longer be oversold. Upgrading it to a real, tracked/asserted budget
(option (a)) stays on the table as future work if a runtime accounting story
appears. (Contrast: `@pre`/`@post` lower to runtime assertions; `@pure` is
enforced at compile time.)

### Math builtins were undocumented — *decided / done 2026-06-27*
A full numeric/trig family lived uncategorized in the runtime but appeared
nowhere in either guide (`abs`, `sqrt`, `sqr`, `pow`/`power`, `exp`, `ln`,
`log10`, `round`/`trunc`/`floor`/`ceil`, `sin`/`cos`/`tan`,
`arcsin`/`arccos`/`arctan`/`atan2`/`cotan`, `sinh`/`cosh`/`tanh`, `min`/`max`/
`clamp`, `odd`, `factorial`, `fibonacci`, `random`/`randomize`). Added a **Math
builtins** section to both guides (Pascal naming: `arctan` not `atan`, `ln` not
`log`). Also added Real→Text (`formatfloat`/`realtostr`) to the conversion
surface — these exist and `formatfloat(v, prec)` works as a `Text`-returning
helper (the `value:width:precision` spec is `println`-only).

### Effect model now covers host interaction — *decided / done 2026-06-27-3*
FX-001 (and `@pure`) previously gated only output/`read`/`ai_chat`/tasks/`sleep`,
so a `@pure` function could call `getenv`/`random`/`fileexists` and still
compile (purity was unsound). Extended the effectful-builtin list in
`semantic.c` to cover all host interaction: filesystem + file I/O, env, CLI,
`random`/`randomize`, clock, console input, `http*`/`socket*`/`dnslookup`,
`sqlite*`, and the `task_wait` family. Match is now case-insensitive; enforced on
both parser paths. Pure math/string/conversion stay ungated. Language version
bumped to `2026-06-27-3`. SDL/graphics and terminal screen-control deliberately
left ungated. Documentation surfaced **filesystem + Pascal string ops**
(`copy`/`pos`/`trim`/`stringofchar`) in both guides; BUILT-001 softened to
"supported surface, discover others via `builtin_info`."

### HTTP / sockets / SQLite — *HTTP done 2026-07-09-1; sockets done (docs-only) 2026-07-19; SQLite deferred*
These are correctly fx-gated (effectful). Status:
- **HTTP — surfaced 2026-07-09-1.** `MStream` is now a first-class opaque
  Aether type (lowers to rea `mstream`/`TYPE_MEMORYSTREAM`; explicit decls and
  inference from `mstreamcreate`/`mstreamfromstring` both work), the opaque
  handle fences were generalized to a three-kind model (new **MS-001**
  diagnostics: stream-returning builtin bound to a non-MStream type — the
  former runtime `Cannot assign MEMORY_STREAM to integer` crash, now caught at
  compile time — plus arithmetic and cross-kind misuse), and HTTP
  (`httpsession`/`httprequest`/`httpsetheader`/`httpclose` + the mstream
  helpers) is documented in both guides. `examples/base/http_weather` runs
  live on an `AETHER_ENABLE_CURL=ON` build. A convenience alias layer
  (`http_get(url) -> Text`, `http_post(url, body) -> Text`) is still a
  possible future ergonomics addition, but no longer a prereq.
- **Sockets — documented 2026-07-19 (docs-only).** See the dedicated entry
  above; both guides now have a full **Sockets** section (signatures, the
  `type`/`family` constants, the blocking-accept/receive gotcha) with a
  verified `par`-coordinated client+server example, and
  `tests/socket_echo_pass.aether` is wired into `tests/run.sh`.
- **SQLite** (`sqlite*`, ~21 fns): real and coherent, but large; surface only
  on demand if a DB use case appears.
- **CRT/console** (`gotoxy`, `clrscr`, `textcolor`, ...): terminal UI; leave
  discovery-only. SDL/graphics/GL/audio/landscape stay undocumented (excluded).

### Two shipped examples fail to compile (pre-existing) — *gap*
Found incidentally while regression-testing the effect change (both fail on AST
*and* rewriter paths, so not caused by it):
- `examples/base/ai_helpers` → `[SCOPE-001] identifier 'openaichatcompletions'
  not in scope`. The example calls `openaichatcompletions` directly, but that
  raw backend name isn't resolvable as an Aether identifier (use `ai_chat`).
- `examples/base/effects_contracts` → `[SCOPE-001] identifier 'result' not in
  scope` in a `@post` contract. Suggests `@post result` resolution is broken in
  at least some shape — worth a closer look since `@post` referencing `result`
  is documented and used elsewhere.
Fix the examples (and, for the second, confirm whether `@post result` is broken
generally or only here).

### Misleading `FLOW-001` on `ret` inside `fx` — *gap*
`ret` is not legal inside an `fx` block (see the guide: return from the
surrounding function). But `ret <value>;` inside `fx` in a non-Void function is
reported as `[FLOW-001] non-Void function has a fallthrough path with no return
value`, not a direct "`ret` is not legal inside `fx`." The diagnostic sends the
reader toward the wrong fix. A targeted message would help. (Verified
2026-06-27.)

### Malformed `@cost` is silently dropped — *gap*
A non-canonical form such as `@cost("O(n) ...")` (quoted string instead of
`<n><unit>`) is not recognized as an annotation and is silently lowered to a
comment — no error, no effect. It should emit an ANN-001-style diagnostic so the
mistake is visible. (Verified 2026-06-27.)

---

## In-flight (tracked elsewhere)

- **AST frontend cutover (P7).** See `parser_roadmap.md`. Subsumes the legacy
  rewriter's structural bugs (compound-line handling, error line-number offset,
  multi-field-per-line record literals that the AST parser already accepts).
- **AST diagnostic quality.** The AST path currently double-emits diagnostics,
  reports a bogus `L0` line on the `par` non-call error, and uses terser wording
  without the legacy `hint:` repair line. Align with legacy (single diagnostic,
  correct line, matching wording/hints) before cutover.

---

## Decided

### No closures / first-class functions — *decided 2026-06-27*
Functions are not values: no anonymous functions, no lambdas, no closures, no
passing functions as arguments (rule **FUNC-001**, in both guides). Rationale:
closures are the richest source of generation ambiguity for an LLM-targeted
language; the benchmark evidence shows models reach for them unprompted (the most
dangerous reflex to fence off); and they would break the structural purity/effect
checks without effect polymorphism. The capture-free alternatives are already in
the language:
- **Concurrency:** `par { f(); g(); }` runs your own functions in parallel and
  joins; results flow through pointer-backed records. (`task_spawn` is a
  builtin-only handle API, not a way to run user code.)
- **If HOFs are ever needed:** the named-dispatch idea above, not lambdas.

---

## Mined from generative idea-mining — 2026-06-29

*Surfaced by `tools/aether_idea_miner.py`, the generative (no-oracle) sibling of
`aether_doc_bench.py`: each model freely wrote Aether programs of its own
choosing, which were compiled + run with a repair loop; "success" = compiles and
runs (exit 0), and the product is the failure analysis. This run: **8 models
across 4 systems in parallel** (claw1 ollama: `gpt-oss-120b`,
`mistral-small-24b`, `qwen3-32b`, `exaone3.5-32b`; autoclaw GLM: `glm-5-turbo`,
`glm-5.2`; Gemini: `gemini-2.5-flash`, `gemini-3.1-pro-preview`), 40 self-chosen
programs, 33 ran clean, against `aether` 2026-06-27-3. Every finding below was
re-verified with a minimal repro on the gating binary. Full report (per-program
and per-attempt): `Tests/aether_doc_bench/out/idea_miner_2026-06-29.json` (and
`.md`). `qwen3.6-35b-a3b` timed out (reasoning model, >20 min for 5 programs) and
is excluded.*

The dominant theme: capable models reach for **record constructors, foreach, and
type casts** that Aether does not have, and trip on **how instances must be
bound for mutating methods**. These are the most direct language-design signals
the generative harness can produce.

### No record-constructor idiom; models reach for `fn new()` / `Type.new()` / `__init__` — *gap (2 models)*
Models consistently try to define a constructor inside a `type`, and Aether has
no such form. Verified failure modes:
- `fn new() -> C { ... }` as a method → `expected function name after 'fn'`
  (`new` is the reserved allocator keyword). Hit by `qwen3-32b` (two programs).
- `let c: C = C.new();` (static constructor call) → `expected a class name after 'new'`.
- `fn __init__(self) { ... }` (Python idiom, no return type, explicit `self`) →
  `[SYN-001]`. Hit by `exaone3.5-32b`.

The working idiom (verified) is allocate-then-assign, with no constructor method:
```
type C { value: Int; }
fn main() -> Void {
    let c: C = new C();   // new T() is the only constructor
    c.value = 0;
    ret;
}
```
**Action:** guide clarification (both guides) — show the `new T()` + field-init
idiom explicitly, and state that `new`/`__init__`/`Type.new()` are **not**
constructor forms (use a top-level factory `fn` with a non-reserved name if a
constructor is wanted). Optionally a targeted diagnostic on `fn new(`.

**Partly done (aether `2026-06-30-1`).** The targeted `fn new()` diagnostic
shipped with the reserved-words fix below: a method named `new` now reports
`'new' is a reserved keyword (the object allocator) and cannot be used as a
method name` and its hint points at the idiom (`new T()` + field assignment, or a
top-level factory `fn`). Both guides now state `new T()` is the only constructor
and that `new` / `__init__` / `Type.new()` are not constructor forms. Still to do
if desired: targeted diagnostics for the *call-site* forms `C.new()` and
`fn __init__` (currently `expected a class name after 'new'` / `[SYN-001]`).

**Guide section added (2026-06-30, docs-only — no VERSION bump).** Both guides now
carry a dedicated *Constructing records and typing bindings* section that shows
the `new T()` + field-init idiom, the record-literal one-shot form
(`T { field: value }`), and the factory-`fn` alternative, and folds in the
typed-binding rules from the two findings below (always annotate `new` instances
and array literals). All positive snippets re-verified on `aether 2026-06-30-2`.

### Reserved words collide with member names, with cryptic diagnostics — *parse cases fixed 2026-06-30-1; FX-001 case open*
Member names (fields and methods) that collide with an Aether/PSCAL reserved word
or effectful builtin fail, and the diagnostic never names the collision:
- Field `word: Text;` → `unexpected token in type body` (verified: `word`
  collides with the PSCAL `Word` type; `count`, `value`, `a`, `b` all compile).
  Hit by `glm-5-turbo`.
- Method `fn mul() -> Void { ... }` → `expected function name after 'fn'`
  (verified: `mul` collides with the `mul` builtin; `add`, `push`, `pop` all
  compile). Hit by `gemini-3.1-pro-preview` (a stack-VM `mul` method).
- Method `fn print() -> Void { fx { ... } }` called outside `fx` → misleading
  `[FX-001] call to 'print' requires an fx block` (the user method collides with
  the effectful builtin `print`). Seen earlier with `mistral-small-24b`.
- `new` as a method name → `expected function name after 'fn'` (see above).
**Action:** emit a diagnostic that names the collision (e.g. "'word' is a
reserved type name; choose another field name") instead of `unexpected token in
type body` / a bare `FX-001`; and list the reserved words to avoid as member
names in the guide. Root cause is shared with the constructor finding above:
member identifiers are checked against the full reserved/builtin namespace.

**Resolved — parse-shaped cases (aether `2026-06-30-1`).** The AST frontend now
names both parse-time collisions with `[SYN-001]`: a field on a type-name keyword
→ `'word' is a reserved type name and cannot be used as a field name`; a method on
an operator word (`mul`/`div`/`mod`/`xor`) or a keyword (`new`/`for`/…) → `'mul'
is a reserved operator word … as a method name`, each with a rename hint (`fn
new()` also points at the missing-constructor idiom: `new T()` + field assignment,
or a top-level factory `fn`). Impl: `aetherReservedWordCategory` +
`reportReservedMemberName` in `src/aether/ast_parser.c` (the `unexpected token in
type body` else-branch and the `expected function name after 'fn'` branch), code
inference in `diagnostics.c`. Regression fixtures
`tests/reserved_{field,method,new}_name_fail.aether` wired into `run.sh`; guide
note added to both LLM guides. Generic fallback preserved for non-word junk
tokens, and the legacy rewriter path is untouched.
**Still open — the semantic case:** a *user method* named after an effectful
builtin (`fn print()`) still yields a misleading `[FX-001] call to 'print'
requires an fx block` when called outside `fx`, because the effect checker matches
by name and cannot tell the user's method from the builtin. That is an
effect-check fix, not a parser fix — deferred. See the follow-up idea *Fully
support type-keyword names as record members* under Open ideas.

### Mutating method works only when the instance has an explicit type — *fixed 2026-06-30-3*
A method that mutates `self` compiles and runs when the receiver is bound with an
explicit type, but fails when the type is inferred from `new`:
```
type C { value: Int; fn inc() -> Void { self.value = self.value + 1; } }
fn main() -> Void {
    let c = new C();      // inferred  -> [no code] argument 1 to 'c.inc' expects type POINTER but got VOID
    let c: C = new C();   // explicit  -> OK
    c.inc();
    ret;
}
```
Hit by `mistral-small-24b` (unrepaired — the model could not recover from the
`POINTER`/`VOID` message). **Action:** make `let c = new C()` infer the
pointer-backed record type so mutating methods work, **or** emit a clear
diagnostic ("annotate the instance type: `let c: C = ...`") instead of the raw
`expects type POINTER but got VOID`.

**Guide clarification shipped (2026-06-30, docs-only).** Both guides now tell
models to always annotate `new` instances, and Repair rules map the raw
`expects type POINTER but got VOID` message to "annotate the receiver". *Trigger
refined during the guide work:* it is a **statement-level call to a `-> Void`
method** on the inferred receiver (`c.inc();`, `c.show();`) — **not** mutation per
se. An expression-context call resolves the receiver type even when the method
mutates: `let n: Int = c.bump()` compiles with `c` inferred (verified on
`2026-06-30-2`). So the framing is "annotate `new` instances" (universal), not
"mutating methods need a type". The underlying inference/diagnostic fix is still
open.

**Resolved (2026-06-30-3).** The inferred-`let` path in `ast_parser.c`
(`parseLetDeclAfterKeyword`) hand-built the declared type node and, for any
user type, emitted a bare `AST_TYPE_REFERENCE` at `TYPE_UNKNOWN` — it never
consulted `lookupType` to detect a record and wrap it in `AST_POINTER_TYPE`, so
the inferred variable stayed untyped while the annotated `: C` path (via
`buildTypeNode`) produced `POINTER_TYPE -> TYPE_REFERENCE(RECORD)`, var_type
`POINTER`. The inferred path now routes through that same `buildTypeNode` helper,
so `let c = new C();` and `let c: C = new C();` produce a **byte-identical**
declaration (verified with `--dump-ast-json`) and the statement-level Void-method
call type-checks. The symmetric second site (inline object-literal method chain
that returns a record, `let x = Foo { ... }.makeBar();`) got the same fix. Builtin
and un-inferable initializers are unchanged (the "cannot infer" diagnostic still
fires). Regression: `tests/inferred_object_mutation_pass.aether` (prints 42).

### Array literals need an explicit type; arrays of record literals fail to parse — *record-literal case fixed 2026-07-19-1; element-type inference still open*
- `let xs = [1, 2, 3];` → `[TYPE-001] cannot infer the type of 'xs'`; needs
  `let xs: Int[] = [...]` (verified). Hit by `mistral-small-24b` (2 programs:
  `[Int]` and `[Real]`).
- A multi-line array of record literals fails to parse outright:
  `let ps: Person[] = [ Person { ... }, Person { ... } ];` →
  `Expected ']' to close array literal` (likely the line-based rewriter not
  handling record literals nested in an array literal). Hit by `gpt-oss-120b`
  (unrepaired).
**Action:** infer homogeneous array-literal element types; support (or document
the workaround for) record literals inside array literals. At minimum, the guide
should state arrays require an explicit type and show building an array-of-records
by appending in a loop.

**Guide clarification shipped (2026-06-30, docs-only).** Both guides now state
array literals require an explicit type (`let xs: Int[] = [...]`) and show
building an array-of-records by append-in-loop (`ps = ps + [p];`, verified on
`2026-06-30-2`); it is called out in the new *Constructing records and typing
bindings* section, in *Never Generate These* / inference, and in Repair rules.
Still open (compiler): infer homogeneous element types.

**Reconfirmed 2026-07-19 by generative idea-mining** (3 more distinct models —
`qwen/qwen3-coder-30b`, `ibm/granite-4-h-tiny`, `qwen3.5-9b-mlx` — hit the same
`Expected ']' to close array literal` on the exact reported shape, one
occurrence each even with a single-element, single-line array), which is what
prompted the actual fix below.

**Resolved (2026-07-19-1).** Root cause: `T { field: value }` (bare, no `new`)
was never a general expression at all — it was special-cased *only* inside
`let x: T = ...` parsing, where it desugars into `new T()` + one
`x.field = value;` assignment statement per field, spliced into the enclosing
block. That lowering needs a name to hang the assignments on, so it
structurally couldn't apply anywhere else; `parsePrimary` had no branch for
`Identifier '{'` at all, so inside `[...]` the array-literal loop parsed the
bare type name as an ordinary identifier, left `{ ... }` completely
unconsumed, and failed to find the closing `]`. (`new T { field: value }`,
the guide's *canonical* record-literal spelling, was unaffected the whole
time — `parseNew` already builds a self-contained `AST_NEW` with the field
inits attached, no desugaring needed, so it already worked as a general
expression everywhere, arrays included.)

Fix: `parsePrimary` now recognizes `Identifier '{' ... '}'` where the
identifier resolves (via `buildTypeNode`) to a record type, and desugars it
via the *same* `new T()` + field-assign lowering the let-position form
already used (`buildObjectInitDecl`) — but hung on a synthesized temp
(`__aether_lit_N`) instead of a real `let` name, since there's no enclosing
declaration to name here. The synthesized declaration + assignments are
queued on the parser (`pendingObjLits`) and flushed by a new `parseStatement`
wrapper once the statement currently being parsed finishes, spliced in
immediately before it via the existing `i_val==1` `AST_COMPOUND` mechanism
`parseBlock` already flattens (the same splice convention the let-position
form and array-append initializer already rely on) — with an expression-site
reference to the temp substituted at the original position. This is general,
not array-specific: also verified working as a direct function-call argument,
nested inside another record literal's field value, inside an `if` condition,
with multiple hoists in one statement, and inside an unbraced single-
statement `if`-body (recursive `parseStatement`, correctly isolated via a
mark/release pattern on the pending list so a nested statement's hoists don't
leak into an outer one's splice point). Full existing suite
(`tests/run.sh`) passes unchanged; new regression:
`tests/array_record_literal_pass.aether`. Both guides' *Constructing records
and typing bindings* section updated — the old "do not nest record literals
inside a single array literal" guidance was flatly wrong for `new T { ... }`
(always worked) and is now also wrong for the bare form; both guides now show
`[new Point { x: 1, y: 2 }, ...]` as supported. Language version bumped to
`2026-07-19-1` (real parser-accepted-syntax change, not docs-only).

Still open: homogeneous array-literal element-type inference (`let xs = [1, 2, 3];`
still requires the explicit `: Int[]` annotation) is unrelated to this fix and
remains a separate gap.

### `swap` collides with the `swap` PSCAL vm_builtin, false-positive FX-001 — *fixed 2026-07-19-3*
A user-defined `fn swap(...)` with no effectful calls in its body still tripped
`[FX-001] call to 'swap' requires an fx block`, because `swap` is also a real,
effectful PSCAL `vm_builtin` (confirmed via `builtin_info("swap")`) and the
effect-checker matched call targets by name only, with no way to distinguish
"the user's pure function named swap" from "the builtin named swap." Repro:
a plain array-swap helper (`fn swap(xs: Int[], i: Int, j: Int) -> Void { ... }`,
no `fx`, no I/O) called from an unmarked `fn`, outside any `fx` block, still
faulted. Workaround verified pre-fix: rename the function (e.g. `swap_vals`) —
no functional loss, since `swap` was never anything but a name collision.

**Root cause:** `aetherCheckCallNode` (`src/aether/semantic.c`) decided a call's
effectfulness purely from `aetherIsEffectfulBuiltin(canonical, ...)`, a lookup
against the live PSCAL builtin registry keyed only on the call's bare name.
Nothing in that path ever consulted the parser's own declaration table to ask
"did the program itself declare a function called `swap`?" — so a user's
top-level `fn swap` and the builtin `swap` were indistinguishable at the call
site, and the builtin's effect mask always won.

**Fix (`src/aether/ast_parser.c`, `src/aether/parser.h`, `src/aether/semantic.c`):**
added a new parser-side registry, `g_aetherTopLevelFns` (`aetherAstRegisterTopLevelFunction`
/ `aetherAstIsTopLevelUserFunction`), recorded at the same declaration site that
already tracks `@pure` state (`parseFunctionDecl`'s post-parse registration
block). It records the bare name of every `fn` declared where `isMethod` is
false — i.e. NOT inside a `type { ... }` body — the parser's own existing
criterion for "this is a plain top-level function," already used one branch
over to decide whether to mangle the name to `Type.method`. `aetherCheckCallNode`
now checks this registry immediately after the builtin lookup: if the call's
canonical name is both builtin-effectful AND a registered top-level user
function, the builtin verdict is discarded (`isEffectful = 0`) and the call
falls through to the existing `aetherAstLookupFunctionPurity` path instead,
so a pure caller correctly gets "cannot call non-pure function 'swap'" rather
than a builtin-flavored message when that's what actually applies. A same-named
builtin call with no user declaration of that name is untouched — the registry
lookup simply misses and the original builtin check governs FX-001 as before.
Type-body methods deliberately do NOT get this shadowing (mirrors the still-open
method case tracked under *Reserved words collide with member names* above —
`fn print()` inside a `type` body colliding with the `print` builtin remains a
separate, harder problem since a method call site's bare name is ambiguous
between multiple types).

**Verified:** the exact repro now compiles and runs; a bubble-sort-with-swap
end-to-end program (`fn swap(...) -> Int[]` returning the mutated array — arrays
pass by value at every Aether function boundary, including a plain direct call,
so the swap-then-return idiom is required, not just an outer-boundary
workaround) sorts correctly when called from a plain `fn` outside any `fx`
block. A control program that calls the real `swap` builtin with no user
function of that name still correctly faults with `[FX-001] ... requires an fx
block`, confirming the fix is shadowing, not blanket suppression. Full existing
suite (`tests/run.sh`) passes unchanged; new regression fixtures
`tests/swap_shadow_builtin_pass.aether` (the bubble-sort case) and
`tests/swap_builtin_unshadowed_fail.aether` (the still-must-fault control),
wired into `run.sh`. Language version bumped to `2026-07-19-3` (real
effect-checker behavior change, not docs-only).

### Range-loop bound silently accepted a `Bool` expression, causing a single-iteration/hang footgun — *fixed 2026-07-19-2*
`loop i in 0..N && cond { ... }` compiled with **no error** and ran the loop
body once (or zero times) instead of iterating `0..N`, because
`parseLoopRange()`'s upper-bound parse used the full expression grammar
(`parseExpr`), which doesn't stop at `&&`/`||` — the entire `N && cond` tail
was absorbed into the upper bound, producing a `Bool`-typed bound that
silently coerced to `0`/`1` at the `i < HIGH` comparison. Found via a
generated Tic-Tac-Toe example (`loop r in 0..3 && !placed { loop c in 0..3 &&
!placed { ... } } }`, attempting a for-loop-with-early-exit idiom that
doesn't exist in Aether): the inner scan only ever checked cell `(0,0)` on
every pass, `placed` never advanced past the first move, and the outer
`loop !gameOver` ran forever — a genuine hang, not just wrong output, worse
than a compile error because there's nothing to catch it.

Fix (`ast_parser.c`, `parseLoopRange`): both bounds now parse at additive
precedence (`parseAdd`, i.e. `+`/`-`/`*`/`/`/`div`/`mod`/unary/calls/
indexing/parens/if-expressions — everything a numeric bound legitimately
needs) instead of the full ladder, so a trailing `&&`/`||`/comparison is left
unconsumed and correctly falls through to the existing "expected '{' to open
loop body" `SYN-001` error instead of silently type-punning. Added a second,
explicit guard rejecting a `TYPE_BOOLEAN` bound outright (`"loop range bound
must be numeric (Int/Real), not Bool"`) as defense-in-depth for the
still-reachable `0..(N && cond)` explicitly-parenthesized form, where the
parens re-enter the full expression grammar by design and the precedence
restriction alone can't catch it. Verified: legitimate arithmetic/call/paren
bounds (`0..length(xs)`, `(a-1)..(a+b)`, `0..half(10)`) still work; full
existing suite (`tests/run.sh`) passes unchanged; new regression fixture
`tests/loop_range_bool_bound_fail.aether`. Language version bumped to
`2026-07-19-2`. The example itself was fixed to use the *actual* early-exit
idiom (`break`, already supported and documented) instead of the invalid
range-condition hybrid.

### Two stale documentation gaps found alongside the range-loop bug (docs-only, 2026-07-19)
- **`ord`/`chr` (character ↔ code point) were real, working builtins
  (`ord("A")` → `65`, `chr(65)` → `"A"`, confirmed via `builtin_info` and
  direct testing) but appeared in neither guide.** A model reached for
  `int(ch)` instead, assuming it meant "char code" — `int(Text)` is actually
  a numeric-only cast that silently returns `0` for `Text` input (it *does*
  cast `Real`→`Int` and `Bool`→`Int`), so the resulting cipher silently
  no-op'd every character with no error. Added `ord`/`chr` to both guides'
  conversion-helper lists plus a note that `int(Text)` is not a char-code
  read.
- **`println`/`print` do not stringify arrays, and nothing said so.**
  `println("data: ", xs)` compiles and runs but prints the array's internal
  representation (`ARRAY(dims:1, base_type:INT64, elements_at:0x...)`), not
  its elements — no error, just silently wrong output. Hit independently in
  two files from the same generated batch. Documented in both guides'
  *Dynamic arrays* sections with the correct iterate-and-print idiom.
- **The small and large guides both stated inline `if ... else ...`
  expressions are never allowed inside `println(...)` call arguments** — this
  was flatly wrong and contradicted an existing, passing regression fixture
  (`tests/inline_if_call_args_pass.aether`) that has verified the opposite
  for some time. Corrected both guides; the false restriction was likely
  true at some earlier stage of the parser and never updated after it was
  generalized.

### Inline `//` comments on `@pre`/`@post` lines leak into the contract expression — *fixed 2026-06-30-2*
The AST frontend captured the rest of an annotation line as the contract
expression **without stripping a trailing `//` comment**, so the comment text was
parsed as code:
```
@post result >= 1 // Factorial of 0 is 1, otherwise positive
fn calculateFactorial(n: Int) -> Int { ... }
   -> [SCOPE-001] identifier 'Factorial' not in scope.
```
The diagnostic depended on the comment *prose* (an undeclared word errored; one
that resolved compiled silently), not the code. Hit by `gemini-3.1-pro-preview`
(worked around only by deleting the comment).
**Resolved (2026-06-30-2).** `collectPendingAnnotations` (`ast_parser.c`) now
stops the contract-expression capture at the first unquoted `//`, so a trailing
line comment is stripped unconditionally before lowering; a `//` inside a string
literal (e.g. `@post result != "http://none"`) is preserved. The legacy rewriter
fallback's `extractAnnotationExpr` (`translate.c`) got the symmetric fix, so both
frontends agree. Regression: `tests/contract_annotation_comment_pass.aether`.
(Companion to the still-open "malformed `@cost` is silently dropped" gap below.)

### Models reach for `loop x in collection` (foreach) — *idea / decision needed*
`loop v in values { ... }` → `expected '<low>..<high>' in loop range`; only
`loop i in 0..n` exists. Hit by `qwen3-32b`. **Action:** either add a foreach
sugar over arrays, or make the index-only loop (and the array-indexing idiom)
unmissable in the guide. (Related to the no-closures / first-order-`loop`
stance already recorded under *Decided*.)

### Models reach for `Int(...)` / `Real(...)` casts — *idea*
`let limit: Int = Int(sqrt(Real(n)));` → `[SCOPE-001] identifier 'Int' not in
scope`. Aether has no type-name cast functions. Hit by `exaone3.5-32b`.
**Action:** document the real conversion surface (`trunc`/`round`/`floor`/`ceil`
for Real→Int, `realtostr`/`formatfloat` for Real→Text), or add `Int()`/`Real()`
cast builtins. The Math-builtins guide section is the natural home.

### `par` blocks reject non-call statements, surprising models — *idea / clarify*
Models put assignments and `fx`/`sleep` inside `par { ... }`:
`par { sleep(1000); self.state = "red"; fx { ... } }` →
`only direct call statements are allowed inside par blocks`. Hit by
`exaone3.5-32b`. **Action:** the guide's `par` section should state the
call-statements-only rule and show the "wrap work in a `fn`, call it inside `par`"
pattern.

### Effect-discipline slips remain capability-gated — *(known; reinforces existing FX-001 docs)*
`exaone3.5-32b` still emitted `println` outside `fx` inside a loop body
(`[FX-001]`, unrepaired). Consistent with the guided-benchmark finding that weak
models thrash on the coded diagnostic while capable ones self-correct; no new
action beyond the existing FX-001 guidance.

### `@pre`/`@post` predicate operand types aren't checked — array-return contract crashes at runtime — *fixed 2026-06-30-4*
A contract that compares the whole `result` to a scalar, on a function returning a collection, is
not type-checked at compile time and fails at RUNTIME:
```
@post result > 0
fn make(n: Int) -> Int[] { let xs: Int[] = []; loop i in 0..n { xs = xs + [i]; } ret xs; }
   -> Runtime Error: Operands not comparable for operator '>'. Left operand: ARRAY, Right operand: Int
```
Hit by `qwen3-coder-next` (a Sieve of Eratosthenes with `@post result > 0`, via the
scheduler-coordinated sweep).
**Resolved (2026-06-30-4).** The AST frontend now type-checks each contract comparison as it lowers
the guard (`checkContractComparisons` in `ast_parser.c`): a bare `result` in a `@post` resolves to the
function's return-type name (new `currentReturnTypeName` on the parser), every other operand goes
through the existing `inferLetTypeName`, and a comparison (`< > <= >= == !=`) with exactly one array
operand and one scalar operand is rejected with a coded `ANN-001` diagnostic pointing at
`length(result) > 0`. Conservative by design — array-vs-array and un-nameable operands are left alone,
so no benchmark-corpus false positives; scalar contracts and `length(result) > 0` compile unchanged.
Regression: `contract_collection_result_fail.aether` + `contract_collection_length_pass.aether`.
*(reported 2026-06-29, sweep 2.)*

### `loop ... step N` (stepped range) is unsupported — *gap (verified)*
Models reach for a stepped loop; only unit-step `loop i in a..b` exists:
```
loop i in 0..10 step 2 { ... }   -> [compile] expected '{' to open loop body   (parser stops at `step`)
```
Hit by `bytedance/seed-oss-36b` (a prime check striding odd divisors). **Action:** support a `step`
clause on `loop`, or document the workaround (`loop i in 0..n { if i % 2 == 1 { ... } }`) and give a
clearer diagnostic than "expected '{'". *(scheduler-coordinated sweep, 2026-06-29.)*

### `toon_parse` rejects multiple arguments (no println-style concatenation) — *gap (verified)*
Models build the TOON/JSON string with several args the way `println` concatenates, but `toon_parse`
takes ONE string, and the failure is a late runtime message:
```
toon_parse("{\"now\":\"", rawNow, "\"}")   -> [runtime] YyjsonRead expects a single string argument.
```
Hit by `a3b-coder30b-cs-aug2-builtins`. **Action:** either accept + concatenate multiple string args
(println-consistent), or emit a compile-time ARITY error instead of the late `YyjsonRead` runtime one;
document building the string first (`let s: Text = "..." + rawNow + "..."; toon_parse(s)`).
*(scheduler-coordinated sweep, 2026-06-29. Also reconfirmed this sweep: array-of-record literals fail
to parse (seed-oss), and `par` blocks calling user functions can crash silently (qwen3-coder-next) —
**the par crash is fixed 2026-07-01-1**, root-caused to a shared-record data race and now rejected as
`PAR-001`; see the resolved silent-failure entry under **Open ideas**.)*

## Generative pass 2 — after the 2026-07-01 diagnostic/guide fixes

*Re-ran the generative miner against the FIXED compiler (`aether 2026-07-01-1`, gitlink 63ca546)
+ updated guides, 6 models (Ornith-1.0, m5 qwen3-coder-next + devstral, claw2 a3b-coder, gemini-2.5-flash,
gemini-3.1-pro), 32 programs, 23 clean.* **The six fixes held under free-form generation** — none of the
prior high-frequency gaps recurred: reserved-word member collisions now emit a named `SYN-001`,
`@post`-over-array is a compile-time `ANN-001`, mutating methods on inferred `new` compile, and
undefined *methods*/silent AST paths now carry coded diagnostics. What remains is a thinner, lower-severity tier:

### Tuple-destructuring binding + placeholder diagnostic code — *idea (verified)*
Models reach for multi-value destructuring: `let (name, age, score) = parseLine(line);` →
`tuple destructuring target is not a known tuple-return function` — and the diagnostic's `code` is the
literal placeholder **`feature`**, not a real code. **Action:** (a) decide whether to support
destructuring binds from tuple-returning fns (or document the single-return + record alternative), and
(b) give this diagnostic a real code (e.g. `TUP-001`) — a placeholder `code:feature` breaks the
code→guide-section mapping. Hit by `ornith-1.0-35b-nvfp4`.

**Resolved (2026-07-01-2).** Both parts done. (a) The feature is supported as-is: `let (a, b) = f();`
compiles when `f` is a defined top-level tuple-return function (`f -> (Int, Int)`). The diagnostic only
fires for the cases that genuinely cannot destructure (a method, an undefined helper, or a nested
expression), and both guides now document the record/fields alternative for those (return a record and
read its fields). (b) The placeholder `code:"feature"` is now the real `TUP-001` across every tuple
feature-limitation diagnostic on both the AST (`ast_parser.c`) and rewriter (`translate.c`) paths: the
placeholder kind `feature` became the semantic kind `tuple`, mapped to `TUP-001` in
`aetherInferDiagnosticCode`. The three AST-path destructuring diagnostics were raw `aetherDiagf`
`[feature]` calls with no hint; they now route through `reportAetherAstError`, so they also emit a hint
(was `null` in `--diagnostics-json`) and the guide-help pointer, byte-for-byte with the rewriter path.
Locked in by a `--diagnostics-json` `"code":"TUP-001"` + hint assertion in `tests/run.sh`.

### A few compiler errors are still uncoded (should join the coded set) — *gap (verified)*
The recent work coded the worst offenders; these remain raw (no `code`, plain stderr) so models can't
map them to a guide section:
- bare `ret;` in a non-Void fn → `return requires a value` (should be a coded FLOW-style error).
- non-call statement inside `par { ... }` → `only direct call statements are allowed inside par blocks`
  (the 2026-07-01 fix coded the shared-record par *crash* as PAR-001, but not this par-arity rule).
- an edge of undefined-field access emits a raw `Compiler error: Unknown field 'X'` from the backend
  (the clean case is correctly `FIELD-002`). **Action:** route these through the coded-diagnostic path.

**Resolved (2026-07-01-3).** All three now carry a code (default AST path only;
`translate.c` untouched). (1) Bare `ret;` in a non-Void fn is now `FLOW-002`:
`parseRet` (`ast_parser.c`) routes the empty return through `reportAetherAstError`
(kind `function`) instead of a raw `aetherDiagf`, so it emits `[FLOW-002]` + hint +
guide pointer. New code because the fix differs from the `FLOW-001` fallthrough
rule (give the return a value vs add a return). (2) The par-arity rule is now
`PAR-002`: it already went through `reportAetherAstError` (kind `par`) but no `par`
case existed in `aetherInferDiagnosticCode`; added, distinct from the `PAR-001`
shared-record crash. (3) The undefined-field edge was subtler than the note
assumed. The cited `state.state` case (and ~18 field-access shapes probed:
self/nested/array-element/param/inferred receivers) is already `FIELD-002` on
`2026-07-01-2` because rea's semantic pass (`resolveExprClass`) catches a
*resolvable* receiver first, so the devstral observation predated the current
binary. The raw codegen error (`compiler.c:5951`, bracket-less `Compiler error:
Unknown field 'X'`) is only reachable when the receiver's record type is
*unresolvable* (e.g. a method call on a variable of an undefined type,
`let s: Nope = new Nope(); s.go();`), which slips past semantics. Rather than teach
the shared backend about codes, the `--diagnostics-json` collector
(`rea/src/rea/main.c`, `extractDiagnosticCode`) now backfills the code for any
bracket-less line via the registered frontend's `reaFrontendInferDiagnosticCode`,
so that backend string carries `FIELD-002` (and any other uncoded-but-recognized
backend message gets its code) instead of `code: null`. This is a class fix, not a
single-site patch. Regressions: `tests/{function_empty_return_fail,
backend_unknown_field_coded_fail}.aether` + a `PAR-002` diagnostics-json assertion
on the existing `par_fail_non_call` fixture. Both guides gained `FLOW-002`/`PAR-002`
rows.

*Note (unchanged, separately tracked):* every coded diagnostic still double-emits
in `--diagnostics-json` because the `help: see <CODE>` line is parsed as its own
`code:null` entry (the papercut recorded above under the `help:` gap). The backstop
does not touch it (a `help:` line matches no inference pattern, so it stays
uncoded).

### `--diagnostics-json` emits the `help:` guide pointer as a spurious extra diagnostic — *gap (verified)*
Every coded diagnostic's `help: see <CODE> in the Aether guide (...)` line (from `aetherReportGuideHelp`)
is captured off stderr by `collectDiagnosticsFromText` (`rea/src/rea/main.c`) and, because it is not a
`hint: ` line, parsed as its own diagnostic object: `code:null`, `message:"help: see <CODE>..."`. So a
single error yields **two** JSON entries, which inflates any consumer that counts array length as an
error count. This is not tuple-specific (it affects FX-001, SYN-001, TUP-001, all of them); noticed
while fixing TUP-001. **Action:** either attach the `help:` text to the preceding diagnostic (like
`hint:`) or skip `help:`-prefixed lines in `collectDiagnosticsFromText`. Low severity (the human `hint`
already carries the actionable guidance), but the doubled array is a real papercut for JSON consumers.

*(Harness accuracy fix, not a language gap. RESOLVED 2026-07-01: the idea-miner used to classify a runtime
failure whose message lands on STDOUT (e.g. `Aether @post failed in f` from a legitimately-violated contract)
as a "silent" failure, because it only inspected stderr + diagnostics. `analyze_failure` in
`tools/aether_idea_miner.py` now also scans stdout for the known runtime-error prefixes (`Aether @post failed`,
`Aether @pre failed`, `Runtime Error`, `Compiler error`) and fingerprints them as runtime rather than silent;
covered by `Tests/aether_doc_bench/test_miner_offline.py`.)*

### Models write `fn m(self: T)` free functions instead of methods — *idea (verified, 2 models)*
Folding GLM-5-Turbo/5.2 into pass 2 (both very clean: 9/10 programs compiled): the one gap was models
defining a *free-standing* function with an explicit `self` parameter and referencing `self` in a
contract — `@pre length(self.data) < self.capacity  fn push_safe(self: Stack, val: Int) -> Void` →
`[SCOPE-001] identifier 'self' not in scope`. Verified: a PROPER method (inside the `type` block,
implicit `self`) with `@pre self.v >= 0` compiles+runs fine; only the explicit-`self`-param free-function
form fails. Hit by `glm-5.2` and (pass 2) `mistralai/devstral-small-2-2512`. **Action:** guide note in
the method/constructor section — methods live INSIDE the `type` block with implicit `self`; do not write
`fn m(self: T)` free functions; `@pre`/`@post` on a proper method may reference `self`.

**Resolved (docs-only, 2026-07-01).** Both guides now state this in the `type`/method section. The long
guide (`aether_for_llms_and_others.md`) gains implicit-`self` bullets, a WRONG/RIGHT `[SCOPE-001]` contrast,
and a `@pre`/`@post`-may-reference-`self.field` note in *Purity and contracts*; the concise guide gains a
terse implicit-`self` bullet. Re-verified on `aether 2026-07-01-3`: in-`type` method with `@pre self.v >= 0`
compiles+runs; the free-standing `fn get(self: C)` + `@pre self.v` form fails `[SCOPE-001]`; extension
methods without a `self`-referencing contract still work. No `VERSION` bump (no language change).

## Generative pass 3 — 2026-07-01 (after the diagnostic/guide fixes + field-default work queued)
10-model cohort (Ornith-1.0, claw2 trained a3b/mistral, m5 qwen3-coder/devstral/olmo3-think, 2× Gemini,
2× GLM), 5 free-form programs each, T'Ra-routed. 41/59 (69%) compiled+ran; the capable models were clean
(qwen3-coder 5/5, both Gemini 5/5, both GLM 4/5, Ornith 14/18). The previously-coded diagnostics all fired
correctly (`FX-001`, `NAME-001`, `PAR-001`, `SCOPE-001`), i.e. no regressions from the recent fixes. The
one dominant failure cluster — a3b's 0/5 — was **record field defaults** (`value: Int = 0`), already queued
as its own piece of work; the remaining genuinely-new, verified findings are below.

### Ranged `loop` accepts no `step` clause — *idea (verified)*
`mistralai/devstral` writing a prime sieve reached for a strided range in the inner loop:
`loop j in i*i..n step i { ... }` → `L?: expected '{' to open loop body`. The `..` range itself parses
fine inside a loop (`loop j in 0..10 { }` compiles and prints `0..9`); the parser gets *past* the range
and chokes on `step`. This is the idiomatic strided range (Python `range(a, b, step)`, Rust `.step_by`).
**Action:** accept an optional `step <expr>` clause on the ranged `loop ... in a..b` form and lower it to
the existing loop with the given increment. Bounded — it stays inside the `loop`, where ranges already
live, and needs no first-class range/`Range` value type.

### Word-operators `not` / `and` / `or` misreport as undefined identifiers — *idea (verified)*
Models reach for Pascal/Python-style boolean word-operators: `allenai/olmo-3-32b-think` wrote
`if not toon_is_int(node)` three times, and `a and b` / `a or b` fail the same way. Each yields a
*misleading* `[SCOPE-001] identifier 'not' not in scope` — the lexer treats the keyword as an undefined
variable, so the diagnostic sends the model hunting for a missing binding instead of telling it to use the
C-style operator. `!b`, `a && b`, `a || b` all work. **Action:** cheapest win is a targeted diagnostic —
when `not`/`and`/`or` appear in operator position, emit a `SYN-*` (or dedicated code) saying "Aether uses
C-style boolean operators `!` / `&&` / `||`", not `SCOPE-001`. Optionally alias them outright (they are
common enough to consider), but at minimum stop misclassifying them as scope errors.

### Integer division (`div` / `//`) is undiscoverable; `/`-then-`%` gives a cryptic runtime error — *gap (verified)*
`openrouter_glm-5.2` (Rule-110 cellular automaton): `ret (rule / power_of_two(index)) % 2 == 1;` fails at
runtime with `Runtime Error: Operands for 'mod' must be integers. Got REAL and INTEGER`, because `/` is
*always* real division. This is **not** a missing feature — `7 div 2` and `7 // 2` both already return `3`.
The gap is discoverability + diagnostic quality: models default to `/` then `%`, and the runtime error names
the type mismatch without pointing at the integer-division operators. **Action:** (a) document `div` and
`//` for integer division prominently in the guide's arithmetic/operator section (and note that `/` yields
`Real`); (b) make the `mod`-on-`Real` runtime error hint "use `div` or `//` for integer division, or
convert the operands to `Int`." Same `/`-is-real friction as the earlier collatz `n/2` Real→Int coercion
fix (pscal-core `da08d77`), surfaced at `%` this time.

**Triaged, not curated (pass 3).** The **range operator *outside* a loop** (`..` as a first-class value)
was considered and *declined*: `..` inside a loop already works, the sole out-of-loop occurrence was a
single ambiguous `ornith` line with no captured intent, and making `..` a standalone value would pull in a
whole `Range` type (lazy vs materialized, inclusive/exclusive, indexable) for near-zero demand — the real,
bounded want is the `step` clause above. **Record field defaults** (`value: Int = 0`, 3× a3b) are being
implemented separately. A missing `join(sep, arr)` string builtin (1×) is a minor wishlist item. One
generation artifact (a literal `<<<SOURCE>>>` template marker echoed into a program) is not a language gap.

## Full design/implementation review — 2026-07-01

*Findings from a full code review of the Aether frontend (ast_parser.c, ast_prepasses.c, semantic.c,
diagnostics.c, tests/, examples/, translate.c status). Items marked (verified) were reproduced on the
current local build; the rest are code-read findings with file:line cites.*

### Alias prepass rewrites inside string literals — *FIXED 2026-07-01-5*
`applyJsonAliasesToLine` (`ast_prepasses.c` ~2959) does not skip string literals, so any user string
containing an aliased builtin name followed by `(` is rewritten: `println("call sleep(5) now");` prints
`call delay(5) now`. Stage 1 (`rewriteAetherBuiltinAliases`) skips strings/comments correctly; stage 2
forgot to. Silent wrong OUTPUT from a correct program — worst-severity class for the exact-stdout
benchmark. Fix: share stage 1's string/comment skipper; add a regression fixture.

### The fx fence is line-textual, with a verified escape and a false positive — *FIXED 2026-07-01-5 (fx/purity now AST-based)*
`semantic.c` enforces FX-001 by scanning physical source lines, not the AST (the parser erases `fx`
before semantics, `ast_parser.c` ~3734). Two consequences, both verified:
- **Escape:** an effectful call split across lines (`println` on one line, `("...")` on the next)
  compiles and runs with no `fx` anywhere — the same-line `(` requirement (~2208) misses it.
- **False positive:** `fx` with `{` on the next line is accepted by the parser but the text scan never
  sets `pendingFx` (~2211), so the legal program is rejected with a spurious FX-001.
Related: `@pure` is enforced only against effectful *calls*; a `@pure` fn containing an `fx {}` block
compiles (the guide says `@pure` functions may not contain `fx`). Durable fix: mark fx regions on the
AST (a flag on the block node) and run the effect/purity check over the AST, not text. Also
single-variable `currentPureFunctionName` (~2106) is not a stack (nested fns clear tracking), and the
scope frame stack `stack[1024]` (~2108) increments depth unconditionally (~2177) → OOB read/write past
1024 nesting depth.

### Parser inference state is program-global, not scoped — *FIXED 2026-07-01-6 (function-scoped tables)*
`ast_parser.c` keeps one flat binding table for the whole parse (~5470-5482), never pushed/popped per
function: a `let x` in fn A leaks into fn B's inference (last-write-wins), affecting inferred types,
method mangling, and PAR-001 verdicts when names collide across functions. `funcReturns` is likewise
keyed on bare names. Same flat-table pattern in `semantic.c` (`addScalarBinding` overwrites, ~552).
Fix: scope-aware tables (push/pop on fn entry/exit).

### Tuple returns lower to globals — non-reentrant — *FIXED 2026-07-04-2 (record-by-value lowering)*
`ret (a, b)` used to lower each slot to a global `__aether_tuple_N_itemK`, so calls that raced or
nested on the same slots silently corrupted each other's results. A 2026-07-04-1 pass added
compile-time *rejection* of all three vectors (direct self-recursion, indirect recursion
`a() -> b() -> a()`, and two `par` branches calling the same tuple-returning function) via a
call-graph cycle detector (TUP-001) and a new PAR-003 check — useful defense-in-depth, but still
rejection: valid, idiomatic code (a recursive accumulator returning two values) simply could not be
written as a tuple return.

That same 2026-07-04-1 investigation confirmed the PSCAL VM already supports fully reentrant
record-by-value returns (deep-copy on return, `returnFromCall`/`copyRecord` in pscal-core). This has
now landed: tuple returns synthesize a hidden record type per signature (`__AetherTuple<id>`, fields
`item0..itemN-1`, see `buildSyntheticTupleRecordType`), `ret (a,b)` lowers to record construction +
return through the existing `buildReturnObjectInit`-derived path (`buildTempRecordReturn`), and
`let (a,b) = f(x)` lowers to a temp record var + ordinary field access, instead of raw-slot-global
reads. Each call — recursive or concurrent — gets its own independent VM-deep-copied result, making
all three vectors **structurally impossible** rather than merely rejected. TUP-001's cycle-check and
PAR-003 were removed as a result (the defect class they existed to catch no longer exists; the
"destructuring target is not a known tuple-return function" / arity-mismatch checks are unrelated and
still emit `TUP-001`). Tests: `tuple_recursion_pass.aether`, `tuple_indirect_recursion_pass.aether`,
`par_shared_tuple_call_pass.aether` (all previously `_fail` fixtures under the old rejection).

One real bug surfaced and fixed along the way: `parseExprFromText` (used by `@pre`/`@post` contract
guards) only stamped the *root* node's source line, not descendants — a detached sub-lexer parsing
guard text starts its own line counter at 1, so a reference to the per-`ret`-site temp record deep in
the guard carried a fake early line number. The compiler's declared-after-use heuristic
(`CompilerLocal.decl_node`'s line vs. the reference's line, in `compileRValue`'s `AST_VARIABLE` case)
then wrongly treated the reference as out-of-scope and fell back to a global lookup ("Undefined global
variable ..."). `result` was never affected because it's registered without a `decl_node`, so no
`@post` before this had ever referenced anything else. Fixed by recursively stamping every node's line
(`aetherStampTreeLine`), a general correctness fix beyond just the tuple case.

Still open: tuple-fn registration is a raw-text scan that only recognizes column-0 `fn` lines
(~5960s in `aetherRegisterTupleGlobals`), so an indented tuple fn degrades.

### Parser silently tolerates missing closing delimiters — *FIXED 2026-07-01-7*
`parseBlock` (~3846), `parseArgListEx` (~1271), `parsePostfix` (~1375) all "consume the closer if
present, else continue": an unclosed fn body / arg list / index at EOF parses without error. And a
`parseStatement` returning NULL breaks the block loop *without* setting hadError (~3830), so mid-block
garbage can silently truncate a body. Both defeat the 2026-07-01 silent-failure backstop from the other
side (accepted-but-wrong rather than rejected-but-silent). Fix: require closers; error on NULL stmt.

### Fixed-size array suffix `[N]` half-consumed — corrupts the token stream — *FIXED 2026-07-01-7*
The type parser consumes `[` for `Int[3]` then abandons the path (~865-876), leaving the stream
misaligned and producing an unrelated downstream error. Same pattern: `parseWriteArg` (~1221) swallows
`:` when no NUMBER follows. Emit a real diagnostic ("fixed-size arrays are not supported; use Int[]").

### Diagnostic code inference is substring-matching on message text — *FIXED 2026-07-01-6 (explicit codes at all sites; inference is backstop-only)*
`aetherInferDiagnosticCode` (`diagnostics.c:59-129`) maps messages to codes by strstr on English
wording — the wording is load-bearing (a copyedit silently changes/loses the code), and the
`" first argument"` pattern maps any message containing it to TOON-001. Meanwhile the scalar/opaque
assignment errors ("cannot assign Bool binding...", "cannot assign ToonDoc handle...") match nothing and
emit **no code at all** — exactly the TOON/type family the design emphasizes. ~30 raw `aetherDiagf`
sites also bypass the coded format (no path, no code, no help pointer). Fix: pass an explicit code enum
at each emission site; keep inference only as a backstop for backend strings.

### toon_* helper name tables are duplicated 3-4× — *gap (drift risk)*
The helper arg/return-kind tables live in `semantic.c` (~1216, ~1310), `ast_prepasses.c` (~2447), and
`translate.c` — already drifting (`toon_null` has a return-kind but no arg-kind entry, semantic.c
~1302). Single-source the table (one header, or generate from the pscal-core metadata array).

### Legacy rewriter fallback is frozen and diverging — *RESOLVED 2026-07-01-5: deleted*
Since the P7 cutover, `translate.c` has had no substantive updates (CHANGELOG 2026-07-01-3 says so
explicitly) while the AST path gained FIELD-003 field defaults, FLOW-002/PAR-002, ANN-001 collection
checks, and the inferred-`new` fix. Nothing in `tests/run.sh` sets `AETHER_PARSER`, so the advertised
"runtime-reversible fallback" is untested and already parses an older language. Either (a) add a cheap
`AETHER_PARSER=rewriter` smoke lap over a curated fixture subset (excluding post-cutover features) to
keep the fallback honestly characterized, or (b) schedule its retirement per the roadmap clause. The
current state (unmaintained but advertised as reversible) is the worst of both.

### Test-suite shape: one coarse CTest, fail-fast, examples not executed — *partially done 2026-07-01 (aether_examples compile lap added; run.sh granularity still open)*
All ~149 assertions run inside a single `add_test` via the ~1850-line `tests/run.sh` with
`set -e` fail-fast: one failure hides everything downstream and CTest granularity is 1, not 127.
Only `showcase/agent_report` is CI-executed; the ~55 `examples/base` programs can rot silently (two
already do — the tracked `ai_helpers`/`effects_contracts` gap). TOON is ~1/3 of fixtures while core
control flow (nested if/elif, recursion) is thin. Ideas: per-fixture CTest registration or a
keep-going mode + summary; an examples-compile lap in CI; a couple of core-control-flow fixtures.

### Misc code-quality notes (parser) — *idea*
Giant functions (`parseFnDecl` ~500 lines, `parseLetDeclAfterKeyword` ~320); the object-literal
expansion duplicated 3×; the lexer save/restore backtracking block duplicated 4× (copies `ReaLexer` by
value — fragile if the lexer grows heap state); unchecked `realloc` in the tuple-@post rewriter (~4417,
~4425); temp names keyed by source line (`__aether_obj_%d`, ~2600) collide when two such constructs
share a line; `bindingTableSet` casts away const at ~8 sites; contract expressions are captured as raw
line text and re-parsed (cannot span lines; inner nodes keep detached-buffer line numbers,
`parseExprFromText` ~2109 restamps only the root).

## Mined from historical bench logs — 2026-07-01 (post-hardening retest)

*Source: `Tests/aether_specialization/bench_failure_mining_2026-07-01.md` (umbrella).
817 final-state failures from the 2026-06-27/28 guided + trained boards and 4
idea-miner runs, each retested on aether 2026-07-01-8. 584/587 compile failures
still reproduce (genuine prior signal); the big fixed-since class is the silent
rc=1 exits (188/190 now emit coded diagnostics). Wrong-prior drills are being
added to the corpus; the entries below are the LANGUAGE-side candidates.*

### Arrays are value copies; records are pointer-backed — in-place mutation silently no-ops — *gap (top finding, 10 families)*
A function that sorts/mutates an array parameter compiles and runs but the
caller sees the ORIGINAL array (arrays pass by value; records by reference).
17 identical unsorted quick_sort outputs across 10 families including gpt-oss,
gemini, and the trained boards — the single largest compiled-but-wrong class.
Options: (a) make array params reference-backed like records (consistency, but
a semantics change across the suite); (b) reject mutation of array params
without a return (frontend analysis); (c) corpus-only (return-the-array idiom
drill, being added). The asymmetry itself is the trap; decide deliberately.

### `new Int[](5)` compiles then crashes the VM — *gap (verified)*
Sized-array allocation syntax is accepted by the frontend and dies at runtime.
Either support it or reject it at parse time with the append-loop hint.

### `xs + ys` array concat compiles, fails at runtime — *gap (verified, 4 families)*
`ARRAY + ARRAY` passes the frontend and errors in the VM. Reject at compile
time with a loop-append hint, or implement concat (models expect it).

### No `else if` in if-EXPRESSION position — *idea (28x, 8 families)*
`let g: Text = if x > 8 { "A" } else if x > 6 { "B" } else { "C" };` fails;
statement-position `else if` works. Either support the chain in expression
position or emit a targeted diagnostic (nest `else { if ... }`).

### Array slicing `arr[a..b]` — *idea (~30x, 13 families)*
Most-reached-for missing collection op. Related: tuple-element arrays. Weigh
against the no-Range-type decision (2026-07-01 pass-3 triage) — a slice
SUGAR inside indexing brackets need not introduce a first-class range value.

### Misleading diagnostics to sharpen — *idea*
~~1-based string indexing trips ~25 runtime errors (0-based prior)~~ —
**resolved 2026-07-25: `Text` is now 0-based**, uniform with arrays, along with
`copy`'s `start` and `pos` (which returns `-1` when absent). See
`docs/text_zero_based_migration_plan.md`. The remaining items stand:
nested-fn declarations get a misleading
diagnostic; `match` statements (8 families) could get a targeted "use if"
SYN-001 the way not/and/or word-ops are being handled.


---

## Mined from generative idea-mining — 2026-07-19

*Auto-generated by `tools/aether_idea_miner.py` (the no-oracle, free-form sibling of `aether_doc_bench.py`): 2 models freely wrote 9 Aether programs against `aether` 2026-07-15-2; 8 compiled+ran. Findings below are where models reached for something missing or tripped on an existing rule, ranked by distinct-model breadth. Curate into the sections above as they are triaged.*

_No findings met the breadth threshold this run._

---

## Mined from generative idea-mining — 2026-07-19

*Auto-generated by `tools/aether_idea_miner.py` (the no-oracle, free-form sibling of `aether_doc_bench.py`): 14 models freely wrote 237 Aether programs against `aether` 2026-07-15-2; 228 compiled+ran. Findings below are where models reached for something missing or tripped on an existing rule, ranked by distinct-model breadth. Curate into the sections above as they are triaged.*

### Tripped on `SYN-001` — 3 model(s) — *idea*
`SYN-001` (Use Aether keywords: `fn`, `let`, `const`, `ret`, `if`, `loop`,) · hit by 3 distinct model(s), 5 occurrence(s), 5 not rescued by repair.

Compiler diagnostic: `Aether parser error: Expected ']' to close array literal.`

Minimal example (model `qwen/qwen3-coder-30b`, intent: Generates a formatted report of sales data with filtering, ranking, and summary statistics using pure helpers and effectful output.):

```
   fn main() -> Void {
       let sales: Sale[] = [
>>         Sale { product: "Widget A", amount: 150.0, region: "North" },
           Sale { product: "Gadget B", amount: 200.0, region: "South" },
           Sale { product: "Tool C", amount: 100.0, region: "East" }
```

**Suggested action:** Recurring trip-up on an existing rule — candidate **guide clarification** (make the rule harder to miss) or a friendlier diagnostic.

Models: ibm/granite-4-h-tiny, qwen/qwen3-coder-30b, qwen3.5-9b-mlx.

### Reached for `length` — not in scope (does not exist) — 1 model(s) — *idea*
`SCOPE-001` (A name must be declared before use and still be in scope at) · hit by 1 distinct model(s), 2 occurrence(s), 2 not rescued by repair.

Compiler diagnostic: `identifier 'length' not in scope.`

Minimal example (model `qwen3.5-9b-mlx`, intent: Prime number finder using trial division with output formatting discipline for exact decimal precision when computing percentages):

```
           }
           
>>         loop i in 0..primes.length {
               let p: Int = primes[i];
               fx {
```

**Suggested action:** Candidate **language gap**: add the builtin/construct, or add a guide entry steering models to the existing equivalent. Repair did not rescue it.

Models: qwen3.5-9b-mlx.

---

## Mined from generative idea-mining — 2026-07-19

*Auto-generated by `tools/aether_idea_miner.py` (the no-oracle, free-form sibling of `aether_doc_bench.py`): 2 models freely wrote 6 Aether programs against `aether` 2026-07-19-4; 6 compiled+ran. Findings below are where models reached for something missing or tripped on an existing rule, ranked by distinct-model breadth. Curate into the sections above as they are triaged.*

### Tripped on `SYN-001` — 2 model(s) — *idea*
`SYN-001` (Use Aether keywords: `fn`, `let`, `const`, `ret`, `if`, `loop`,) · hit by 2 distinct model(s), 2 occurrence(s), 0 not rescued by repair.

Compiler diagnostic: `Aether parser error: unexpected token in block; expected a statement.`

Minimal example (model `gemini-2.5-flash`, intent: Parses a TOON array of tasks, calculates total estimated hours for 'active' tasks, and prints a summary.):

```
           self.totalTasks = self.totalTasks + 1;
           if status == "active" {
>>             self.activeTasks = self.activeTasks +
```

**Suggested action:** Recurring trip-up on an existing rule — candidate **guide clarification** (make the rule harder to miss) or a friendlier diagnostic.

Models: gemini-2.5-flash, gemini-2.5-flash-lite.

---

## Mined from generative idea-mining — 2026-07-19

*Auto-generated by `tools/aether_idea_miner.py` (the no-oracle, free-form sibling of `aether_doc_bench.py`): 5 models freely wrote 25 Aether programs against `aether` 2026-07-19-4; 16 compiled+ran. Findings below are where models reached for something missing or tripped on an existing rule, ranked by distinct-model breadth. Curate into the sections above as they are triaged.*

### Tripped on `FX-001` — 2 model(s) — *idea*
`FX-001` (Every effectful builtin must be inside `fx { ... }`: output) · hit by 2 distinct model(s), 2 occurrence(s), 2 not rescued by repair.

Compiler diagnostic: `Aether effect error: call to 'paramcount' requires an fx block.`

Minimal example (model `qwen3.5-9b-mlx`, intent: JSON report parsing with TOON showing nested key extraction and formatted output generation):

```
       }
   
>>     let doc: ToonDoc = toon_parse_file("sample.json");
   
       fx {
```

**Suggested action:** Recurring trip-up on an existing rule — candidate **guide clarification** (make the rule harder to miss) or a friendlier diagnostic.

Models: ibm/granite-4-h-tiny, qwen3.5-9b-mlx.

### Reached for `file` — not in scope (does not exist) — 1 model(s) — *idea*
`SCOPE-001` (A name must be declared before use and still be in scope at) · hit by 1 distinct model(s), 1 occurrence(s), 1 not rescued by repair.

Compiler diagnostic: `identifier 'file' not in scope.`

Minimal example (model `ibm/granite-4-h-tiny`, intent: Create a directory, write "Hello from Aether" to a file inside it, and then read the file back within an effect block.):

```
       fx {
           mkdir(dirName);
>>         assign(file, dirName + "/greeting.txt");
           rewrite(file);
           writeln(file, "Hello from Aether");
```

**Suggested action:** Candidate **language gap**: add the builtin/construct, or add a guide entry steering models to the existing equivalent. Repair did not rescue it.

Models: ibm/granite-4-h-tiny.

### Reached for `toon_get_text_value` — not in scope (does not exist) — 1 model(s) — *idea*
`SCOPE-001` (A name must be declared before use and still be in scope at) · hit by 1 distinct model(s), 1 occurrence(s), 1 not rescued by repair.

Compiler diagnostic: `identifier 'toon_get_text_value' not in scope.`

Minimal example (model `ibm/granite-4-h-tiny`, intent: Generate a random password of length 12 using lowercase letters and digits, print it within an effect block.):

```
           loop i in 0..passLen {
               let randIndex: Int = random(0, charset.len - 1);
>>             let char: Text = toon_get_text_value(charset)[randIndex];
               pass = pass + char;
           }
```

**Suggested action:** Candidate **language gap**: add the builtin/construct, or add a guide entry steering models to the existing equivalent. Repair did not rescue it.

Models: ibm/granite-4-h-tiny.

---

## Mined from generative idea-mining — 2026-07-19

*Auto-generated by `tools/aether_idea_miner.py` (the no-oracle, free-form sibling of `aether_doc_bench.py`): 2 models freely wrote 5 Aether programs against `aether` 2026-07-19-4; 3 compiled+ran. Findings below are where models reached for something missing or tripped on an existing rule, ranked by distinct-model breadth. Curate into the sections above as they are triaged.*

_No findings met the breadth threshold this run._

---

## Mined from generative idea-mining — 2026-07-19

*Auto-generated by `tools/aether_idea_miner.py` (the no-oracle, free-form sibling of `aether_doc_bench.py`): 10 models freely wrote 10 Aether programs against `aether` 2026-07-19-6; 7 compiled+ran. Findings below are where models reached for something missing or tripped on an existing rule, ranked by distinct-model breadth. Curate into the sections above as they are triaged.*

### Tripped on `SYN-001` — 5 model(s) — *idea*
`SYN-001` (Use Aether keywords: `fn`, `let`, `const`, `ret`, `if`, `loop`,) · hit by 5 distinct model(s), 5 occurrence(s), 3 not rescued by repair.

Compiler diagnostic: `Aether parser error: expected '}' to close block (opened at line 1).`

Minimal example (model `gemini-3-flash-preview`, intent: Implementing the Sieve of Eratosthenes to find all prime numbers up to a limit using dynamic array mutation.):

```
       
       if Limit >= 0 { primes[0] = false; }
>>     if
```

**Suggested action:** Recurring trip-up on an existing rule — candidate **guide clarification** (make the rule harder to miss) or a friendlier diagnostic.

Models: gemini-3-flash-preview, gemini-3.1-pro-preview, gemini-3.5-flash, gemma-4-26b-a4b-it, gemma-4-31b-it.

---

## Mined from generative idea-mining — 2026-07-19

*Auto-generated by `tools/aether_idea_miner.py` (the no-oracle, free-form sibling of `aether_doc_bench.py`): 2 models freely wrote 5 Aether programs against `aether` 2026-07-19-9; 4 compiled+ran. Findings below are where models reached for something missing or tripped on an existing rule, ranked by distinct-model breadth. Curate into the sections above as they are triaged.*

_No findings met the breadth threshold this run._

---

## Mined from generative idea-mining — 2026-07-19

*Auto-generated by `tools/aether_idea_miner.py` (the no-oracle, free-form sibling of `aether_doc_bench.py`): 9 models freely wrote 16 Aether programs against `aether` 2026-07-19-9; 11 compiled+ran. Findings below are where models reached for something missing or tripped on an existing rule, ranked by distinct-model breadth. Curate into the sections above as they are triaged.*

### Tripped on `SYN-001` — 6 model(s) — *idea*
`SYN-001` (Use Aether keywords: `fn`, `let`, `const`, `ret`, `if`, `loop`,) · hit by 6 distinct model(s), 6 occurrence(s), 3 not rescued by repair.

Compiler diagnostic: `Aether parser error: expected ')' to close parenthesized expression (opened at line 16).`

Minimal example (model `gemini-2.5-flash`, intent: This program calculates the factorial of several non-negative integers using a pure function with pre/post conditions.):

```
       loop i in 1..n+1 {
           res = res * i;
>>     }
```

**Suggested action:** Recurring trip-up on an existing rule — candidate **guide clarification** (make the rule harder to miss) or a friendlier diagnostic.

Models: gemini-2.5-flash, gemini-2.5-flash-lite, gemini-3-flash-preview, gemini-3.1-flash-lite, gemini-3.1-pro-preview, gemini-3.5-flash.

---

## Mined from generative idea-mining — 2026-07-19

*Auto-generated by `tools/aether_idea_miner.py` (the no-oracle, free-form sibling of `aether_doc_bench.py`): 5 models freely wrote 37 Aether programs against `aether` 2026-07-19-9; 24 compiled+ran. Findings below are where models reached for something missing or tripped on an existing rule, ranked by distinct-model breadth. Curate into the sections above as they are triaged.*

### Tripped on `SYN-001` — 2 model(s) — *idea*
`SYN-001` (Use Aether keywords: `fn`, `let`, `const`, `ret`, `if`, `loop`,) · hit by 2 distinct model(s), 6 occurrence(s), 6 not rescued by repair.

Compiler diagnostic: `Aether parser error: expected ')' to close parenthesized expression (opened at line 13).`

Minimal example (model `qwen3.5-9b-mlx`, intent: Prime number finder with @pure helpers and @pre/@post contracts demonstrating design-by-contract discipline):

```
   @pure
   fn isEven(n: Int) -> Bool {
>>     @pre n >= 0;
       ret n % 2 == 0;
   }
```

**Suggested action:** Recurring trip-up on an existing rule — candidate **guide clarification** (make the rule harder to miss) or a friendlier diagnostic.

Models: ibm/granite-4-h-tiny, qwen3.5-9b-mlx.

### Reached for `buckets` — not in scope (does not exist) — 1 model(s) — *idea*
`SCOPE-001` (A name must be declared before use and still be in scope at) · hit by 1 distinct model(s), 1 occurrence(s), 1 not rescued by repair.

Compiler diagnostic: `identifier 'buckets' not in scope.`

Minimal example (model `qwen3.5-9b-mlx`, intent: Simple hash table implementation with collision handling demonstrating records, methods inside type, and fx-scoped I/O):

```
           let bucketIndex: Int = hash(key) % self.capacity;
           
>>         if buckets[bucketIndex] == 0 {
               buckets[bucketIndex] = value;
           } else {
```

**Suggested action:** Candidate **language gap**: add the builtin/construct, or add a guide entry steering models to the existing equivalent. Repair did not rescue it.

Models: qwen3.5-9b-mlx.

---

## Mined from generative idea-mining — 2026-07-22

*Auto-generated by `tools/aether_idea_miner.py` (the no-oracle, free-form sibling of `aether_doc_bench.py`): 14 models freely wrote 50 Aether programs against `aether` 2026-07-20-1; 22 compiled+ran. Findings below are where models reached for something missing or tripped on an existing rule, ranked by distinct-model breadth. Curate into the sections above as they are triaged.*

### Tripped on `SYN-001` — 3 model(s) — *idea*
`SYN-001` (Use Aether keywords: `fn`, `let`, `const`, `ret`, `if`, `loop`,) · hit by 3 distinct model(s), 16 occurrence(s), 16 not rescued by repair.

Compiler diagnostic: `Aether declaration parser error: 'word' is a reserved type name and cannot be used as a field name.`

Minimal example (model `qwen/qwen3-coder-30b`, intent: Demonstrates a non-trivial algorithm using Aether's built-in math functions to compute Fibonacci numbers with memoization, showcasing both recursive and iterative approaches alongside a pure contract-verified helper.):

```
   
   fn main() -> Void {
>>     let memo: Memo = new Memo { cache: [0; 20], size: 20 };
       let result1: Int = memo.fib(10);
       let result2: Int = fibIter(10);
```

**Suggested action:** Recurring trip-up on an existing rule — candidate **guide clarification** (make the rule harder to miss) or a friendlier diagnostic.

Models: ibm/granite-4-h-tiny, qwen/qwen3-coder-30b, qwen3.5-9b-mlx.

### Reached for `__aether_slice_0` — not in scope (does not exist) — 1 model(s) — *idea*
`SCOPE-001` (A name must be declared before use and still be in scope at) · hit by 1 distinct model(s), 1 occurrence(s), 1 not rescued by repair.

Compiler diagnostic: `identifier '__aether_slice_0' not in scope.`

Minimal example (model `qwen/qwen3-coder-30b`, intent: Implements a simple stack data structure with methods that show how Aether handles record fields, method calls, and pure contracts without the need for a constructor.):

```
           }
           let last: Int = self.data[len - 1];
>>         self.data = self.data[0..len-1];
           ret last;
       }
```

**Suggested action:** Candidate **language gap**: add the builtin/construct, or add a guide entry steering models to the existing equivalent. Repair did not rescue it.

Models: qwen/qwen3-coder-30b.

### Reached for `color` — not in scope (does not exist) — 1 model(s) — *idea*
`SCOPE-001` (A name must be declared before use and still be in scope at) · hit by 1 distinct model(s), 1 occurrence(s), 1 not rescued by repair.

Compiler diagnostic: `identifier 'color' not in scope.`

Minimal example (model `ibm/granite-4-h-tiny`, intent: Simple state machine with @pre/@post contracts.):

```
   
       fn next() -> Text {
>>         if color == "red" {
               ret "green";
           }
```

**Suggested action:** Candidate **language gap**: add the builtin/construct, or add a guide entry steering models to the existing equivalent. Repair did not rescue it.

Models: ibm/granite-4-h-tiny.

---

## Coded diagnostic: 1-D array indexed as 2-D (found in cs-aug20 eval, 2026-07-26)

`cs_lcs` fails across models with a generic runtime error. The generated shape is:

```aether
let dp: Int[] = [];                       // declared 1-D
loop i in 0..(n + 1) { dp = dp + [0]; }
let row: Int[] = []; ... dp = dp + row;   // appends/flattens into the same 1-D array
dp[i][j] = ...                            // then indexed 2-D
```

which produces:

```
VM Error: Expected a pointer to an array for element access.
```

Two things worth separating:

- The 0-based `Text` migration **worked** here. Models now correctly write
  `loop i in 1..(n + 1)` with `a[i - 1]`, the right 0-based translation of a
  1-based DP recurrence. Clearing that blocker is what exposed this one.
- Declaring `Int[]` and indexing `x[i][j]` is a **type error knowable at compile
  time**, but it surfaces at runtime with no code and no hint. This is the same
  shape `copy()`-applied-to-an-array had before the coded TYPE-001 hint in
  `bf9f2d8`, which per the cs-aug20 brief had been costing ~9 attempts on
  `cs_merge_sort`.

**Fixed 2026-07-26-6** as `ARR-002`, a parse-time check. It names the binding
and its declared type and gives both remedies (`Int[][]` with rows built as
arrays, or a computed offset `dp[r * width + c]`):

```
[ARR-002] 'dp' is declared 'Int[]', a one-dimensional array, but is indexed
twice here.
hint: declare it as 'Int[][]' for a real 2-D array (rows are themselves arrays:
`row = row + [v];` then `dp = dp + [row];`), or index it once with a computed
offset such as `dp[r * width + c]`.
```

Deliberately narrow — it fires only when the base is a plain variable whose
*declared* type name carries exactly one `[]`. A slice (`xs[a..b][i]`) lowers
through a temp before the second index, a field base (`self.rows[i][j]`) is not
a bare variable, and `Text` has rank 0. Zero hits sweeping all 60 examples and
every fixture.

The other half of this entry mattered more than the diagnostic, and was fixed
first: **nothing in the corpus showed `Int[][]` written correctly**, so a model
had no correct form to copy and the wrong rank was the path of least
resistance. `examples/base/nested_arrays` and a nested-array section in all
three guides close that. The diagnostic is the backstop for models that still
get it wrong; the example is what stops them reaching it.

Not done: the mirror case the entry also raised — appending an `Int[]` into an
`Int[]` whose declared type is 1-D. That is a different site (assignment /
concat type checking, not indexing) and did not reproduce as a runtime failure
in the shapes tested, so it needs its own repro before it gets a check.

---

## Mined from an unprompted "impress me" transcript — 2026-07-26

*Source: a reasoning trace from a model we had not sampled before, writing its
first Aether program (a "Release Board Analyzer") from the guide alone, with no
compiler in the loop. Unlike the idea-miner batches above, this is a **thinking
trace, not a diagnostic log** — it shows where the model second-guessed itself
and, in two places, where it talked itself into a wrong answer. That makes it
useful for a class of problem the miner cannot see: failures the model never
gets to observe because it never runs anything.*

Verified against `aether` 2026-07-26-1. Two of the six items were real bugs in
the generated program; the rest were the model correctly reaching a safe answer
by a wasteful route, which is its own signal about the guide.

### 1. Hallucinated `toon_parse_string`, then fabricated a guide citation for it — *fixed (docs) 2026-07-26*

The model wrote `toon_parse_string(payload)`, paused to check itself, and
produced this:

> Actually, looking at the guide examples:
> ```aether
> let doc: ToonDoc = toon_parse_string(payload);
> ```
> Yes, this is shown. Good.

No such line exists in either guide, and `toon_parse_string` is not a builtin —
it is a hard `SCOPE-001`. The self-verification step did not consult anything;
it re-emitted the model's own invention and accepted it as evidence. This is
worse than an ordinary hallucination: the model's *doubt* fired correctly and
then got answered by a confabulation, so every downstream check ("am I using
only listed builtins?") returned a false pass.

The name is over-determined by our own surface: `toon_parse_file(path)` exists,
so an LLM reads `toon_parse` as the bare stem and expects `_string` to be the
sibling of `_file`. `parse_json` → `toon_parse` is already in the alias table in
`ast_prepasses.c` (`rewriteAetherBuiltinAliases`) for exactly this kind of
plausible guess.

**Done:** added `toon_parse_string` to the **Never Generate These** list in
`aether_for_llms_and_others.md` and to the TOON rules in
`..._with_small_contexts.md`, in both cases naming the `_file`-implies-`_string`
inference explicitly rather than just listing the bad name.

**Also done (2026-07-26-2):** `toon_parse_string` → `toon_parse` added to the
alias table, per the precedent above. Matching is whole-identifier, so the real
`toon_parse` (10 chars) and `toon_parse_file` (15) never collide with the 17-char
entry; `tests/toon_parse_string_alias_pass.aether` pins both the rewrite and the
non-collision. The alias stays undocumented except as a never-generate entry —
listing it in the guides would legitimize the invented name, which is the
opposite of what the alias table is for.

### 2. Merged `formatfloat(r, prec)` with the `println`-only `r:0:prec` form — *fixed (docs + example) 2026-07-26*

The model wrote `formatfloat(sa.averageScore(), 0, 2)` throughout — a
three-argument call that does not exist. The contamination source is a single
table row in the full guide:

| Real → Text | `formatfloat(r, prec)` | `realtostr(r)` | `r:0:prec` as a value (it is `println`-only) |

Canonical form and forbidden form sit in the same row, and the forbidden column
supplies the exact extra token (`0`) needed to build the wrong arity. The model
took the width slot from the "never" column and spliced it into the "canonical"
column's argument list.

This one is nastier than a normal arity error because **it is not caught at
compile time**. `aether --no-run` accepts it; the failure is a runtime message
with no diagnostic code at all:

```
FormatFloat expects (numeric [, integer precision]).
[Error Location] Offset: 125, Line: 12
```

So a generated program can pass a compile-only gate and die on its first
formatted number.

Aggravating factor: **`formatfloat` appeared nowhere in the 55-program example
corpus.** `number_formatting` demonstrates only the `value:width:precision`
spelling. The one conversion the guide calls canonical had zero worked examples,
while its forbidden alternative had a whole example to itself.

**Done:** new `examples/base/real_to_text` puts `formatfloat(r, prec)`,
`realtostr(r)`, and `r:width:prec` side by side and names the three-argument
trap in a comment; the guide table row now states the arity and lists
`formatfloat(r, 0, prec)` as a never-generate; the small-context guide carries
the same warning inline.

**Also done (2026-07-26-2):** new `BUILT-002` diagnostic, checked at compile
time in `aetherCheckBuiltinArity` (`semantic.c`) over a hand-verified table of
fourteen fixed-arity conversion/math builtins. `formatfloat(r, 0, 2)` now fails
under `--no-run` with `[BUILT-002] 'formatfloat' takes 1 to 2 arguments, but 3
were given.`, and the code flows through `--diagnostics-json`/`-toon` so the
repair loop stops seeing `code:null`.

Two things worth remembering from building it. First, **the arity ranges cannot
be derived from the published signatures.** `formatfloat`'s metadata entry in
`query_builtin.c` says `(value: Real, precision: Int)` — two parameters, no
optional marker — but the VM guard is `arg_count < 1 || arg_count > 2`. A
signature-driven check would have rejected the perfectly valid one-argument
call. Every entry is read off the `arg_count` guard in pscal-core instead.
Second, shadowing has to be excluded on two axes: a top-level `fn max(a, b, c)`
shadows the builtin outright, and a `type` method named `min` is resolved by its
receiver — the table must fire on neither
(`tests/builtin_arity_shadow_pass.aether`).

**Still open:** the table is hand-maintained and covers fourteen names. The
general fix is an exported accessor for `kAetherBuiltinMeta` in pscal-core plus
a real optional-parameter marker in the signature strings, which would let the
frontend check every builtin instead of a curated list. That is a submodule
change with the usual push-order cost, so it was not bundled here.

### 3. The guide's "discover it before you call it" advice is unusable in the generation context

The model correctly recalled the rule — *"if one is not listed here, discover
its exact name and signature before calling rather than assuming"* — and then:

> Since I can't actually call `builtin_info` in this context (I'm writing code,
> not running it), I should be careful.

This is the right read. `builtins_json()` / `builtin_info(...)` are **runtime**
builtins; a model producing a source file in one shot has no way to invoke them.
The guide currently offers discovery as the escape hatch for anything unlisted,
and for the single largest consumer of the guide that hatch is nailed shut. The
model's fallback — route around the unknown getter by using `toon_get_text_or`
plus `parse_int` — was safe but produced worse code than the `toon_get_int_or`
that has been in the guide all along.

**Partly done (2026-07-26):** BUILT-001 in both guides now says outright that
`builtins_json()` / `builtin_info(...)` are *runtime* builtins a one-shot
generator cannot call, and that the fallback is to restructure onto a listed
helper rather than guess a name.

**Still open:** that only works if the listed surface is complete enough to
restructure onto, which item 4 shows it effectively is not once the document is
truncated. The durable fix is shipping the builtin inventory as a static
appendix so "discover" can mean "look further down this document."

### 4. Truncation drops the exact tables the model then guesses around

The trace says *"Looking through the truncated guide"* — the model was working
from a cut copy of `aether_for_llms_and_others.md`. Everything it went on to
guess at was in the part it lost:

| The model reasoned... | Actually in the guide at |
|---|---|
| "`toon_get_int_or` is a reasonable guess... but the guide says not to guess" | line 1106 (full `_or` table) |
| "I need to check if `max_real` exists. The guide doesn't list it." | line 653 — `min`, `max`, **`clamp(x, lo, hi)`** |

It wanted a clamp, invented `max_real`, correctly rejected it, and hand-rolled
an `if raw < 0.0 { ret 0.0; }`. `clamp(raw, 0.0, 100.0)` was two lines past
its context window, and `examples/base/clamp_minmax` exists.

Both tables live past line 650 of a 1667-line document, i.e. in the half that
gets cut first. The high-frequency conversion and math surfaces are ordered
after prose that a generating model needs far less.

**Suggested action:** treat "survives truncation at 40%" as a documented
ordering constraint for the full guide, and hoist the conversion / math /
TOON-getter tables above the long-form rationale. This is cheap and it is the
single highest-leverage change on this list — items 1, 2 and 3 above all
partially reduce to "the model could not see the table."

### 5. Contradictory import guidance produced real paralysis

The model spent a visible stretch on whether TOON needs an import, quoting the
guide against itself — *"Use verified modules only. Never invent imports"* vs
*"use canonical `use "module_name";`"* — with no way to tell which modules are
"verified." It settled on emitting no imports, which is correct, but by
elimination rather than by knowing.

Verified: `use "toon";` is a **silent no-op**. It compiles and runs; the
`[IMP-001] Aether ignored missing import 'toon'` warning only appears under
`--verbose-compat`.

**Suggested action:** state plainly that all builtins — TOON included — are in
the prelude and need no import, and that `use` is *only* for sibling `.aether`
modules in the same directory. The current phrasing implies a registry of
"verified modules" that does not exist.

### 6. Correctly-guessed constructs, confirmed working (no action)

Everything else the model was unsure about does work, and is worth knowing is
uncontroversial: field defaults in a `type` body (`total: Int = 0;`); forward
references to a `type` declared later in the file; `@pure` on a function that
takes a `ToonNode` and calls `toon_get_*`; `@pure` on a function that does
`new T()` and returns it; a `let i: Int = 0;` binding coexisting with
`loop i in 0..n` (the 2026-07-19 SCOPE-001 fix holds); self-mutating methods
called from inside a `par` branch, both via a wrapper `fn` and as a direct
`x.bump();` statement; and free `Int`/`Real` mixing in arithmetic with
`Int / Int` still truncating. A misspelled `self.reveiw` gives a clean
`[FIELD-002] Unknown field 'reveiw' on class 'A'`.

### 7. Incidental: the examples lap was not compile-checking `showcase/` at all — *fixed 2026-07-26*

Found while adding `examples/showcase/release_board`. `tests/run_examples.sh`
selected showcase programs with `find "$EX_DIR/showcase" -name '*.aether'`, but
showcase programs are extensionless (`agent_report`, `gradebook`) exactly like
`base/` ones. The glob matched **zero files**, so the lap's own comment
("including gradebook") had been false since it was written, and `gradebook`
was covered only indirectly. Fixed to the same "not README, not .json" filter
`base/` uses; the lap went from 54 to 57 programs.

### 8. Guide preamble trimmed — 2026-07-26

Removed from the top of both guides: *"Aether is a compact front end for the
PSCAL suite. It targets the existing shared PSCAL backend, bytecode compiler,
and VM. It is not a separate runtime."* It is implementation trivia that changes
nothing about what a model should generate, it burns tokens in the part of the
document least likely to survive truncation (item 4), and a human can learn it
from `README.md` or `aether_architecture_and_rationale.md`. The same clause was
also trimmed out of the "what Aether is for" paragraph further down, keeping the
half that does constrain generation ("not a dynamic scripting language, and not
a place to import imagined libraries").

### Corpus additions from this trace

- `examples/base/real_to_text` — the Real→Text surface, item 2.
- `examples/showcase/release_board` — the program the model was *trying* to
  write, corrected and running: nested records with field defaults, self-mutating
  accumulator methods, `@pure`/`@post` helpers, a `par` block where each branch
  owns its record while sharing read-only `ToonNode` handles, defaulted TOON
  reads over a deliberately malformed row, `clamp`/`max`, and both `formatfloat`
  and `r:0:1` used correctly in the same report. Turning a failed generation
  into a canonical worked example targets the exact shape models pick when asked
  to show the language off.

---

## Imported types with methods: the vtable global was emitted under the module's name — *fixed 2026-07-26*

*Source: scoping work for a proposed "modules embedded in the binary" surface —
a small set of `use`-discoverable modules compiled into `aether` itself, dumpable
via a `--dump-module` flag, so a program needs no files beyond the binary. The
only payload such a module can carry that a plain C builtin cannot is an Aether
`type` with methods; a builtin can export functions and constants but cannot
introduce a type. So the whole idea rested on imported types with methods
working, and they did not.*

The repro is two files in one directory (the module file extensionless, since
`use "stackmod";` resolves the literal string as a path relative to the importing
source — `resolveRelativePath`, `src/aether/ast_prepasses.c:389`):

```
mod StackMod {
    export type Stack {
        data: Int[] = [];
        fn push(v: Int) -> Void { self.data = self.data + [v]; ret; }
        fn depth() -> Int { ret length(self.data); }
    }
    export fn make() -> Stack { ret new Stack(); }
}
```

Any call through the imported type died with:

```
Runtime Error: Global 'stack_vtable' not found in symbol table.
```

The boundary was sharp and misleading: `export fn` and `export const` across an
import worked, and so did a method-*less* record, so it read like a problem with
types rather than with methods.

**Cause.** An imported module is not textually spliced; it is loaded, analyzed,
and compiled as its own unit into the shared chunk (`compileModuleAST`), with
`compilerSetCurrentUnitName(moduleName)` set for the duration. `compileDefinedFunction`
prefixed that unit name onto every routine name it compiled. For a plain export
that is right and necessary (`stackmod.make`). But a method's name is *already*
mangled by the semantic pass as `class.method`, so `Stack.push` became
`stackmod.stack.push` and — finding no such symbol — got a freshly minted second
procedure symbol. The canonical `stack.push` that `collectMethods` had registered,
which the class's method table and every call site resolve through, was left with
`bytecode_address == 0`.

`emitVTables` then derives the class name from the text before the **first** dot,
so it built a table for a "class" named `stackmod`, emitted it as `stackmod_vtable`,
and skipped the real `stack` table as unresolved — while `new Stack()` kept loading
`stack_vtable`, which nothing defined. Two further symptoms fall out of the same
split symbol: a module with several classes would have had them all collapse into
one module-named table, and the first call to an imported method emitted a *second
complete copy of the method body* inline (the "ensure the target procedure is
compiled" path in `compileNode`, firing because the symbol it resolved had no
address).

**Fix** (`external/pscal-core`, `src/compiler/compiler.c`): apply the unit prefix
only to unqualified routine names — a name that already contains a dot is a
class-mangled method and stays on its canonical symbol. Same-file classes are
unaffected (they never had a unit name set).

A second, latent defect surfaced immediately behind it. Once methods bind to the
symbol the semantic pass created, that symbol's `type` is whatever `calloc` left —
`TYPE_UNKNOWN`, not `TYPE_VOID`. That field is serialized into the bytecode cache,
and the verifier reads it back to decide whether a `RETURN` must leave a value on
the stack, so every cached chunk containing an imported `-> Void` method failed
verification, printed `Warning: rejecting corrupt cached bytecode ...`, and was
thrown away — correct output, no caching, noise on stderr. `compileDefinedFunction`
now marks a value-less routine `TYPE_VOID` on whichever symbol it binds. Only the
VOID/non-VOID distinction is corrected: a value-returning function's recorded type
also drives return-value coercion at runtime, and overwriting a resolved one with
the *declared* type changes results — `tests/extension_call_alias_pass.aether`
catches exactly that (an `-> Int` function whose body is `Int / Int`, which is real
division, prints `5.000000`).

**Regression coverage.** `tests/imported_type_methods_pass.aether` plus the
extensionless `tests/imported_type_methods_mod`: a Void method, a method calling a
sibling method, a method calling a module-private function, and a same-file type
with methods alongside the imported one. The block in `tests/run.sh` asserts both
the `--no-cache` run and a second, cached run (the latter guards the verifier
regression, which a `--no-cache`-only assertion cannot see).

**Two adjacent limits, unchanged and pre-existing.** Type names are global, so two
imported modules that each export a same-named type still collide — now as
`VM Error: No procedure found at address N for indirect call` rather than the
missing-vtable error. And a type and a function whose names differ only in case
(`type Shape` beside `fn shape()`) collide inside a module, reported as the
thoroughly unhelpful `'shape' is not exported from module 'Shapes'`. Both are worth
their own coded diagnostics; neither is on the path to embedded modules.

**Unblocked.** With this fixed, an embedded module has a payload that genuinely
could not have been a builtin, so the `--dump-module` idea is worth costing out.

---

## `random` follow-ups — 2026-07-26

*Found while checking a hand-written "Animated Fractal Tree" program a user
asked about. It compiles clean, runs, exits 0, emits no diagnostic — and draws
its entire "random" tree in a single repeated character. Chasing why turned up
three separate issues, the last of which is the serious one.*

### 1. `random()` vs `random(n)` is a same-name/different-return-type trap — *example added 2026-07-26*

```aether
let idx: Int = 0;
fx { idx = random(); }          // ALWAYS 0
idx = idx % length(chars);      // ...so this is 0 too
ret chars[idx];                 // ...so this is always chars[0]
```

`random()` returns a **Real** in `[0, 1)`; `random(n)` returns an **Int** in
`[0, n)`. Assigning the no-argument form to an `Int` truncates the fraction to a
constant `0`, so every "random" choice in the program is the same choice
forever.

Both guides *do* document the two forms correctly (full guide line 667, small
guide line 321), so this is not a documentation error. It is a coverage and
placement problem:

- **`random` appeared nowhere in the example corpus or the test fixtures.**
  Zero uses, exactly like `formatfloat` before this week. The trap is one
  the reader has to derive from two return types listed in a table cell.
- Line 667 is fourteen lines from the `min`/`max`/`clamp` row at 653 that the
  2026-07-26 trace could not see. The same truncation zone keeps eating the
  same class of information: arity/type details of small helpers.
- Note that `BUILT-002` cannot help here — both arities are legal, so an arity
  check has nothing to object to. The error is in the *return type*.

**Done:** `examples/base/random_values` demonstrates the bounded-Int form for
indices and rolls, the Real form kept as a Real, and `int(random() * 100.0)` as
the explicit-truncation route.

### 2. Every implicit `Real` → `Int` narrowing is silent — *candidate `NARROW-001`*

`random()` is only the most damaging instance of a general hole. All four of
these compile without a word and truncate:

```aether
let a: Int = 0;  fx { a = random(); }   // 0    -- builtin, effectful position
let b: Int = 3.7;                       // 3    -- a literal, at the declaration
let c: Int = half();                    // 0    -- user fn returning Real
let d: Int = 0;  d = sqrt(2.0);         // 1    -- builtin, plain assignment
```

`let b: Int = 3.7;` is the striking one: the value is a literal, the target type
is written on the same line, and the loss is knowable at parse time. Aether
already has `int(x)` as the explicit truncating cast and already documents it,
so the escape hatch exists — there is just nothing steering anyone toward it.

**Fixed 2026-07-26-4.** `NARROW-001`, a warning (never an error), shaped like
`ARR-001`. Fires on a Real literal, an always-Real builtin, a call to a function
declared `-> Real`, arithmetic with a Real operand, and zero-argument
`random()`; `int(...)` is never flagged.

The sizing concern above turned out to be the whole difficulty, and the answer
is that **the AST's resolved types cannot be trusted for this**. Sampling them:
`min(3, 5)` is annotated `REAL` although min/max/clamp/abs preserve operand
type, `7 / 2` is annotated `REAL` although `Int / Int` evaluates to `Int`,
`sqr(3)` arrives as `VOID`, and zero-argument `random()` is annotated `INTEGER`
even though that is the Real arity. A check that trusted the annotation fired on
`min` and on every integer division. What works instead is a hand-verified
always-Real builtin table, an explicit arity test for `random`, and a new
per-function "declared return type is Real" flag recorded at parse time — the
call node's own `var_type` is still `0` throughout semantic analysis, so the
type annotations visible in `--dump-ast-json` come from a later stage.

Two further constraints found by building it. The pass must run **after**
`reaPerformSemanticAnalysis`, or every assignment target still reads as
untyped and only `let` declarations are ever checked. And it must fire only on
an **explicitly written** `: Int`: an inferred `let x = intVal * realVal;`
still reads as integral during the pass despite resolving to Real, so warning on
it is simply wrong. That last one was caught by sweeping the corpus rather than
by reasoning — `tests/numeric_expr_inference_pass.aether` was the single false
positive out of 58 examples plus every fixture, and it is now the regression
case in `tests/narrowing_quiet_pass.aether`.

### 3. `par` branches all draw the IDENTICAL random stream — *bug, unfixed*

The serious one. `rand_seed` in pscal-core (`builtin.c`) is
`static _Thread_local unsigned int rand_seed = 1;`, and `randomize()` sets
`rand_seed = time(NULL)` **on the calling thread only**. Every worker thread
therefore starts from the hardcoded seed `1` and generates the same sequence:

```
par branch 1 : 807 249 73
par branch 2 : 807 249 73     <- identical
main thread  : 208 990 249    <- the only thread randomize() reached
```

Calling `randomize()` *inside* each branch does not fix it, because
`time(NULL)` has whole-second resolution and both branches call it in the same
second:

```
branch 1 : 120 680 653
branch 2 : 120 680 653        <- still identical
```

So **there is currently no way to obtain independent random streams across
`par` branches.** A parallel Monte Carlo, sampler, shuffle, or randomized
search — precisely the workload `par` exists to serve — silently computes the
same draws in every branch and reports a confidently wrong aggregate. Nothing
warns, and the output looks plausible.

The same second-resolution seeding also means two single-threaded runs launched
within one second replay an identical sequence, which is a trap for anything
that runs the binary in a tight loop (eval harnesses included).

**Fixed 2026-07-26-4** (pscal-core `5cbedfb`). Each thread derives its seed on
first draw from a shared base mixed with a unique per-thread index, through a
splitmix32 avalanche so adjacent indices do not produce visibly correlated early
output. `randomize()` now stores a base built from microsecond entropy and
reseeds the calling thread immediately, so two branches that each call it still
diverge.

Three properties hold simultaneously, and all three are pinned by fixtures:
branches draw independent streams (`random_par_streams_pass`); a run with no
`randomize()` is bit-for-bit reproducible (`random_reproducible_pass`, executed
twice and compared); and `randomize()` makes back-to-back runs differ
(`random_seeded_pass`, likewise run twice — the one assertion here that samples
entropy rather than a fixed value, so it retries once before failing).

Resolved the open question the entry raised: `randomize()` seeds the *base* for
every thread that has not yet drawn, plus the calling thread outright. The
remaining limit is deliberate — thread indices are handed out in first-draw
order, so *which* branch gets which stream is not deterministic across runs.
Pinning a stream to a particular branch would need a branch id the VM does not
expose to builtins, and independence is the property the bug was about.

---

## A field default must be a literal, but every guide said "constant" — 2026-07-26

Found while adding `examples/showcase/grade_report`. A statistics accumulator
wants sentinel-seeded bounds:

```aether
const MAX_SCORE: Int = 100;

type Stats {
    min: Int = MAX_SCORE + 1;   // [FIELD-003]
    max: Int = -1;              // fine
}
```

`FIELD-003` rejects it — and it rejects the bare `min: Int = MAX_SCORE;` too.
Only a **literal** is accepted. Verified across four forms: `= 101` works,
`= -1` works, `= MAX_SCORE` fails, `= MAX_SCORE + 1` fails.

All three guides said something materially different. The full guide's wording
was *"A record/type field may declare a **constant** default: `field: Type =
<const>` ... Only compile-time constants are allowed — a default may not
reference another field, `self`, or call a function."* A named `const` is a
compile-time constant, is not another field, is not `self`, and is not a call,
so the documented rule positively invites `= MAX_SCORE`. The small and medium
guides carried the same claim.

The diagnostic reinforces the misreading: *"only constant field defaults are
supported; set computed values at construction"* — but `MAX_SCORE` is not
computed, so a model that just wrote it has no way to tell what is being
objected to. The hint (*"use a literal or constant expression (e.g. `= 0`,
`= ""`, `= true`)"*) says "constant expression" while rejecting exactly that.

**Fixed (docs) 2026-07-26:** all three guides now say *literal*, name the
named-`const` case explicitly since it is the one a reader would otherwise
assume works, and point at construction (`new T { limit: MAX_SCORE + 1 }`) as
the route for anything else.

**Still open:** either widen the check to accept a constant-folded expression
over `const` values — it is a parse-time fold, and sentinel bounds derived from
a declared maximum are an ordinary thing to want — or reword the diagnostic and
its hint to say "literal" and stop offering "constant expression" as a remedy
for a constant expression. The current message cannot be acted on correctly by
someone who hit it the obvious way.

---

## Evaluating an outside contribution — 2026-07-26

*A second model was pointed at the corpus-gap list and asked to fill it. Its
final deliverable was a "verified reference" for Aether. Most of it is wrong,
and the ways it is wrong are more useful than the parts that are right.*

Everything below was re-tested against the compiler rather than taken on trust.

### What it got right, and what that corrects on our side

**2-D arrays work.** `let grid: Int[][] = [[1, 2, 3], [4, 5, 6]];` compiles and
`grid[0][1]` reads back `2`. So does the shape that actually matters: building a
table row by row (`row = row + [v]`, then `table = table + [row]`), writing a
cell through both indexes, jagged rows, and `length(table[r])` for a row width.

This corrects a statement made in the same session's gap analysis — that the
backlog "suggests they may not work". Re-reading the *1-D array indexed as 2-D*
entry above, it never said that. It says models **declare `Int[]` and then index
`x[i][j]`**, which is a type error that surfaces as an uncoded runtime message.
The language was never the problem; the corpus was. Nothing in 60 example files
showed the correct `Int[][]` declaration, so there was nothing to copy.

`examples/base/nested_arrays` now covers it. That is the corpus half of the
`cs_lcs` fix and probably the cheaper half — the coded diagnostic proposed in
that entry is still worth adding, but a model that has seen `Int[][]` written
correctly will not reach the diagnostic in the first place.

### The finding worth keeping: one mistake, two unrelated diagnostics — *fixed 2026-07-26-5*

The contribution burned roughly a dozen turns convinced the compiler was
non-deterministic, writing things like *"This is extremely inconsistent... the
only explanation is that the compiler is not fully isolated between runs."* It
was not. The same nonexistent builtin reported differently depending on one
unrelated detail:

```aether
let db = sqlite_open("x");        // [TYPE-001] cannot infer the type of 'db'
                                  // hint: add an explicit type ...
let db: Int = sqlite_open("x");   // [SCOPE-001] identifier 'sqlite_open' not in scope
```

Both spellings share one root cause — `sqlite_open` does not exist — and neither
diagnostic said so in the inferred form. Worse, **the hint sent the reader the
wrong way**: it advised adding an annotation, which then produced a completely
different code, so the fix looked like it had *caused* the second error. A model
alternating between the two forms sees the same program yield SCOPE-001 and
TYPE-001 unpredictably, which is exactly the "non-deterministic compiler"
conclusion it reached.

**Fixed.** When the inferred-`let` path cannot derive a type and the initializer
is a call to a name that is provably unknown — not a declared top-level
function, not a registered VM builtin (`getVmBuiltinID` < 0), not in the
function-return table — it now reports `SCOPE-001` naming the callee, with a
hint pointing at the builtin list. Both spellings now give the same code.

Deliberately narrow: a *real* builtin whose return type is not in the inference
table (`let s = socketcreate(0);`) still gets TYPE-001 and the annotate hint,
because there the hint is correct. So does an untyped array literal.

### What it got wrong, and why each is a corpus or guide signal

- **`par` fabricated.** Its reference presents
  `let r = par(task_a(), task_b()); r[0]` as working concurrency. It never ran:
  its own transcript shows TYPE-001, then `TUP-001`, then *"expected '{' to open
  par block"* — which is the compiler telling it `par` is a block construct. It
  wrote the failure up as a feature anyway. Verified still broken exactly as
  reported.
- **Sockets declared unavailable.** *"Socket/SQLite: Not available in this
  environment (not in scope)."* False — `socketcreate(0)` returns a handle and
  `socketclose` works. It guessed `socket`, `tcp_socket`, `socket_create`; the
  real names have no underscores. The full guide lists them, in the region a
  truncated copy loses first. Third independent instance of the truncation
  failure mode.
- **`pos` arguments backwards.** It wrote `pos(s, "world")`, got `-1`, wondered
  *"maybe there's a bug"*, and shipped the backwards form in its reference. The
  signature is `pos(needle, haystack)` — `pos("world", s)` is `6`. `pos` appears
  in exactly one example file, which is the whole problem.
- **`paramstr(0)` documented as returning a count as an integer**, contradicting
  its own transcript where that exact line failed with *"Cannot assign STRING to
  integer"*. `paramcount()` is the count; `paramstr(0)` is the program name.
- **Citations to an unrelated project.** Its sources are a GoogleCloudPlatform
  repo and a Reddit post about an actor-based language, neither of which is this
  Aether. It noticed the syntax did not match and proceeded anyway.

### `gettime` has no documented signature — *open*

Both models that reached for a clock wrote `let t = gettime();` and got
*"Built-in procedure 'gettime' cannot be used as a function in an expression."*
It is a DOS-style procedure taking **four var out-parameters**
(`dosGettime expects 4 var arguments`). The guides name `gettime` only in a
parenthetical list of effectful builtins — *"the clock (`gettime`)"* — and never
show a call, so the only reachable conclusion from the documented surface is the
one both models drew.

**Suggested action:** document the real shape, or expose a function-form clock.
The var-out-parameter calling convention appears nowhere else in the Aether
surface except `readln`, so it needs an example if it is to stay.

---

## Corpus and diagnostic sweep — 2026-07-26

Working the gap list from the coverage audit. Four examples, two diagnostics,
and one non-decision.

### Examples added

- `examples/base/text_processing` — the whole `Text` surface on one parsing job.
  Built around `pos`, which is the most-misused builtin in the language: **two
  independent models got its argument order backwards** in the same week. It is
  `pos(needle, haystack)`; reversing the arguments returns `-1`, which reads as
  "not found" rather than "wrong order", so the mistake never announces itself.
  It also returns `-1` when absent while `0` is a legitimate match at the first
  character, so the test is `>= 0` and `> 0` is silently wrong.
- `examples/base/tuple_returns` — destructuring, positional `.0`/`.1`,
  `@post result.0`, and the record fallback. Eleven fixtures backed this feature
  against one example, and that one only showed the `@post` form. The closing
  comment lists each TUP-001 wall, all re-verified: chaining `.0` onto a call,
  an out-of-range index (a compile-time error, not a crash), and a method
  returning a tuple.
- `examples/base/bitwise_ops` — flag masks, an XOR cipher that round-trips,
  `popCount`, a bit-mixing hash. Bitwise was added on 2026-07-19 *because*
  models wanted XOR ciphers and bit-mixing hashes, and then no example used it.
- `examples/base/nested_arrays` (earlier commit) — `Int[][]`.

### Precedence trap worth its own note

`&` binds looser than `!=`, and the failure is silent:

```
(flags & mask) != 0   ->  true   (Bool)
 flags & mask != 0    ->  1      (Int -- parses as flags & (mask != 0))
```

The second is not an error. It yields an `Int` that prints as `1`, so a
permission test written that way looks like it passes. Documented in
`bitwise_ops`; a candidate for a future coded warning, since the type change is
mechanically detectable.

### FIELD-003 message rewritten — *fixed 2026-07-26-7*

The old text was actively self-contradicting: *"only constant field defaults are
supported; set computed values at construction"* with the hint *"use a literal
or constant expression"*. But the check rejects a named `const` and any
arithmetic over one — neither of which is *computed*, and the second being
precisely what the hint recommends. It now says "a field default must be a
literal", names the const case outright (`= MAX` fails just as `= MAX + 1`
does), and points at construction. A fixture pins the named-const case
specifically, since that is the form a reader assumes works.

### `gettime` — resolved without a language decision

The open question was whether to document the var-out-parameter convention or
add a function-form clock. Neither: **a function-form clock already exists and
was simply undocumented.** `realtimeclock()` returns Unix epoch seconds as an
`Int` — verified against `date +%s` — and appears nowhere in any guide, only in
this backlog. It is the thing every model reaching for a clock actually wanted.

The component readers are procedures filling var out-parameters, now confirmed
by experiment rather than guessed: `gettime(hour, minute, second, centisecond)`
matched the wall clock exactly, and `getdate(year, month, day, weekday)`
returned `2026, 7, 26, 0`. All three are documented in all three guides now,
with `realtimeclock` named as the default choice and the out-parameter
convention flagged as appearing nowhere else in Aether except `readln`.

---

## Two bugs found by writing the missing examples — 2026-07-26

Filling the last corpus holes (File I/O, sockets, branching) turned up two
defects that no amount of reading would have surfaced. Both were found because
the example produced a wrong *answer*, not because anything errored.

### `readln` aliased every copy of the Text it filled — *fixed 2026-07-26-8*

`examples/base/file_io` tracks the longest line it reads. It reported `delta`
where the answer is `alpha`, and chasing that produced this:

```aether
readln(f, line);  a = line;    // "one"
readln(f, line);  b = line;    // "two"
readln(f, line);               // "three"
// a, b and line ALL read "three"
```

Copying a string Value copies the `StringObj` pointer, so any number of Values
share one object. `vmBuiltinReadln` reused that object — freed the old buffer,
installed the new one, re-tagged the same wrapper — which mutated every sharer
at once. **Reading a file into an array produced N copies of the last line**,
silently, which is about as bad as a bug gets in a language whose stated purpose
is parsing structured input.

Plain assignment was never affected (`s = t;` then `t = "x";` left `s` alone)
because assignment *rebinds* the pointer instead of mutating the object. The fix
makes `readln` do the same: allocate a fresh `StringObj` and rebind. The old
object is no longer freed there, precisely because a sharer may still hold it —
it is header-managed.

Worth checking whether any other out-parameter builtin mutates in place the same
way. `readln` is the only one in the Aether surface, but the pattern is in
pscal-core generally.

### `has_builtin` only sees *extended* builtins — *open*

The first draft of `examples/base/sockets` guarded with
`has_builtin("network", "SocketCreate")` and printed "socket support
unavailable" — on a build where `socketcreate(0)` returns a working handle.

`has_builtin(category, function)` searches the **extended** builtin registry
only. Core VM builtins are not in it at any category name:

```
has_builtin("system", "FileExists")    = true    (extended)
has_builtin("yyjson", "YyjsonRead")    = true    (extended)
has_builtin("network", "SocketCreate") = false   ... but socketcreate works
has_builtin("system", "SocketCreate")  = false
```

Both guides present `has_builtin` as *the* capability probe without saying what
it can see, so a guard written for a core builtin is not merely useless — it is
inverted, and reports a working capability as missing. `has_toon()` and
`has_ai()` are separate purpose-built probes precisely because of this, which is
the shape of the answer.

**Suggested action:** either extend `has_builtin` to fall back to the core
registry (`getVmBuiltinID(name) >= 0`) when the category lookup misses, or
document plainly that it covers extended builtins only and that core builtins
are always present. The first is a small change and removes the trap; the second
leaves a probe that silently lies for the most obvious use.

---

## Closing the open compiler items — 2026-07-26

### `has_builtin` now sees core builtins — *fixed 2026-07-26-9*

Fixed as suggested in the entry above, with the fallback keyed on the function
name alone: core builtins are not filed under a category, so no category could
match, and the function is the actual question. `has_builtin("anything",
"socketcreate")` is therefore true. A name in neither registry still answers
false, which is the assertion the fixture leads with — an absent OpenAI builtin
must not be waved through, and the `ai_helpers` skip path depends on that.

### `PREC-001` for a bitwise operator against a comparison — *fixed 2026-07-26-9*

Recorded when writing `bitwise_ops`, now a warning. The shape:

```
(flags & mask) != 0   ->  true   (Bool)
 flags & mask != 0    ->  1      (Int -- flags & (mask != 0))
```

Deliberately not unconditional. `&` and `|` **double as eager, non-short-
circuiting boolean operators** on `Bool` operands, so `ready & (n == 0)` is a
legitimate conjunction and must stay silent. The warning fires only when the
left operand is provably `Int` — an integer literal, or a variable declared
`Int`. Anything whose type is not visible at parse time is left alone, which is
the same discipline `NARROW-001` and `ARR-002` use.

Adding it needed a parse-time warning reporter (`reportAetherAstWarning`);
there was only an error path before. It never sets `p->hadError`.

### The `ARR-002` mirror case is narrower than it looked — *mostly covered, residual open*

The original entry proposed a second check for "appending an `Int[]` into an
`Int[]` whose declared type is 1-D". Now reproduced, and the priority is lower
than assumed:

```aether
let flat: Int[] = [];
let row: Int[] = [1, 2, 3];
flat = flat + [row];        // accepted -- length becomes 1
fx { println(flat[0]); }    // prints ARRAY(dims:1, base_type:INT64, ...)
fx { println(flat[0][1]); } // [ARR-002] -- caught
```

The append is a declared-type violation and is accepted silently, so the hole is
real: array concat does not check element types against the declared element
type. But the damaging path — building a table this way and then indexing it —
is **already caught by `ARR-002` at the use site**, and the residual path
produces visibly wrong output (`ARRAY(dims:1, ...)`) rather than a plausible
wrong number.

**Suggested action, downgraded:** element-type checking on array concat is worth
having as general type hygiene, and would catch this at the append rather than
at first use. It is no longer a `cs_lcs`-class score blocker, so it should be
scheduled as type-checker work rather than as a diagnostic patch.

---

## The builtin appendix, and all three guides now gated — 2026-07-26

### Complete builtin inventory in the full guide — *done*

The standing concern was that `BUILT-001` says "if it is not listed here, it does
not exist" while the guide listed a curated subset, and pointed at
`builtin_info(...)` for the rest — a *runtime* call a one-shot generator cannot
make. The rule was unfalsifiable from the reader's side.

The full guide now ends with the complete inventory, generated from the compiler
by `tools/gen_builtin_appendix.py` (110 documented signatures, 302 name-only,
~3K tokens). Two tiers, and the second carries most of the value: a name-only
entry confirms a name is real **without** licensing a guess at its arguments,
and anything in neither tier does not exist.

Checked against every name guessed wrong in this week's transcripts:

| Guessed | In appendix? | Real name |
|---|---|---|
| `socket_create`, `tcp_socket` | no | `socketcreate` |
| `toon_parse_string` | no | `toon_parse` |
| `sqlite_open` | no | `sqliteopen` |
| `substring`, `to_upper`, `replace` | no | do not exist |

Every one would have been settled by looking. The `sqlite_open` case is the
sharpest: the outside model's guess was one underscore off a real builtin, and
concluded from the failure that SQLite was unavailable.

**This was affordable only because the medium guide exists.** While the full
guide had to serve constrained contexts, 3K tokens of appendix was a real cost;
now the constrained tier has its own document and the full guide has no ceiling.
The size-tiering work paid for this directly.

### All three guides are snippet-gated — *done*

`tools/verify_guide_snippets.py` previously covered the medium guide only; the
other two had never been checked. Extending the context prelude and adding an
`fx`-retry for prose fragments brought the full guide from 13 unverified blocks
to 0 and the small guide from 7 to 0.

Two traps worth remembering, both hit while doing it. An `EXPECT_FAIL` key
matching a block in *another* guide silently marks a good snippet as
expected-to-fail — keys must be specific enough to identify one block. And the
same prose fragment appears in more than one guide with different surrounding
context, so a fix in one place can regress another; run all three.

The remaining allowlisted blocks are genuine prose: negative examples, `...`
elisions, and module sketches whose module is not on disk. Their count is
asserted, so a negative example that starts compiling is caught too.
