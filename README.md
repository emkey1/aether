# aether

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
