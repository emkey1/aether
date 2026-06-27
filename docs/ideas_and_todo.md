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
