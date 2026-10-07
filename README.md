# Aether

<img src="assets/aether-logo.svg" alt="Aether logo" width="88" align="right"/>

**Aether is a small, statically-typed programming language designed to be
written by language models.** It compiles and runs real programs — typed records
with methods, `@pure` contracts, effect-scoped I/O, integer and real arithmetic,
and first-class JSON/TOON parsing — through a small, regular grammar. What sets
it apart is the design goal: a model should be able to produce *valid, correct*
Aether **with no reference guide in its prompt**, and the language is shaped by
what capable models actually reach for, not by taste.

```aether
@pure
fn classify(score: Int) -> Text {
    if score >= 90 { ret "distinction"; }
    if score >= 70 { ret "merit"; }
    ret "pass";
}

fn main() -> Void {
    fx { println("Ava: ", classify(92)); }   // Ava: distinction
    ret;
}
```

When a capable model consistently reaches for a construct that does not exist —
or trips on one that does — that is treated as a language bug and fixed. The
benchmark, not opinion, is the instrument: see the
[findings](docs/aether_specialization_findings.md) and the
[design rationale](docs/aether_architecture_and_rationale.md).

## You don't have to fine-tune a model to use it

Fine-tuning is the research frontier, but it isn't the cost of entry. The
concise guide
([`aether_for_llms_with_small_contexts.md`](docs/aether_for_llms_with_small_contexts.md),
~955 lines) pasted into the prompt gets a frontier model that has *never seen
Aether* writing valid, correct programs a surprising fraction of the time — the
grammar is small and regular enough to pick up in-context.

And the loop closes. When a model slips, the compiler doesn't just reject the
code: it names the mistake and tags it with a stable error code — `FX-001` for an
effect used outside an `fx` block, `ANN-001` for a misplaced contract, `TOON-001`
for a malformed TOON handle, and so on. Those same codes are section headings in
the guide, so the diagnostic points straight back to the paragraph that explains
the fix. A model can read its own error and correct the program on a second pass.

Measured: in the [guided benchmark](docs/aether_guided_benchmark.md), a 120B
model writes the **full 30-task v2 benchmark correctly** from the guide alone —
no fine-tuning (and in the broader archived sweep, so does a 122B).

## How it runs

Aether is a contract- and effect-typed front end for the **PSCAL** VM. It lowers
to [Rea](https://github.com/emkey1/rea): it parses its own source, runs its own
semantic analysis (effects, purity, contracts), and emits the shared PSCAL AST,
which the shared bytecode compiler lowers and the PSCAL VM runs. It carries no VM
or front-end engine of its own — it reuses the shared Rea engine and installs its
overrides (parser, semantic analysis, diagnostics, source rewriting) through the
engine's hook seam (`rea`'s `src/rea/frontend_hooks.h`) via
`aetherInstallFrontendHooks()`. The dependency chain is **aether → rea →
pscal-core**. `rea` and `pscal-core` are vendored as git submodules under
`external/` and wired in through CMake `FetchContent` (`SOURCE_DIR`).

## Build

`rea` and `pscal-core` are vendored as git submodules under `external/`, so clone
recursively. A plain clone leaves `external/` empty and the build fails.

```sh
git clone --recurse-submodules https://github.com/emkey1/aether.git
cd aether
cmake -S . -B build      # builds aether against external/{rea,pscal-core}
cmake --build build -j
./build/aether --no-cache program.aether
```

Already cloned without `--recurse-submodules`? Run `git submodule update --init
--recursive` once, then build.

Networking (`http*`, `socket*`) is on by default and needs **libcurl** headers
present at configure time — `brew install curl` on macOS, `libcurl4-openssl-dev`
or equivalent on Linux. It is documented language surface and `tests/run.sh`
exercises it, so the default build is the one the docs describe. To build a
network-less binary instead, configure with `-DAETHER_ENABLE_CURL=OFF`.

## Install

```sh
cmake --install build --prefix /usr/local
```

This puts the `aether` binary in `<prefix>/bin`, the example programs in
`<prefix>/share/aether/examples`, and the language docs in
`<prefix>/share/doc/aether`. The fetched dependencies (rea, pscal-core) guard
their install rules to standalone builds, so only Aether's own artifacts are
installed.

## Test

The `.aether` conformance corpus lives in [`tests/`](tests/) and runs under CTest:

```sh
ctest --test-dir build --output-on-failure
```

You can also run it directly against any binary by pointing `AETHER_BIN` at it
(this is how the umbrella build exercises the same corpus):

```sh
AETHER_BIN="$PWD/build/aether" tests/run.sh
```

## Examples

Runnable programs live in [`examples/`](examples/), from `base/hello` through the
`showcase/` agent demo:

```sh
./build/aether --no-cache examples/base/hello
./build/aether --no-cache examples/showcase/agent_report
```

## SDL graphics (optional)

Aether can drive the PSCAL VM's SDL backend (windows, 2D drawing, input). It is
off by default; build with `-DAETHER_ENABLE_SDL=ON` and see
[`examples/sdl/`](examples/sdl/) for a runnable demo and the build details.

## Docs

New to the repo, or an agent working in it: start with [`CLAUDE.md`](CLAUDE.md).

The three LLM-facing guides are the product (sizes in o200k tokens, 2026-10-06):

- [`aether_for_llms_and_others.md`](docs/aether_for_llms_and_others.md): the full guide (28,961 tokens; frontier contexts), ending in a generated builtin inventory
- [`aether_for_llms_medium_contexts.md`](docs/aether_for_llms_medium_contexts.md): the working guide (14,985 tokens, hard ceiling 15,000; ~32K contexts). The benchmark's main tier
- [`aether_for_llms_with_small_contexts.md`](docs/aether_for_llms_with_small_contexts.md): the concise guide (11,898 tokens; ~16K contexts, ceiling 8,000 to be reached by the next small-guide pass)

Maintainer docs in [`docs/`](docs/):

- [`aether_architecture_and_rationale.md`](docs/aether_architecture_and_rationale.md): how it is built and why (as-built)
- [`aether_doc_maintenance.md`](docs/aether_doc_maintenance.md): the rules for editing the guides (budgets, stamps, gates)
- [`aether_spec.md`](docs/aether_spec.md): the normative language spec (skeleton), whose examples run as a test
- [`aether_decisions.md`](docs/aether_decisions.md): the decision register
- [`CHANGELOG.md`](CHANGELOG.md), [`aether_guide_changelog.md`](docs/aether_guide_changelog.md), [`ideas_and_todo.md`](docs/ideas_and_todo.md): the language versions, the guide stamps, the backlog
- [`aether_guided_benchmark.md`](docs/aether_guided_benchmark.md), [`aether_specialization_findings.md`](docs/aether_specialization_findings.md): benchmark results with and without a guide

Historical, kept for the record and not a description of today's compiler:
[`src/aether/DESIGN.md`](src/aether/DESIGN.md) (the original design vision),
[`parser_roadmap.md`](docs/parser_roadmap.md) and [`docs/archive/`](docs/archive/).
The pipeline as built is §2 of the
[architecture and rationale](docs/aether_architecture_and_rationale.md#2-the-compile-pipeline-as-actually-built).

## Models and benchmarks

Training recipes, fine-tuning operations, and benchmark analyses for the language
models that write Aether live in a companion repo:
[**aether-infrastructure**](https://github.com/emkey1/aether-infrastructure).
