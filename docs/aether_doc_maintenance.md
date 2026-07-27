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
adding depth there over compressing it. The builtin appendix is the first thing
added on that basis: it would have been unaffordable when the full guide had to
serve constrained contexts, and it costs ~3K tokens of a budget that no longer
has a ceiling.

### The builtin appendix is generated

`docs/aether_for_llms_and_others.md` ends with a complete builtin inventory
between `<!-- BEGIN GENERATED BUILTIN INVENTORY -->` / `<!-- END ... -->`
markers. **Do not hand-edit it.** Regenerate after adding, renaming or removing
a builtin:

```sh
python3 tools/gen_builtin_appendix.py
```

It reads `builtins_json(true)` from the built compiler, so it reflects that
build. Two tiers, and the second is the point: names with a documented signature
are safe to call, while name-only entries confirm a name is real *without*
licensing a guess at its arguments. Anything in neither tier does not exist,
which is what makes BUILT-001 checkable instead of an assertion.

The generator drops host-registered `3d` / `user` categories (demo and graphics
surfaces that are not the language) and internal backend spellings (any name
that is another entry's `backend_name`, since the Aether-facing alias is what
should be written). If a build registers a new host category, add it to
`SKIP_CATEGORIES` rather than letting it into the guide.

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

**All three** guides are gated, not just the medium one:

```sh
for g in and_others medium_contexts with_small_contexts; do
    python3 tools/verify_guide_snippets.py docs/aether_for_llms_$g.md || exit 1
done
```

It extracts every ` ```aether ` block, wraps fragments in a function with a
context prelude supplying the names prose snippets reference, appends a trivial
`main` to declaration-only blocks, and runs `aether --no-run` over the lot. A
fragment that fails is retried inside an `fx { }` block, since a snippet quoted
from prose may be a bare `println(...)` whose surrounding text already
established it is inside one. Exit status is nonzero on any unexpected result,
so it can gate.

Blocks that *must* fail are allowlisted by a distinctive substring in
`EXPECT_FAIL`, and the count of them is asserted — so a deliberate negative
example silently starting to compile is also caught. They fall into three kinds:
negative examples (a TOON getter handed a `ToonDoc`), prose sketches using `...`
elision, and module examples whose module is not on disk.

Two maintenance notes. When a fragment references a new name, add it to
`CONTEXT` rather than dropping the block from the check. And keep `EXPECT_FAIL`
keys **specific** — a key broad enough to match a block in another guide will
mark a perfectly good snippet as expected-to-fail and hide a real regression.

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
