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

### HTTP / sockets / SQLite — *HTTP done 2026-07-09-1; sockets/SQLite deferred*
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

### `swap` collides with the `swap` PSCAL vm_builtin, false-positive FX-001 — *deferred 2026-07-19, still open*
A user-defined `fn swap(...)` with no effectful calls in its body still trips
`[FX-001] call to 'swap' requires an fx block`, because `swap` is also a real,
effectful PSCAL `vm_builtin` (confirmed via `builtin_info("swap")`) and the
effect-checker matches call targets by name only, with no way to distinguish
"the user's pure function named swap" from "the builtin named swap." Repro:
a plain array-swap helper (`fn swap(xs: Int[], i: Int, j: Int) -> Void { ... }`,
no `fx`, no I/O) called from an unmarked `fn`, outside any `fx` block, still
faults. Workaround verified: rename the function (e.g. `swap_vals`) — no
functional loss, since `swap` was never anything but a name collision.
**Root cause not yet fixed:** the effect-checker needs to prefer a
same-scope/user-declared function over a same-named builtin when resolving a
call target for FX-001 purposes (shadowing), not fail closed on name alone.
Deferred at the user's explicit request while other corpus work was in
flight; not yet resumed.

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
1-based string indexing trips ~25 runtime errors (0-based prior): consider a
hint on out-of-range string ops; nested-fn declarations get a misleading
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
