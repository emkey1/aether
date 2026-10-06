# CLAUDE.md — Aether

Aether is a small statically-typed language designed to be written by language
models. This repo is the front end (`src/aether/`) over the shared Rea engine
(`external/rea`) and the PSCAL VM (`external/pscal-core`). It is **public**:
never commit host names, fleet details, credential locations or session paths
(`tools/check_public_hygiene.py` fails on them).

## Before any work

- Checkouts lag `origin` across machines. Run `git fetch` and rebase first, and
  read `git status -sb` before committing.
- Verify any claim a doc makes against `./build/aether` before acting on it.
  Hand-written docs here have drifted before; generated and tested ones have not.

## Build, run, test

```sh
git submodule update --init --recursive   # external/rea, external/pscal-core
cmake -S . -B build && cmake --build build -j   # Release by default
./build/aether --no-cache prog.aether            # --no-run compiles only
ctest --test-dir build --output-on-failure      # run.sh, examples, hygiene lint
AETHER_BIN=$PWD/build/aether tests/run.sh        # the .aether corpus directly
```

Guide gates: `python3 tools/verify_guide_snippets.py docs/aether_for_llms_<guide>.md`
for each of `and_others`, `medium_contexts`, `with_small_contexts` (compile-only);
`python3 tools/gen_builtin_appendix.py` regenerates the full guide's appendix
(never hand-edit it).

## Where a diagnostic comes from

Search in this order: `src/aether/ast_parser.c` (parse, lowering, most coded
errors), `src/aether/ast_prepasses.c`, `src/aether/semantic.c`, then
`external/rea/src/rea/` (SCOPE-001, FIELD-002, module loading), then
`external/pscal-core/src/` (compiler and VM runtime errors). Aether-only
behaviour in the shared code sits behind `frontendIsAether()`.

## Shipping

- **Language version.** `VERSION` changes only when what programs compile or
  how they run changes: `tools/bump_version.py` plus a `CHANGELOG.md` entry.
- **Engine changes.** Commit on pscal-core (or rea) `main` and push, then bump
  `external/` here (aether's pscal-core pin wins over rea's), then the
  umbrella's `components/` gitlinks. Batch engine fixes: one pin bump and one
  VERSION bump per release, not one per fix.
- **No Aether VM.** Never add a separate VM, bytecode or runtime for Aether.

## The guides

`docs/aether_for_llms_and_others.md` (full), `..._medium_contexts.md` (medium,
the benchmark's tier) and `..._with_small_contexts.md` (small) are the product.
Read `docs/aether_doc_maintenance.md` before editing one: stamps and changelog
rows, the medium guide's 15,000-token ceiling (measure, never estimate), the
paired-cut rule, and the LLM-only content audit. A guide freeze is in effect
until the current stamps have a benchmark baseline recorded (decision register
D3).

## Doc map

| Tier | Docs |
|---|---|
| As-built | `docs/aether_architecture_and_rationale.md` |
| Reference | the three guides |
| Decisions and ledgers | `docs/aether_decisions.md`, `CHANGELOG.md`, `docs/aether_guide_changelog.md`, `docs/ideas_and_todo.md` |
| Results | `docs/aether_guided_benchmark.md`, `docs/aether_specialization_findings.md` |
| Historical (do not trust for current behaviour) | `src/aether/DESIGN.md`, `src/aether/README.md`, `docs/parser_roadmap.md`, `docs/text_zero_based_migration_plan.md`, `docs/archive/` |
| Generated | the builtin appendix at the end of the full guide |

The benchmark harness and the fine-tuning corpus live in the umbrella repo
(`emkey1/pscal`: `tools/aether_doc_bench.py`, `Tests/aether_doc_bench/`,
`Tests/aether_specialization/`).
