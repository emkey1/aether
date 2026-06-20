# Aether showcase

This directory contains more substantial Aether examples than the small
one-file samples in `examples/base`.

Run them from the repository root with:

```sh
./build/bin/aether examples/showcase/agent_report
./build/bin/aether examples/showcase/gradebook
```

`agent_report` exercises the current Aether surface in combination:

- relative `use` imports across sibling Aether modules
- imported `const` values used through inferred `let` bindings
- exported helper functions from another Aether module
- `@pure`, `@pre`, and `@post`
- `type` blocks with methods and `self`
- inferred local bindings
- compact `loop` control flow
- `ToonDoc` / `ToonNode` parsing from a file payload
- typed TOON traversal and defaulted reads
- explicit `fx` output boundaries

If yyjson/TOON support is unavailable in the current build, the program exits
cleanly after reporting that capability gap.

`gradebook` is a second, self-contained showcase (no TOON or imports, so it runs
anywhere): a `type` with methods (`average`, `passed`), a top-level helper that
takes the record, parallel `Text[]` / `Int[]` data, accumulation, a real class
average, and formatted output.
