# Plan: training data for Aether 1.0 and the first small model (drafted 2026-10-20)

Starts when Aether 1.0 beta 1 is tagged (D75, revised: the release that completes the 2026-10-20 review round,
2026-10-20-11, labelled `1.0beta1`; the final 1.0 comes later, after mileage).
Nothing below is generated before then.
- **Why wait:** D72 (renames), D70 (case-sensitive names) and D74 (Real formatting) change what correct programs
  look like.
- **D74 in particular:** distillation keeps an Aether solution only when it prints exactly what the Python program
  prints. Until Aether prints Reals the way Python does, float tasks mostly fail that test.

## 1. Bring the tooling to 1.0 (hours)

- **Graders:** confirm the GPU hosts, the scoring Mac and the workstation run 2026-10-20-11. Then re-run the zero-GPU
  replay of the published 27B (`qwen38-27b-r7`, seeds s2/s3) to record how it scores on 1.0. That is expected to be
  unchanged except where D74 changes a Real's printed form.
- **Scoring clone:** move the scoring Mac's clone to 1.0, so the medium guide used by the guided solver includes Maps.
- **Corpus gate:** run `--check-gates` on 1.0. The review session migrates the corpus and seed pairs with each
  change, so this should pass as is.

## 2. Distillation batch with maps (about 3-4 hours, mostly unattended)

- **Tasks:** `tools/aether_distill.py <batch> tasks --count 200 --with-maps`.
  - The new `MAP_CATEGORIES` (six keyed-data categories, each requiring a stated output order) appear twice in the
    rotation, so roughly half the batch is keyed data.
  - Task writing, Python and provenance go through T'Ra's large-open lane (MiMo-V2.6-Flash-RL, MIT), as before.
- **Freeze:** on the Python output. The prompt-contradiction filter applies.
- **Solve with both solvers, split from opposite ends, as for d3:**
  - the guided base (Qwen3.8-27B with the 1.0 medium guide, Ollama on the scoring Mac, under the one-model lock);
  - MiMo with the guide via T'Ra, plus a second MiMo pass on what the base cannot solve.
- **Harvest:** with per-pair solver tags. Check pairs, pack reports, commit.

## 2b. Standard-library breadth pack (owner, 2026-10-20: required before the next iteration)

A coverage check after D72 found the training data uses only 9 of the 60 renamed builtins.
- Unused: all 21 SQLite functions, 16 of 19 HTTP functions, all 13 socket functions, and others.
- The benchmark (193 reference solutions) and the published model's 457 programs use only `format_float` /
  `formatfloat`. So a fine-tune barely knows whole subsystems.

