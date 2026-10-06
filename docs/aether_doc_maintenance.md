# Aether Doc Maintenance Notes

This file is for human maintainers. It should not be appended to the LLM-facing
reference documents.

## The three LLM-facing guides

| Guide | Budget | Audience |
|---|---|---|
| `aether_for_llms_with_small_contexts.md` | ~9K tokens *(stated; measures 11.4K — see below)* | 8–16K context models |
| `aether_for_llms_medium_contexts.md` | hard ceiling **15K tokens** | ~32K context models |
| `aether_for_llms_and_others.md` | no ceiling | frontier models (256K–1M) |

Each guide carries its own `YYYY-MM-DD-N` stamp in its front matter. That stamp
is what `aether_guided_benchmark.md` records per row, so a guide whose text
changes under an unchanged stamp, or a stamp with no changelog row, leaves a
score nothing can be attributed to. Every guide edit, medium included:

1. Edit the guide.
2. Run `python3 tools/bump_guide_version.py docs/aether_for_llms_<guide>.md`.
   It computes the next stamp from `git show HEAD:<guide>`, so it is
   idempotent: a second run, or a run after an earlier one, leaves the stamp
   alone. It writes the stamp's entry in [`guide_stamps.json`](guide_stamps.json)
   (body sha256, o200k, date) and prints the changelog row, with net tokens
   when tiktoken is installed.
3. Paste the row at the top of the guide's table in
   [`aether_guide_changelog.md`](aether_guide_changelog.md) and write its
   change text. Leave the commit column as `(this commit)`: the row lands in
   the same commit as the stamp, and `tools/check_guide_stamps.py --commits`
   derives the hash from git when anyone needs it, so no follow-up commit
   fills it in.
4. Commit the guide, `guide_stamps.json` and the row together.

`ctest` enforces this through `aether_guide_stamps`
(`tools/check_guide_stamps.py`): each guide's body hash (its bytes without the
stamp line) must equal the manifest entry for its stamp; every manifest stamp
needs a changelog row, the current one first; a commit hash typed into such a
row must be the commit that introduced the stamp; and `VERSION` must equal the
top `CHANGELOG.md` entry. A stamp without a text change is refused:
harness-only bumps are retired, because the benchmark MANIFEST's harness and
prompt-template sha256s now identify the harness. Changelog rows written before
this rule keep their meaning. `guide_stamps.json` doubles as the stamp → sha256
map a benchmark row can record beside the stamp.

The `pre-commit` hook in `tools/hooks` no longer bumps anything. With
`core.hooksPath` pointed there it only warns when a staged guide still carries
HEAD's stamp; it never edits, stages or blocks, and the gate does not depend on
it.

The medium guide must leave a ~32K window room for the prompt, a reasoning trace,
the emitted program, and one repair round. **Hard ceiling: 15K tokens**, measured
— roughly half the window, leaving ~17K for everything else. If it grows past
15K it stops being the thing it exists to be — cut, do not creep.

**Measure the budget, do not estimate it.** A `chars / 4` estimate undercounts
these guides by roughly 16% — they are dense with code fences, tables, and
backticked identifiers, all of which tokenize badly. Measured with `o200k_base`
(`cl100k_base` is within 0.5%):

| Guide | ceiling | 2026-08-06 | 2026-08-11 (SCOPE-001) | 2026-08-11 (ORDER-001) | 2026-08-11 (ORDER-001 retired) | 2026-09-05 | headroom |
|---|---|---|---|---|---|---|---|
| small | ~9K *(stated, not re-baselined)* | 11,369 | 11,613 | 11,631 | 11,534 | 11,898 | −2,898 |
| medium | 15K | 14,350 | 14,922 | 14,922 | 14,849 | 14,985 | 15 |
| full | none | 25,646 | 26,960 | 27,099 | 26,950 | 28,892 | n/a |

The 2026-09-05 additions (foreach and `step` loops, the word operators, array
`==`, the rewritten reserved-word and KEY-001/NEST-001 rules, TYPE-002 with the
`Float`/`String` aliases, `setlength`, the cast spellings, `toon_key_or` /
`toon_null`) landed against the paired-cut constraint. The cuts: the KEY-001 and NEST-001
examples are one or two lines, the cast spellings sit in the existing `int(x)`
sentence instead of a table row, the reserved-word bullet shrank because fewer
words are reserved, the precedence paragraph is a sentence, the array-printing
and getter-on-a-`ToonDoc` examples are one-liners (the former now shows the
foreach form), the `shout` iteration example is a sentence, and the "a `type`
may be declared after another `type`" bullet went (ORDER-001's retirement made
it a statement about an ordering that no longer exists), the duplicated
single-`println` block under Effects went, and the inference paragraph lost its
examples list. The
medium guide measures 14,985, under the 15,000 ceiling by 15 tokens.

