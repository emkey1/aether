# frestore-20261127: gemma4-12b-v3 with its early layers put back to the base (written 2026-11-27, before any candidate was built or scored)

The owner asked on 2026-11-27 whether v3's changes to layers 0 to 15 are what damages its Python, and what restoring
the untrained layers there does to Python and to Aether, and said yes to running it on the training host.

**What is known before the session.**
- The published model's card, `training/config/hf_release/README.gemma.int8.md`: on the benchmark's 126 tasks in
  Python at int8, the untrained base passes 109 first attempts (125 after repair) and v3 68 (94). Many failed first
  attempts carry Aether into Python: `for i in 0..5:`, `has_toon()`, `toon_parse()`.
- `../fgraft-20261126/`, on 298 Aether records held out of both v3 seeds: the base with only v3's changes to layers 20
  to 47 has eval loss 0.0456 (v3 0.0430, base 0.7696), about 99.6% of v3's drop; with only v3's changes to layers 0 to
  19 it has 0.7878, slightly worse than the base. Between the two v3 seeds, the cosine of their changes to the base is
  0.000 in layers 0 to 15.
- `../../training_runs/gemma4-12b-late/`: LoRA trained on layers 20 to 47 only. Its preliminary reading on the
  training host is -1.36 points [-3.02, +0.29] in Aether against the all-layer recipe; the deciding session on the
  scoring Mac is still running. Its Python has not been measured.

Nothing here retrains anything. Every candidate is a fixed edit of fixed weights, so the readings describe these
weights (seed 3407's, the published ones), not a recipe, and training-seed noise does not enter them.

## Candidates

`build_restore.py` builds r16 and e16 from `gemma4-12b-v3`'s merged export and the untrained `gemma-4-12B-it`, by
layer, byte for byte, for each of the 328 LoRA-target tensors (the seven target projections in the 48 decoder layers).
It stops unless every other tensor is already byte-identical in the two files and every target tensor of v3 differs
from the base's, so r16 is exactly "v3 without its changes to layers 0 to 15". The header, config, tokenizer and chat
template are v3's. Its `restore_manifest.json` (copied here as `restore_manifest_<tag>.json`) records the inputs' and
the output's SHA-256.

| tag | layers 0-15 | layers 16-47 | answers |
|---|---|---|---|
| `base` | base | base | Python |
| `v3` | v3 | v3 | Python, Aether |
| `r16` (the question) | **base** | v3 | Python, Aether |
| `e16` (the reverse) | v3 | **base** | Python |
| `v3_late` (`gemma4-12b-v3-late`, LoRA trained on layers 20-47 only, seed 3407) | | | Python |

## The session

`score_host.sh`, on the training host, in one session: each model served at bf16 from its merged export by the vLLM
image of gemma4-12b-late's training-host session (`sha256:30a38a1d…`; it was removed from this host on 2026-11-27, is
copied back from another GPU host for this session and removed after it, on the owner's word), 16 requests at a time.
Every model answers `../fv3-20261105/`'s 1,026 frozen tasks with no guide and no repair, at T=0, seed 42:
- **Aether**, graded by the 1.0 release built for Linux (SHA-256 `923415…`), as in gemma4-12b-late's session.
- **Python**, the harness's `--python-baseline --skip-aether`: the program runs under the host's `python3` in the
  harness's docker sandbox (no network, no child processes) and passes on exactly the expected output and exit code.
  Every one of the 1,026 expected outputs was matched by an independent Python program when the tasks were frozen
  (`achievability: python-agreed-candidate`), so each task is solvable in Python.

Order: r16 (Aether, Python), v3 (Aether, Python), base, e16, v3_late (Python). `session_server_version.txt` records
the image, the grader, the worker count, the host's python3 and the sandbox image before the first pass; every pass
checks them and the session stops if one changed. A pass counts only with all 1,026 tasks measured, no infrastructure
failure, no empty answer and no leaked reasoning (`check_report.py`). The reports are packed as
`report__{python,aether}_<tag>.json.gz`.

## The reading, fixed now

`compare.py` computes it: first-attempt pass or fail per task, differences paired over the 1,026 tasks, 95% intervals
from a paired bootstrap over tasks (10,000 resamples, seed 42).

**Python (the question): r16 minus v3.**

| 95% interval | reading |
|---|---|
| above zero | **restoring layers 0-15 recovers part of v3's Python**; the share of the base-minus-v3 gap it recovers is reported with its interval |
| includes zero | **no Python recovery shown**; the upper end says how large a recovery is ruled out |
| below zero | **restoring layers 0-15 makes v3's Python worse** |

**Aether (the price): r16 minus v3,** at the 2.0-point margin of gemma4-12b-late:

| 95% interval | reading |
|---|---|
| above zero | **better at Aether than v3** |
| lower end at or above -2.0 points | **keeps Aether** |
| below zero | **costs Aether** |
| otherwise | **not resolved** |

**What follows.** Only "recovers part of v3's Python" together with "better" or "keeps Aether" makes r16 a candidate,
and a candidate changes nothing by itself: the same edit is first answered at int8 on the scoring Mac (the card's
eight Python suites and fv3's Aether), and anything about a release stays the owner's decision. Every other outcome:
no change, v3 stays as it is. Nothing here touches the published repositories.

**Reported, not deciding:**
- Where the Python damage sits: e16 minus base (do v3's changes to layers 0-15 alone cost Python?), and r16 minus base.
- The late-only LoRA run: v3_late minus v3 and minus base, in Python.
- Failure classes: Python (exit code, wrong output), Aether (coded, return code, wrong output).
- Aether written into Python: of each model's failed first Python programs, how many carry any of five markers fixed
  in `compare.py` before any program was read (`in a..b` outside a comment, a TOON builtin call, `println(`, an
  `fx {` block, an Aether `fn` / `let x:` / `loop` line), and how many carry each.
- v3's Aether answers here against the same weights' answers in gemma4-12b-late's training-host session (same image,
  grader and worker count): same first program and same pass or fail, when that report is present.

**Limits.** One set of weights. bf16 through vLLM, not the int8 Ollama form people download. First attempts only, no
repair. The 1,026 tasks were written for Aether by one writer; that they are solvable in Python is known, but they may
favour neither language's habits. The split at layer 16 is where the two seeds' changes stop being unrelated, not a
tuned boundary.
