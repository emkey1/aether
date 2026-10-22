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
diagnostic code set; `code: null` fails), `.rc`, `.in`, `.flags`, `.files` (files copied beside the program, e.g. a `name.mod` module source as `name.aether`), `.cap`
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

`aether_manual_snippets` (`verify_guide_snippets.py --run-outputs`, the card's
gate) runs the human manual: every complete program must print exactly the
`text` block after it, with nothing on stderr, and a block fenced
`aether @ok rc=N warn=CODE`, `@reject=CODE` or `@trap` must match what a
terminal shows, stderr included. A change to a diagnostic's wording or a
program's output fails it; copy the printed diff into the manual's `text` block
after reading it. There are no hidden goldens and no `--update`.

`aether_replay` (`tests/run_replay.sh`) replays about 220 real model-written
benchmark programs frozen in `tests/replay/`. Regenerate them only with
`python3 tools/export_replay.py` (it reads `bench/results`); a declared
language break goes in `tests/replay/WAIVED` with its CHANGELOG version.

`tools/diag_census.py` measures failing programs' diagnostics (coded, hinted,
uncoded backend lines, JSON `code: null`, stderr over 1,200 chars) over the
archived attempts in `bench/results`, the `_fail` fixtures and deletion mutants. The
baselines are in `tests/diag_census/`; after each diagnostics batch, re-run it
with `--umbrella . --compare tests/diag_census/<last>.csv` and commit
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
paired-cut rule, and the LLM-only content audit. The guide freeze of decision
register D3 ended on 2026-10-13: the B0, B0-small and B0-full baselines are committed
under `bench/results/`, and guide pass 1 (G1) was adopted after a paired A/B. A guide
edit now follows the normal rules above; measure it before calling it an improvement.

`docs/aether_manual.md` is the human reference, not a fourth guide: no stamp, no
token ceiling and no benchmark baseline, so it may carry the build workflow,
history and design rationale the guides must leave out. Keep it in tutorial
order, and verify every claim in it against `./build/aether`.

## Doc map

| Tier | Docs |
|---|---|
| As-built | `docs/aether_architecture_and_rationale.md` |
| Reference | `docs/aether_manual.md` (the manual for people: tutorial order, rationale and history; every program runs under `aether_manual_snippets`); the three guides (for models); `docs/aether_spec.md` (normative skeleton, examples run by `ctest -L spec`); `tests/surface/registry.json` (surface synonyms) |
| Decisions and ledgers | `docs/aether_decisions.md`, `CHANGELOG.md`, `docs/aether_guide_changelog.md`, `docs/ideas_and_todo.md`; `results/decisions/` (the measured evidence behind decision rows, with the commands that produced it) |
| Results | `docs/aether_guided_benchmark.md`, `docs/aether_specialization_findings.md` |
| Historical (do not trust for current behaviour) | `src/aether/DESIGN.md`, `src/aether/README.md`, `docs/parser_roadmap.md`, `docs/text_zero_based_migration_plan.md`, `docs/archive/` |
| Generated | the builtin appendix at the end of the full guide |

## Benchmark and fine-tuning

They moved here from the PBuild umbrella (`emkey1/pscal`) on 2026-10-17 as a
plain move; their history before that is in the umbrella.

| Path | What |
|---|---|
| `bench/` | task suites (`tasks*.json`, byte-identical to the umbrella's: reports hash them), `results/` (archived reports, never rewritten), `replay_waivers.json`, `manifest_history/` (pre-move manifest versions for replay), the offline tests |
| `training/` | corpus candidates and manifests, seed pairs, datasets, run records; `training/config/` holds `families.json` and `runs/` |
| `docs/benchmarks/`, `docs/bench_runbook.md` | per-run reports; the bench and eval ops runbook (read before any sweep) |
| `tools/` | `aether_doc_bench.py` (the harness), `replay_bench.py`, `aether_oracle_check.py`, `instrument_check.sh`, `aether_specialization_*.py`, `aether_train_run.py`, `fleet_env.py` and the rest |

Archived reports and the dataset manifests keep the umbrella's repo-relative
paths (`Tests/aether_doc_bench/...`, `umbrella_commit`); readers map them with
`aether_doc_bench.legacy_repo_path()`. Quick checks, no GPU:

```sh
python3 bench/test_doc_bench_offline.py
python3 training/test_corpus_gates_offline.py && python3 training/test_train_run_offline.py
python3 tools/aether_specialization_validate_corpus.py --strict
bash tools/instrument_check.sh --lint-only          # --aether-bin build/aether for the full check
python3 tools/replay_bench.py --aether-bin build/aether   # paired replay over bench/results
```

Fleet facts stay out of this repo: which machines run the boards and the
training, their names, counts and hardware. Tracked files say "the GPU hosts" or
"the training host", and archived records use the aliases `host-a` .. `host-g`
(`bench/results/pack_reports.py` applies `FLEET_RULES` when packing reports, and
`--rescrub` to anything already committed); the `fleet-hardware` rule of
`tools/check_public_hygiene.py` fails on a slip. The tools read real names from
the private overlay `~/.config/pscal/fleet.env` (`tools/fleet_env.py`): a run
config's `"host": "host-c"` resolves through `AETHER_HOST_C`, the ssh user through
`AETHER_TRAIN_SSH_USER`, the remote GPU host through `AETHER_REMOTE_GPU_HOST` /
`AETHER_REMOTE_GPU_IP`, the distill proxy through `AETHER_DISTILL_TRA_TARGET` and
`AETHER_DISTILL_PROXY_HOST`. Shared-GPU jobs go through the T'Ra scheduler as the
umbrella's CLAUDE.md describes.
