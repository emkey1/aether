# Fine-tuning runs: registry, run configs, driver

One driver, `tools/aether_train_run.py`, takes a run from dataset to scores. It
replaces the rig-only scripts in `../rig_snapshot_20261015/` (kept as a record)
and implements recommendations 3, 4 and 7 of the training-framework assessment.

| file | what |
|---|---|
| `families.json` | per model family: chat markers for response masking, LoRA targets, training image, the chat-template switch (thinking off), whether to serve with the base tokenizer, vLLM settings, reasoning-leak prefixes. Each entry cites the run that established it. |
| `runs/<tag>.json` | one run: family, base model path on the host, host short name, precision, LoRA recipe, dataset id and compiler version, scoring plan |
| `../datasets/<id>/` | the dataset a run trains on, with `DATASET_MANIFEST.json` (file sha256s, source commit as `umbrella_commit`, compiler version and sha256, record counts) |
| `../training_runs/<tag>/` | generated scripts and the records fetched back from the host (no weights) |

## Running one

```
R=training/config/runs/qwen38-27b-l1-pilot.json
python3 tools/aether_train_run.py $R plan       # everything it would do; changes nothing
python3 tools/aether_train_run.py $R dataset    # build + gate here, against the pinned compiler
python3 tools/aether_train_run.py $R push       # dataset, trainer, harness subset, scripts -> host; checks model/image
python3 tools/aether_train_run.py $R train      # detached training container on the host
python3 tools/aether_train_run.py $R status     # from the host's own files, any time
python3 tools/aether_train_run.py $R score      # detached tmux: serve, smoke, score base and tuned
python3 tools/aether_train_run.py $R fetch      # records back here (pack reports before committing)
python3 tools/aether_train_run.py $R release    # stop this run's serving container and tmux
```

Every stage can be re-run: training resumes from its last intact checkpoint,
scoring resumes each suite with the harness's `--resume`, and a finished stage
is skipped. Nothing long runs on the machine that launched it.

Runs name their host by a neutral alias (`"host": "host-c"`). The private fleet
overlay (`~/.config/pscal/fleet.env`, read by `tools/fleet_env.py`; never in a
repo) maps it to a machine: `AETHER_HOST_C=<name>` for `host-c` (in general
`AETHER_HOST_<ALIAS>`), and sets the ssh user, `AETHER_TRAIN_SSH_USER`. Neither
has a default; a stage that needs the host says which variable to set.

## Guards

- **One GPU job per host, and only with room.** `train` and `score` refuse while
  another `aether-train-*` / `aether-serve-*` container is up or the host has less
  than `min_available_gb` available. The GPU hosts are shared (ComfyUI, MiMo
  serving); freeing one is the owner's call, never the driver's.
- **Markers.** The trainer stops before step 1 if the family's markers leave more
  than a tenth of the rows with no trainable token (a wrong pair masks
  everything; it happened twice).
- **Template.** Training text is rendered with the family's
  `chat_template_kwargs`, the same ones the served requests send. Qwen3.8's
  template adds a "Reasoning effort is set to xhigh" system turn otherwise.
- **Export smoke.** Each scored variant first runs three easy tasks with no guide;
  empty output or a reasoning trace stops that variant before any suite runs.
- **Grader pin.** The scoring script checks the grader's language version
  against the dataset's and passes its sha256 to the harness.
- **Records.** `trainer` writes seed, data-order seed, early-stopping settings,
  dataset sha256s, library versions and the first rendered training text to
  `train/run_metadata.json`; `train.sh` records the image id and its libraries.
- **Divergence (opt-in).** `train.divergence_stop_ratio` ends a run whose eval
  loss leaves its best behind, with no export (below).

## Stopping a diverged run

Off unless a run sets `train.divergence_stop_ratio`, a number above 1 (2 is the rule `../distill/c1-20261118/` fixed
for scoring). From eval `train.divergence_stop_min_evals` on (default 2, counting from 1: the first eval only sets the
best), an eval loss above ratio × the best eval loss of the earlier evals ends training at that eval. A NaN or inf
eval loss ends it at any eval: a ratio test never fires on NaN, and no later step recovers from non-finite weights.
Then:

- the trainer writes no final adapter and no merged export, records the stop under `diverged` in
  `train/run_metadata.json` (step, eval loss, best eval loss and its step, the settings, a message), and exits 6,
  which `docker ps -a` shows. The checkpoints stay.
- `status` prints a `DIVERGED:` line, says "merged export: none (the run diverged)", and exits 1.
- `train` refuses to resume the run, and so does the trainer: its checkpoints are past the divergence. Retrain under
  a new tag.
- `score --when-ready` with a tuned variant gives up with exit 6 instead of waiting for an export that will not come.

