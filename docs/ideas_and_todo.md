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
