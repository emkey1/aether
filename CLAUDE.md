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
ctest --test-dir build --output-on-failure      # everything below, ~2.5 min
ctest --test-dir build -LE invariance           # the same minus the 2-min reformat lap
AETHER_BIN=$PWD/build/aether tests/run.sh        # the .aether corpus directly
```

CTest runs `tests/run.sh`, the examples lap, the hygiene lint, and the guide and
doc gates: the snippet gates for each guide (compile-only), the builtin appendix
(`tools/gen_builtin_appendix.py --check`; regenerate it, never hand-edit it), the
staged token ceiling (needs tiktoken: configure with
`-DAETHER_DOCS_PYTHON=<python with tools/requirements-docs.txt>`, otherwise
Skipped), guide stamps against `docs/guide_stamps.json`, commit references in
docs, and the reformat-invariance lap (`tests/reformat_invariance.known` may only
shrink). `aether_guide_audit` and `aether_diag_codes` report only until the first
guide pass. Bump a guide with `python3 tools/bump_guide_version.py <guide>`, which
prints the changelog row to commit with it; the pre-commit hook only warns.
`-DAETHER_STRESS_TESTS=ON` adds the par stress lap (minutes, every core).

`aether_backlog` (`tools/run_backlog.py`, label `backlog`) pins today's wrong
behaviour of each open plan item as a probe in `tests/backlog/`, by outcome
class. A fix that changes a probe's class fails it: in the same commit, update
the ledger entry and guide lines its `manifest.tsv` row names, then move the
probe into `tests/` as a regression fixture. `ctest -LE backlog` skips it
during WIP.

**New fixtures go in `tests/fx/`, not in `tests/run.sh`.** A fixture is
`tests/fx/<dir>/<name>.aether` plus sidecars: `.out` (exact stdout),
`.unordered` (compare `.out` as a multiset of lines, for `par`), `.err`
(required stderr substrings; a `!` line is forbidden), `.codes` (the exact
diagnostic code set; `code: null` fails), `.rc`, `.in`, `.flags`, `.cap`
(curl, yyjson, openai, sdl). `python3 tools/run_fixtures.py --update <name>`
writes `.out`/`.rc`/`.codes` and prints the diff: review it before committing.
CTest runs one `aether_fixtures_<dir>` per directory (re-configure after adding
one), and `tests/run.sh` runs them all at its end.

`aether_guide_run` and `aether_examples_run` run what the compile gates only
compile: every complete guide program against `docs/guide_goldens.json`, the
recipe drivers in `tests/guide_recipes/` against hand-written `.out` files, and
every example (goldens in `tests/example_goldens/`). `--update` on
`tools/verify_guide_snippets.py --run` or `tests/run_examples.sh --run`
re-blesses the goldens; read the printed diff before committing.

`aether_replay` (`tests/run_replay.sh`) replays about 220 real model-written
benchmark programs frozen in `tests/replay/`. Regenerate them only with
`python3 tools/export_replay.py --umbrella <umbrella checkout>`; a declared
language break goes in `tests/replay/WAIVED` with its CHANGELOG version.

`tools/diag_census.py` measures failing programs' diagnostics (coded, hinted,
uncoded backend lines, JSON `code: null`, stderr over 1,200 chars) over the
umbrella's archived attempts, the `_fail` fixtures and deletion mutants. The
baselines are in `tests/diag_census/`; after each diagnostics batch, re-run it
with `--umbrella <checkout> --compare tests/diag_census/<last>.csv` and commit
the new CSV and table. `aether_diag_census` (label `metric`) is a quick report.

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
- **Release train.** Gate candidate pins with `tools/pin_gate.sh`, move them with
  `tools/bump_pins.sh` (never by hand); the policy is the "Release train"
  section of `docs/aether_doc_maintenance.md`.
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
| Reference | the three guides; `docs/aether_spec.md` (normative skeleton, examples run by `ctest -L spec`); `tests/surface/registry.json` (surface synonyms) |
| Decisions and ledgers | `docs/aether_decisions.md`, `CHANGELOG.md`, `docs/aether_guide_changelog.md`, `docs/ideas_and_todo.md`; `results/decisions/` (the measured evidence behind decision rows, with the commands that produced it) |
| Results | `docs/aether_guided_benchmark.md`, `docs/aether_specialization_findings.md` |
| Historical (do not trust for current behaviour) | `src/aether/DESIGN.md`, `src/aether/README.md`, `docs/parser_roadmap.md`, `docs/text_zero_based_migration_plan.md`, `docs/archive/` |
| Generated | the builtin appendix at the end of the full guide |

The benchmark harness and the fine-tuning corpus live in the umbrella repo
(`emkey1/pscal`: `tools/aether_doc_bench.py`, `Tests/aether_doc_bench/`,
`Tests/aether_specialization/`).
