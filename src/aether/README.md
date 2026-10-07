# src/aether: the Aether front end

This directory is the Aether front end: source pre-passes (`ast_prepasses.c`), the
recursive-descent parser that builds the shared PSCAL AST (`ast_parser.c`, with
`ast_lower.c`, `ast_types.c` and `ast_checks.c`), the Aether semantic checks
(`semantic.c`) and the hook registration (`state.c`). The earlier text of this file
described the line-based rewriter (`translate.c`) and its debug flag, both deleted
on 2026-07-01; that text survives in git history only.

- How the pipeline is built, and why: `docs/aether_architecture_and_rationale.md` §2.
- What the language accepts: the fixtures in `tests/` and the snippet gates.
- How to write Aether: the three guides in `docs/`.