Both constrained guides had silently drifted over their old budgets, because the
`chars / 4` estimate was flattering. The medium ceiling was **re-baselined from
13K to 15K on 2026-08-06** against real counts rather than trimming the guide.

**Re-measure before every medium-guide edit, not just after.** Between
2026-08-06 and 2026-08-11 the medium guide grew 572 tokens across several
edits while this table still advertised 650 tokens of headroom, so the SCOPE-001
addition on 2026-08-11 was planned against a number that had already been spent.
It fit, with 78 tokens to spare. **The next medium-guide addition of any size
needs a paired cut** — this is now a hard constraint, not a caution.

The ORDER-001 revision was the first edit made under that constraint, and it
held for medium: 14,922 in and 14,922 out, across two rounds of edits. The
paired cut was the "capture-free way to parallelize user code" tail of the
`par` section, whose actual steer was already carried verbatim by the
FUNC-001 entry in the rule list. **Duplication between a rule and the section
that elaborates it is the cheapest place to look for a cut** — it is the one
kind of deletion that costs no information.

ORDER-001 was then **retired outright** the same day, once the compiler bug that
justified it was fixed (language `2026-08-11-3`): a `par` branch now resolves its
target like any other call, so top-level order is free with no exception, and a
rule stating a non-constraint does not earn a slot in a scarce list. Its one
still-true clause (locals need `let` before use) was already carried by
SCOPE-001 in all three guides, so the deletion cost nothing. Medium gave back 73
tokens (14,922 → 14,849), taking headroom from 78 to 151. **A rule that exists
to warn about a compiler defect should be deleted when the defect is fixed, not
softened** — and the paired-cut constraint above still binds the next addition;
this repayment does not license a spend.

Note the small guide grew (+18) in the same revision. It has no enforced
ceiling to pair against, only the aspirational ~9K below, so cuts there are
opportunistic rather than mandatory. That asymmetry is deliberate but it is
also why the small guide keeps drifting; see below.

The small guide's ~9K is **still the old estimate-derived number** and it
measures 11,631. It has not been re-baselined and has not been trimmed; the
figure in the table above is aspirational, not a measured budget. Resolve it the
same way — pick a real ceiling or cut to the stated one — before relying on it.

To reproduce these counts, run the gate that enforces them
(`ctest -R aether_guide_tokens` runs the same script):

```sh
pip install -r tools/requirements-docs.txt   # tiktoken; tokenizers is optional
tools/fetch_tokenizers.sh                    # o200k_base + Qwen3.5 into ~/.cache/aether-tokenizers
python3 tools/check_guide_tokens.py          # prints the table, writes build/guide_sizes.md
```

It measures whole documents. Until the first guide pass it **fails only when
the medium guide exceeds 15,000 o200k tokens**; small, full and every other
tokenizer are reported. From the first guide-pass commit the medium ceiling is
counted in the largest cached cohort tokenizer (max(o200k, Qwen3.5, ...) ≤
15,000), falling back to o200k ≤ 14,100 when only o200k is cached, because
o200k undercounts the Qwen cohort by about 6%. That flip is one constant
(`STAGE`) in the script; `--stage g1` previews it. Without tiktoken, or with
o200k_base not cached, the test reports **Skipped**, never Passed: it downloads
nothing unless run with `--allow-download`. The core/library split (Files,
HTTP, Sockets, Tasks/AI and the appendices count as library) is reported and
never gated.

Measured 2026-10-06 at `a208b98`, whole documents (Qwen3.5 is the cached
Qwen3.5/3.6 `tokenizer.json`, sha256 `87a7830d63fc`):

| Guide | stamp | o200k | Qwen3.5 | core (o200k) | library (o200k) |
|---|---|---|---|---|---|
| small | 2026-09-05-1 | 11,898 | 12,633 | 10,019 | 1,879 |
| medium | 2026-09-05-1 | 14,985 | **15,913** | 13,757 | 1,228 |
| full | 2026-10-06-1 | 28,961 | 30,661 | 21,622 | 7,339 |

