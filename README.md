# aether

<img src="assets/aether-logo.svg" alt="Aether logo" width="88" align="right"/>

**Aether** is a contract- and effect-typed front end for the **PSCAL** VM. It
lowers to [Rea](https://github.com/emkey1/rea): Aether parses its own source,
runs its own semantic analysis (effects, purity, contracts, ...), and emits the
shared PSCAL AST, which the shared bytecode compiler lowers and the PSCAL VM
runs.

## Built on the Rea engine

Aether carries no VM, code generator, or general front-end engine of its own. It
reuses the shared Rea front-end engine and installs its overrides — parser,
semantic analysis, diagnostics, source rewriting — through the engine's hook
seam (`rea`'s `src/rea/frontend_hooks.h`) at startup via
`aetherInstallFrontendHooks()`. The engine itself has no dependency on Aether.

The dependency chain is **aether → rea → pscal-core**, all wired automatically
through CMake `FetchContent`.

## Build

```sh
cmake -S . -B build      # fetches rea (+ pscal-core) and builds aether
cmake --build build -j
./build/aether --no-cache program.aether
```

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
AETHER_BIN=./build/aether tests/run.sh
```

## Examples

Runnable programs live in [`examples/`](examples/), from `base/hello` through the
`showcase/` agent demo:

```sh
./build/aether --no-cache examples/base/hello
./build/aether --no-cache examples/showcase/agent_report
```

## Docs

In-depth language documentation is in [`docs/`](docs/):

- [`aether_architecture_and_rationale.md`](docs/aether_architecture_and_rationale.md): design and rationale
- [`aether_for_llms_and_others.md`](docs/aether_for_llms_and_others.md): the full language guide
- [`aether_for_llms_with_small_contexts.md`](docs/aether_for_llms_with_small_contexts.md): a condensed guide

See [`src/aether/DESIGN.md`](src/aether/DESIGN.md) for the front-end internals.

## Models and benchmarks

Training recipes, fine-tuning operations, and benchmark analyses for the language
models that write Aether live in a companion repo:
[**aether-infrastructure**](https://github.com/emkey1/aether-infrastructure).
