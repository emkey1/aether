# The Gemma 4 12B fine-tune at 4 bits on a Mac (set up 2026-11-03)

The final step of `../gemma4-12b-v2/` at nvfp4 and mxfp4, with no guide, on the eight suites, graded by the 1.0 release
binary. It asks how far the model can shrink before its scores go: on e3, 4 bits cost a few held-out tasks. A Mac has no FP4
hardware, so this run gives scores, sizes and memory, not the speed FP4 might gain on hardware with FP4 kernels. `../gemma4-12b-v2-nvfp4/`
tests that.

## Setup

The recipe of `../gemma4-12b-v2-q8/`; only the quantization changes.
- **Model:** the merged 16-bit export of `gemma4-12b-v2`, imported into Ollama with `ollama create -q <format>`, each
  copied with a 32K context.
- **Settings:** T=0, seed 42, one repeat, two repair rounds, 8,192 output tokens, thinking off
  (`"reasoning_effort": "none"`). Grader: the 1.0 release binary, checked by hash.
- **Memory:** Ollama's resident size is logged after every suite, because Gemma's int8 copy grew from 15 GB to 21 GB over a pass.
  e3 at mxfp4 then repeats its own pass to get the same reading; its scores stay in `../e3-int4-1.0-20261101/`.

## Running it

The command is at the top of `score.sh`. Each suite resumes from its checkpoint; re-run the script to continue. Copy the
reports here as `score/<format>/tuned__<suite>.json.gz` with `bench/results/pack_reports.py`.

## Results (2026-11-03)

**Four bits cost this model far more than eight, so a Mac should stay at int8.** No guide, first attempt (after repair). All eight
suites ran with no infrastructure failures or empty answers, and all 126 prompts are byte-identical to the 16-bit run's.

| | on disk | `ollama ps`, full pass | main (73) | held-out (53) | all (126) |
|---|---:|---:|---|---|---|
| **nvfp4** | **8.0 GB** | **16 to 18 GB** | **58 (65)** | **21 (26)** | **79 (91)** |
| **mxfp4** | **7.7 GB** | **16 to 17 GB** | **55 (59)** | **22 (28)** | **77 (87)** |
| int8 (`../gemma4-12b-v2-q8/`) | 12 GB | 15 to 21 GB | 65 (70) | 28 (36) | 93 (106) |
| 16-bit (`../gemma4-12b-v2/`), served by vLLM | 24 GB | not read | 67 (69) | 30 (36) | 97 (105) |
| e3, mxfp4 (`../e3-int4-1.0-20261101/`) | 20.9 GB | 26 to 29 GB | 64 (68) | 33 (40) | 97 (108) |

| suite | nvfp4 | mxfp4 | int8 | 16-bit |
|---|---|---|---|---|
| tasks_v2_pos (35) | 31 (32) | 29 (30) | 33 (34) | 33 (34) |
| tasks_hard_v2 (14) | 9 (13) | 10 (12) | 12 (14) | 13 (13) |
| tasks_hard_nontoon (5) | 1 (1) | 1 (1) | 2 (4) | 3 (4) |
| tasks_cs (19) | 17 (19) | 15 (16) | 18 (18) | 18 (18) |
| tasks_frontier (15) | 10 (12) | 10 (11) | 10 (13) | 10 (13) |
| tasks_frontier_algo (14) | 8 (9) | 5 (7) | 9 (11) | 8 (10) |
| tasks_frontier_spec (12) | 2 (4) | 4 (7) | 5 (6) | 6 (6) |
| tasks_sealed_toon (12) | 1 (1) | 3 (3) | 4 (6) | 6 (7) |

- **Against int8:** nvfp4 takes 5 tasks to 19 on first attempts (p=0.007) and 7 to 22 after repair (p=0.008); mxfp4 takes 6 to 22
  (p=0.004) and 5 to 24 (p=0.0005). The two formats are not separable, 13 to 11 and 15 to 11.
- **Gemma loses more than e3.** From full size to nvfp4 it loses 18 first attempts, where e3 loses 10.
- **What grows is wrong output.** After repair, nvfp4 has 26 wrong outputs and mxfp4 23, against int8's 8. On first attempts the
  classes, coded / return code / wrong output, are 25 / 2 / 20 and 24 / 6 / 19, against int8's 17 / 7 / 9.
- **Against the builds in `../gemma4-12b-v2-nvfp4/`:** nvfp4 is level with `mlp_only`, 17 to 16 and 14 to 17. It is ahead of the W4A4
  plain recipe, 21 to 11 (p=0.11) and 18 to 10 (p=0.18), but not separably, so this does not show what 4-bit activations cost.
- **Repeat noise on this stack is real.** e3's repeat pass, the same weights and settings two days after its first (Ollama versions
  not compared), scored 96 (105) against 97 (108). 11 first attempts changed and 80 of 126 first programs were identical, close to
  the 78 int8 shares with 16-bit.

## Speed

The first eleven algorithm-suite prompts, the median of the ten answers after the load. Each earlier reading was taken alone, under
different outside load. The back-to-back run took all five readings within minutes, with unrelated builds and simulator tests running.

| | earlier | back to back |
|---|---:|---:|
| Gemma, int8 | 38.8 tok/s | 20.1, then 19.9 tok/s |
| Gemma, nvfp4 | 20.7 tok/s | 32.9 tok/s |
| Gemma, mxfp4 | 13.2 tok/s | 26.4 tok/s |
| e3, mxfp4 | 217.0 tok/s | 84.8 tok/s |

Readings on this Mac can move by 2 to 3 times with outside load, so only the back-to-back ratios are usable: nvfp4 is 1.6 times int8,
mxfp4 1.3 times, and e3 at mxfp4 4.2 times. A reading on a quiet machine is still missing.

## Memory

e3 at mxfp4 is 20.9 GB on disk, and `ollama ps` read 26 GB after one suite and 29 GB for the rest of the pass, at a 32K context. Gemma
at int8 reached 21 GB. If those figures are what the model needs, e3 is tight on a 32 GB Mac and Gemma at int8 is not. Some of the
growth may be cache that would shrink under pressure. Neither has run on a 32 GB machine.