Why: `gemma4-12b-c1` (2026-11-25, `load_best_model_at_end` false) diverged at step 54 of 978. Its eval loss was 0.14
at step 50, 6.59 at step 75 and 4.13 at the end. It trained to the end and exported the diverged model, about 1.8 GPU
hours after the divergence showed. With the stop at 2 it would have ended at step 75.

**A healthy run is unchanged.** Until it fires, the stop only reads eval losses. The run trains, saves, keeps
checkpoints and exports as it would without it; only its records name the setting. So turning the stop on for a
study's runs is a recipe change only for a run it would have stopped. That run then ends with no export, where without
the stop it would have trained to the end (and, with `load_best_model_at_end`, exported its best checkpoint). The stop
reads every eval, not only the last, so it also ends a run that rose above the ratio and would have come back. That is
the one case where it is stricter than c1-20261118's rule, which reads only the final eval loss. The healthy 12B and
4B runs of that recipe end within 1.2 × their best. The three fetched eval histories
(`../training_runs/qwen38-27b-{algo,l1-pilot,toon}/train/trainer_state.json`) never rose above 1.06 × their best so
far.

## Scoring a kept checkpoint

A run with `train.keep_checkpoint_steps` keeps those steps' adapters. To score one, write a twin config: the same
recipe and dataset, a new `tag`, and `"merge_from": {"run": "<the trained run's tag>", "step": 575}`.

```bash
python3 tools/aether_train_run.py $TWIN push     # dataset, this checkout's trainer, harness, merge.sh
python3 tools/aether_train_run.py $TWIN merge    # detached tmux; logs/merge.out ends with MERGED
python3 tools/aether_train_run.py $TWIN score
```

`merge.sh` is generated and runs the twin's own pushed copy of the trainer (export only), so there is no
hand-copied merge script on the host to go stale. `train` refuses a twin.

### Progress by execution, not eval loss

Eval loss chose the wrong stopping point for the MoE: it was best at epoch 0.64, while the pass rate kept rising to the
end of the schedule. (How many epochs the schedule has mattered less: on 2,802 fresh tasks a finished two-epoch run and
a three-epoch run do not differ, `../distill/fep-20261118/`.) To read progress by pass rate instead, keep adapters at
several steps and score each:

```bash
python3 tools/aether_train_run.py $RUN twins      # writes <tag>-s<step>.json per kept step (--force rewrites)
# for each twin: push, merge, score, fetch (above)
python3 tools/aether_train_run.py $RUN progress   # first attempt (after repair) by step and epoch
```

`progress.suites` in the run config names the development pool the twins score (default: the run's own suites).
`twins` refuses held-out suites there, so the held-out score stays untouched for the final choice. `progress` finds
twins by their `merge_from`, so older twins with other names count, and prints development and held-out columns
apart. Fix the stopping rule (for example, the earliest step within one task of the best development score) in the run
config before the twins are scored.

## Scoring on a Mac through Ollama

`tools/score_ollama_model.sh` scores one model on a scoring Mac: it imports a merged export (`--merged DIR`,
`--quant int8` by default) or uses an existing Ollama model (`--model`), waits for the Mac's one-model lock
(`tools/ollama_guard.sh`), and runs the Aether suites with no guide (`--suites`), with a guide (`--guided-suites` and
`--guide NAME=PATH`) and the sandboxed Python baseline (`--python-suites`). The grader's sha256 is required and checked;
the Ollama server version is pinned by the first suite. Every report goes through `tools/check_bench_report.py`: no
all-empty answers, no reasoning traces, every case measured, the named guide (name and sha256) in a guided report and
no guide in an unguided one. Re-run the same command to resume; `DIR/finished` says `DONE` or why it stopped. It
replaces the per-model Mac scripts (a guided pass once ran without its guide and nothing noticed).

## Publishing to Hugging Face

`tools/hf_release.py` (see its docstring) uploads a release folder, checks every file against the Hub, and only
then moves `main` or makes a new repo public. A new repo is created private; a repo that is already public takes
uploads on a branch. The token comes from `$HF_TOKEN` alone. Each step that changes public content is the owner's
decision.

## Scoring plan

The default plan scores the untrained base and the fine-tune, both with no
guide, on the four B0 suites (tasks_v2_pos, tasks_hard_v2, tasks_hard_nontoon,
tasks_cs: 73 tasks), T=0, seed 42, two repair rounds, generous output budget.
Add `"base:medium"` to `score.variants` to also measure the base with the medium
guide (the bar a no-guide fine-tune has to reach). Repeats default to 1; a
hyperparameter conclusion needs 3 seeds (assessment item 11).

## Adding a family

Probe the template first (`../rig_snapshot_20261015/rig_dir/tmpl_probe.py`, or
render one conversation with and without the thinking switch), confirm the
markers appear verbatim in the rendered text, then add the entry with its
evidence. Image choice: a family whose `model_type` the image's transformers
does not know cannot train there.
