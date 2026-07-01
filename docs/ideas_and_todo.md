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

### `@cost` is decorative — *gap / decision needed*
`@cost <n><unit>` (units: `ns us ms s op ops step steps`) is syntax-validated
but **not enforced or tracked** — it carries no codegen and no runtime check.
Decide: either (a) make it a real, tracked/asserted budget, or (b) document it
explicitly as a non-binding annotation so reviews/users stop reading it as
"formal complexity tracking." Until then it risks being oversold. (Contrast:
`@pre`/`@post` lower to runtime assertions; `@pure` is enforced at compile time.)

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

### HTTP / sockets / SQLite — *deferred, not surfaced*
These are now correctly fx-gated (effectful) but remain **discovery-only**, not
in the curated guide surface:
- **HTTP** is low-level: `httprequest(session, method, url, body, out)` writes
  the response into a memory stream and returns the status code. It needs a
  session handle *and* the `mstream*` API. Worse, the memory-stream handle has
  **no surfaced Aether type name** and cannot be inferred (`let s: MStream`,
  `let s = mstreamcreate()` both fail), so HTTP is not cleanly usable from typed
  Aether yet. **Prereq for documenting HTTP:** either expose the mstream type
  name, or (better) add a clean alias layer — `http_get(url) -> Text`,
  `http_post(url, body) -> Text` — mirroring how `toon_*`/`task_*` alias raw
  backend builtins. Until then, leave HTTP undocumented.
- **Sockets** (`socket*`, `dnslookup`) and **SQLite** (`sqlite*`, ~21 fns): real
  and coherent, but large; surface only on demand. SQLite is the strongest
  candidate if a DB use case appears.
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

### Array literals need an explicit type; arrays of record literals fail to parse — *gap*
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
Still open (compiler): infer homogeneous element types, and fix the nested
record-literal-in-array parse (`Expected ']' to close array literal`).

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

*(Also worth a small harness fix, not a language gap: the idea-miner classifies a runtime failure whose
message lands on STDOUT — e.g. `Aether @post failed in f` from a legitimately-violated contract — as a
"silent" failure, because it only inspects stderr+diagnostics. It should also scan stdout for the known
runtime-error prefixes.)*

### Models write `fn m(self: T)` free functions instead of methods — *idea (verified, 2 models)*
Folding GLM-5-Turbo/5.2 into pass 2 (both very clean: 9/10 programs compiled): the one gap was models
defining a *free-standing* function with an explicit `self` parameter and referencing `self` in a
contract — `@pre length(self.data) < self.capacity  fn push_safe(self: Stack, val: Int) -> Void` →
`[SCOPE-001] identifier 'self' not in scope`. Verified: a PROPER method (inside the `type` block,
implicit `self`) with `@pre self.v >= 0` compiles+runs fine; only the explicit-`self`-param free-function
form fails. Hit by `glm-5.2` and (pass 2) `mistralai/devstral-small-2-2512`. **Action:** guide note in
the method/constructor section — methods live INSIDE the `type` block with implicit `self`; do not write
`fn m(self: T)` free functions; `@pre`/`@post` on a proper method may reference `self`.