Before `v1-20261021` is built, author a targeted pack (an agent, as for run 6's pack; never from benchmark task text):
- **Scope:** every builtin family in the official builtin list, with priority on the never-used ones.
- **Verification:** every pair must compile and print its expected output under the dataset gates. That decides
  what can be included with real output:
  - SQLite against an in-memory database: create, insert with binds, queries, column accessors, error handling.
  - Sockets over loopback, if deterministic.
  - Environment and directory calls, with deterministic output only.
  - File operations in the sandbox; the D73 `try_` variants.
- **HTTP:** cannot run offline in the gate. Either compile-only pairs, if the builder gains such a mode, or pairs
  whose output does not depend on the network (building requests, setting headers, error handling with no server).
  Decide when authoring and record the choice.
- **Size:** about 5-10 pairs per family, so each name the model should know appears at least once or twice.
- **No old-name repair drills (owner, 2026-10-20):**
  - Models trained on 1.0 data start from stock Qwen and never see the removed names as outputs.
  - Every removed name is SCOPE-001 with a hint naming the replacement.
  - Drills would spend budget re-exposing retired vocabulary.
  - If porting pre-1.0 code becomes a real use, a few "update this program" pairs can be added then.
- **Prerequisite:** the review session's builtin-surface tests (requested 2026-10-20, before the 1.0-beta1 tag), so the
  names the pack teaches are known to be wired correctly.
- **Done (2026-10-20, on 1.0-beta2):** 84 pairs (`stdlib_*` ids) appended to `training/seed_instruction_pairs.json`.
  - Families: SQLite 14, HTTP 13, files 10, sockets 9, math 7, environment/arguments/stdin 6, tasks 4, collections
    4, clock 3, capability probes 3, ai_chat 2, random 2, conversion 2, strings 2, TOON 2, MStream 1.
  - Usage in the training data, counted over the 213 names in the guide appendix (its signature list plus its
    "documented earlier" list): 127 were used before the pack, 211 after it. All 60 D72 names are now used, against 9
    before.
  - Not taught: `task_status` (the appendix types it `Int`, the guide's prose says `Bool`, and `println` prints it
    as `true`) and `task_result` (its only useful targets are pre-D72 backend names such as `"dnslookup"`).
  - HTTP without a network: `data:` URLs, `file://` + `get_current_dir()`, a one-shot stub on 127.0.0.1 served from a
    `par` branch, and a refused connection to port 1. Each async request in flight has its own session: async jobs
    sharing a session crashed about 1 run in 100. Async byte counters are printed only as `>= 0`, because they can
    lag behind `http_is_done`.
  - Environment-dependent values are printed only as facts that hold on every machine: a variable is set, a
    fallback was used, a timing or random-draw range check passed. Stdin is read only on a branch that a
    command-line argument selects, and the gate passes no arguments.
  - Every pair passed on both the 1.0-beta2 build and pscal-core 18d2c80. The network, task and clock pairs were also
    run 50 times each under parallel load.

## 3. Dataset `v1-20261021`

- **Contents:** everything the run-7 dataset had, migrated to 1.0 by the review round, plus:
  - the 18 map seed pairs from D68;
  - the standard-library breadth pack (2b);
  - any further map pairs;
  - the new batch.
- **Python pairs:** capped at about 13% of instruction records (the run-7 share).
- **Build:** on the scoring Mac (`aether_train_run.py <run> dataset`). It must pass every gate.

## 4. Runs

1. **`qwen35-4b-v1` (config committed):**
   - Qwen3.5-4B with run 7's recipe. The model forces bf16 LoRA instead of 4-bit.
   - Score it on the same eight suites. The comparisons are:
     - its own guided baseline on 2026-10-19-3 (52/73 main, 13/53 held-out);
     - a re-measured guided baseline on 1.0.
   - The question it answers: how close does a 4B get without a guide?
2. **Teacher distillation for small models (after 4b-v1, one change per run):**
   - The published 27B writes Aether for many more tasks, verified on the 1.0 compiler and against Python, which
     gives the 4B far more examples than the guided solvers can produce.
   - The 27B was trained before 1.0, so its outputs pass only where 1.0 accepts them. The compiler filters the rest.
**Run order on the training host** (owner, revised 2026-10-20: the Hugging Face update ships within a day or two
of 1.0-beta1, so the 27B refresh goes first):
1. `qwen38-27b-v1`: the 27B refreshed on the 1.0 dataset. It replaces the published model as a new revision of the
   same repos, which also retires its `formatfloat` habit (D72), and it is the new teacher.
2. `qwen35-4b-v1`: on the same dataset. It uses no 27B output, so it can follow at once.
3. `qwen35-4b-v2`: the 4B plus the refreshed 27B's verified programs; one change from v1.

3. **`qwen38-27b-v1` (see the run order above):** a 27B refresh on the 1.0 dataset, if a published model that knows maps and the 1.0
   names is wanted. Changes from run 7: the data and the language only.

## 5. Each run's write-up

A RESULTS.md beside the run, on one grader. The run-7 lessons apply:
- the noise floor;
- replaying older runs on the current compiler;
- mining failure classes from the main suites only.

## 6. After the current runs: sanity check on the revised VM (owner, 2026-10-20)

The underlying VM (pscal-core) is being audited and fixed: performance and bug fixes, with no Aether surface change
intended. It is not redeployed to the graders until the current data and training work is done. Then, on the
deployed build:
1. Paired replay of every scored model's archived programs: run 7 and its seeds, `qwen38-27b-v1`, `qwen35-4b-v1`.
   Any flip is named.
2. `--check-gates` over the full training data.
3. The compiler's ctest, including the builtin-surface test.
4. Timing of the replay, old build against new, to quantify the performance change.
