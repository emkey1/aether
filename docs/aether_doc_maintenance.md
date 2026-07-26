# Aether Doc Maintenance Notes

This file is for human maintainers. It should not be appended to the LLM-facing
reference documents.

## The three LLM-facing guides

| Guide | Budget | Audience |
|---|---|---|
| `aether_for_llms_with_small_contexts.md` | ~9K tokens | 8–16K context models |
| `aether_for_llms_medium_contexts.md` | ~11K tokens, hard ceiling 13K | ~32K context models |
| `aether_for_llms_and_others.md` | no ceiling | frontier models (256K–1M) |

The medium guide targets **about a third of a 32K window**, leaving room for the
prompt, a reasoning trace, the emitted program, and one repair round. If it grows
past ~13K tokens it stops being the thing it exists to be — cut, do not creep.

Because the medium guide now covers the 32K tier, **the full guide is free to
grow**. It no longer has to be the document that fits everywhere, so prefer
adding depth there over compressing it.

### What the medium guide must preserve

Everything in the small-context checklist below, plus the depth that makes rules
stick rather than merely be stated:

- a violating shape (`✗ ... ✓ ...`) alongside each Highest-Value Rule
- the conversion, arity, and math tables **near the top** — ordering is load
  bearing, see below
- worked, self-contained examples for TOON classification, nested extraction, a
  record with methods, and a method taking caller context
- **Writing what the surface does not give you** — the canonical loops for sort,
  join, case conversion, contains/starts-with/replace, filter/map, sum/mean, and
  a lookup table. `BUILT-001` forbids inventing helpers; this section is where a
  model is sent instead, and its absence is what drives helper invention.

### Two invariants for the medium guide

**Ordering survives truncation.** Sections are ordered so a truncated copy loses
the capability appendix (files, HTTP, sockets, tasks) first and the conversion
and math tables last. A 2026-07-26 transcript recorded a model that hand-rolled a
clamp because `min`/`max`/`clamp` sat at line 653 of the full guide, past where
its copy was cut. Do not reorder these toward the front:

1. Highest-Value Rules → 2. Never Generate → 3. Canonical vs accepted →
4. Conversions/arities/math → 5. Safe generation algorithm

**LLM-applicable content only.** The medium guide is audited against four
categories, all of which must return zero hits:

- implementation or provenance (PSCAL, rea, the VM, yyjson, curl, opcodes,
  bytecode) — including inside *string literals in templates*, which models copy
  verbatim into their own output
- build and human workflow (CMake flags, `./build`, `ctest`, "run the compiler")
- history and rationale ("historically", "used to", "we chose", "heritage")
- cross-document or repo meta (`docs/`, `README`, `examples/`, benchmark names,
  references to the other guides)

Runtime-discovery advice is a special case: `builtin_info` / `builtins_json` are
*runtime* calls a one-shot generator cannot make, so the medium guide states that
plainly and never offers discovery as a fallback for an unlisted name.

### Verifying the guides

Every ` ```aether ` block in the medium guide compiles. Extract each block, wrap
fragments in a function with a context prelude supplying the names they reference,
append a trivial `main` to declaration-only blocks, and run `aether --no-run`
over the lot. Two blocks are expected to fail and are allowlisted: the `TOON-001`
negative example, and the module-import example whose module is not on disk.

Beyond compiling, the recipes in **Writing what the surface does not give you**
are executed and their output checked — a sort or a `replaceFirst` that compiles
and computes the wrong answer would be worse than not shipping one. Avoid `...`
elisions inside fenced blocks anywhere in this guide; models copy them verbatim
and get a `SYN-001`.

## Small-context LLM doc extraction checklist

When updating `docs/aether_for_llms_with_small_contexts.md`, preserve these
items from the full reference:

- Highest-Value Rules
- Never Generate These
- Canonical vs accepted forms for high-risk syntax
- smallest useful program
- safe generation algorithm
- `fx` rules
- print/formatting rules
- conservative inference policy
- import rules
- TOON handle rules
- TOON root-shape rule
- TOON `_or` fallback warning
- tuple limitations
- repair rules
- validation checklist

Do not let the small-context document become a general overview. Its job is to
make generated Aether valid.

## Where to look next

Practical examples:

- `examples/base/README.md`
- `examples/showcase/README.md`

Implementation notes:

- `src/aether/README.md`
- `src/aether/DESIGN.md`

Best example files to copy from:

- `examples/base/hello`
- `examples/base/contracts`
- `examples/base/contract_layouts`
- `examples/base/inferred_decls`
- `examples/base/function_inference`
- `examples/base/object_inference`
- `examples/base/self_mutation`
- `examples/base/toon_access`
- `examples/base/toon_defaults`
- `examples/showcase/agent_report`

## Bottom Line

If you want valid Aether today, keep generated code compact, explicit about
effects, modest about inference, typed around TOON handles and extracted values,
careful about imports, conservative with tuples, and close to the existing
examples. That is still the fastest path for both humans and LLMs to produce
working Aether code.