Medium is already over 15,000 in Qwen3.5, the tokenizer of the local cohort it
serves; the gate warns about that now and enforces it from the first guide
pass. Devstral's Tekken and DeepSeek-Coder counts are not measured: neither
tokenizer was available to fetch.

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
build. `--check` renders the same region and compares it with the file instead
of writing it; that is the CTest `aether_builtin_appendix`, so an appendix that
drifts from the compiler fails `ctest` until it is regenerated (and the full
guide's stamp bumped). The canonical generator build is the standalone default
(CURL on, SDL off); on an SDL build, which registers the graphics builtins for
real, `--check` reports Skipped rather than a false diff. Two tiers, and the
second is the point: names with a documented signature
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

CTest runs the guide gates. `ctest -R 'aether_guide|aether_builtin_appendix'`
selects them, and they are part of the default `ctest -LE 'stress|metric'`:

| CTest | What it checks | Fails when |
|---|---|---|
| `aether_guide_snippets_full`, `_medium`, `_small` | every ` ```aether ` block in that guide, compiled `--no-run` by `tools/verify_guide_snippets.py` | a block does not compile, or an `EXPECT_FAIL` block does |
| `aether_builtin_appendix` | the full guide's generated appendix against what this build registers (`tools/gen_builtin_appendix.py --check`) | any difference; Skipped on a non-canonical (SDL) build |
| `aether_guide_tokens` | whole-document token counts (`tools/check_guide_tokens.py`, above) | medium over 15,000 o200k; Skipped without tiktoken |

Still manual, because no gate does them: running complete programs and the
recipes (the snippet gate compiles, it does not run; see below), and the
four-category content audit above. To run one guide's snippet check by hand:
`python3 tools/verify_guide_snippets.py docs/aether_for_llms_medium_contexts.md`
(`AETHER_BIN` defaults to `build/aether`).

The snippet check extracts every ` ```aether ` block, wraps fragments in a
function with a context prelude supplying the names prose snippets reference,
appends a trivial `main` to declaration-only blocks, and runs `aether --no-run`
over the lot. A
fragment that fails is retried inside an `fx { }` block, since a snippet quoted
from prose may be a bare `println(...)` whose surrounding text already
established it is inside one. Exit status is nonzero on any unexpected result,
so it can gate.

**It compiles; it does not run.** Because the sweep is `--no-run`, a snippet
that compiles clean and then aborts at runtime passes the gate. That is not
hypothetical: from `091e4f0` (2026-07-26) to `652fd4c` (2026-08-11) the medium
guide's `par` example named its type `Tally` and its function `tally`, and on
the 2026-08-11 compiler that case-insensitive collision aborts with a VM
slot-window error the moment it executes. Sixteen days of green snippet checks
across every edit in between, because none of them ran it. (How much of that
window the compiler was actually broken for is unmeasured — the collision bug
was found by this route, not bisected.) When you add or edit a **complete program**
(one with its own `main`, as opposed to a prose fragment), run it once by hand
and look at the output before committing. Note also that `CONTEXT` itself
carries a `type Tally` / `fn tally` pair, so it is not a safe model to copy.

Blocks that *must* fail are allowlisted by a distinctive substring in
`EXPECT_FAIL`, and the count of them is asserted — so a deliberate negative
example silently starting to compile is also caught. They fall into three kinds:
negative examples (a TOON getter handed a `ToonDoc`), prose sketches using `...`
elision, and module examples whose module is not on disk.

Two maintenance notes. When a fragment references a new name, add it to
`CONTEXT` rather than dropping the block from the check. And keep `EXPECT_FAIL`
keys **specific** — a key broad enough to match a block in another guide will
mark a perfectly good snippet as expected-to-fail and hide a real regression.

The recipes in **Writing what the surface does not give you** are **not**
executed by any gate. They were run once by hand when they were written
(`091e4f0`), and nothing repeats that. A sort or a `replaceFirst` that compiles
and computes the wrong answer would be worse than not shipping one, so run any
recipe you touch with a small driver and check its output. Avoid `...` elisions
inside fenced blocks anywhere in this guide; models copy them verbatim and get
a `SYN-001`.

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
