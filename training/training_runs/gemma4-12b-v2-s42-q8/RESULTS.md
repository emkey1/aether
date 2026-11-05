# gemma4-12b-v2-s42-q8: results (2026-11-05)

**By the reading fixed before scoring, the second seed does not keep its lead, and the published int8 form stays.** At 12 GB it scores 96 (114) of 126 against the
published form's 93 (106). After repair the split is 12 to 4, p=0.077, and the bar was p < 0.05, which 13 to 4 would have cleared. First attempts split 11 to 8, so the
second condition holds. Nothing is offered to replace the published form.

The pass took 67 min on the scoring Mac, on the 1.0 grader, with no infrastructure failures or empty answers, and all 126 prompts match the published form's. Two
attempts hit the length cap, described below. `compare.py` prints every table here.

## Results: no guide, first attempt (after repair)

| | size | main (73) | held-out (53) | all (126) |
|---|---:|---|---|---|
| **second seed, int8** | **12 GB** | **65 (72)** | **31 (42)** | **96 (114)** |
| second seed, 16-bit (`../gemma4-12b-v2-s42/`) | 24 GB | 65 (71) | 31 (45) | 96 (116) |
| published seed, int8 (`../gemma4-12b-v2-q8/`) | 12 GB | 65 (70) | 28 (36) | 93 (106) |
| published seed, 16-bit (`../gemma4-12b-v2/`) | 24 GB | 67 (69) | 30 (36) | 97 (105) |
| e3, mxfp4 (`../e3-int4-1.0-20261101/`) | 20.9 GB | 64 (68) | 33 (40) | 97 (108) |
| e3, int8 | 39.1 GB | 65 (70) | 36 (44) | 101 (114) |
| e3 (published), bf16 | 67 GB | 65 (70) | 39 (47) | 104 (117) |

| suite | second seed, int8 | second seed, 16-bit | published, int8 | published, 16-bit | e3, mxfp4 | e3, int8 | e3 |
|---|---|---|---|---|---|---|---|
| tasks_v2_pos (35) | 32 (35) | 32 (35) | 33 (34) | 33 (34) | 33 (34) | 33 (35) | 33 (35) |
| tasks_hard_v2 (14) | 13 (14) | 13 (13) | 12 (14) | 13 (13) | 13 (14) | 13 (14) | 13 (14) |
| tasks_hard_nontoon (5) | 3 (4) | 3 (4) | 2 (4) | 3 (4) | 2 (4) | 2 (4) | 2 (4) |
| tasks_cs (19) | 17 (19) | 17 (19) | 18 (18) | 18 (18) | 16 (16) | 17 (17) | 17 (17) |
| tasks_frontier (15) | 10 (13) | 10 (14) | 10 (13) | 10 (13) | 11 (13) | 9 (14) | 10 (14) |
| tasks_frontier_algo (14) | 11 (14) | 10 (14) | 9 (11) | 8 (10) | 11 (12) | 10 (11) | 10 (11) |
| tasks_frontier_spec (12) | 7 (10) | 7 (9) | 5 (6) | 6 (6) | 4 (6) | 7 (8) | 9 (11) |
| tasks_sealed_toon (12) | 3 (5) | 4 (8) | 4 (6) | 6 (7) | 7 (9) | 10 (11) | 10 (11) |

Tasks only one model passes, second seed at int8 against each, with two-sided sign tests:

| against | | first attempt | after repair |
|---|---|---|---|
| **published, int8** | **all** | **11 to 8 (p=0.65)** | **12 to 4 (p=0.08)** |
| | main | 4 to 4 (p=1.00) | 3 to 1 (p=0.63) |
| | held-out | 7 to 4 (p=0.55) | 9 to 3 (p=0.15) |
| second seed, 16-bit | all | 2 to 2 (p=1.00) | 2 to 4 (p=0.69) |
| e3, mxfp4 | all | 14 to 15 (p=1.00) | 13 to 7 (p=0.26) |
| e3, int8 | all | 12 to 17 (p=0.46) | 9 to 9 (p=1.00) |
| e3 | all | 10 to 18 (p=0.19) | 6 to 9 (p=0.61) |
| | held-out | 6 to 14 (p=0.12) | 4 to 9 (p=0.27) |

Failing programs by class, coded diagnostic / wrong return code / wrong output, over all 126 tasks:

| | first attempt | after repair | failures repair fixed |
|---|---|---|---|
| second seed, int8 | 23 / 3 / 4 | 5 / 2 / 5 | 18 of 30 |
| second seed, 16-bit | 23 / 1 / 6 | 4 / 1 / 5 | 20 of 30 |
| published, int8 | 17 / 7 / 9 | 9 / 3 / 8 | 13 of 33 |
| published, 16-bit | 14 / 7 / 8 | 8 / 2 / 11 | 8 of 29 |
| e3, mxfp4 | 19 / 2 / 8 | 11 / 2 / 5 | 11 of 29 |
| e3, int8 | 18 / 2 / 5 | 7 / 0 / 5 | 13 of 25 |
| e3 | 16 / 2 / 4 | 6 / 0 / 3 | 13 of 22 |

## Reading

- **This was a weak second sample, weaker than the README said.** It expected few programs to stay byte-identical at int8. In fact 77 of 126 first programs match the
  16-bit form, 24 of 53 on held-out, and the published seed is the same at 78 of 126. So the scores staying close is partly the same programs being graded twice, and
  the 12 to 4 does not independently confirm 16-bit's 14 to 3.
- **The direction held, and the bar was not met.** Every total here is level or better than the published form's, and none of the three sign tests over all 126, main
  and held-out reaches p < 0.05.
- **The better repair rate repeats, with the same caveat:** 18 of 30 at int8 and 20 of 30 at 16 bits, against 13 of 33 and 8 of 29 for the published seed.
- **Sign tests cannot separate it from any form of e3,** on any row above. e3 is still ahead on first attempts at int8 and bf16, with 36 and 39 on held-out against 31.
  Only one seed of e3 has been scored.
- **`tasks_sealed_toon` is where these Gemma runs trail.** The two seeds' four forms and the two targeted runs read 3 to 6 first attempts, and e3's three forms read 7, 10
  and 10. After repair the highest Gemma figure is 8 and the lowest e3 figure is 9. The fresh-task filter points the same way (`../../distill/fe3-20261104/`): reading a
  JSON data file fails 43 of 58 on Gemma and 12 of 58 on e3. Those are different tasks, and the suite's failures stay unread.
- **One held-out task ran away.** Its first attempt and first repair both hit the 8,192-token cap, and the second repair passed. The 16-bit form passes it first time and no
  other run in this table hits the cap. It accounts for 16,384 of this pass's 80,688 completion tokens, against 71,814 on the published int8.
- **Speed was not measured.** The pass took 67 min against the published form's 48 and used 12% more tokens, so the tokens do not explain all of it. Nothing else was
  checked about load on the Mac.

## What was checked

- **The weights:** SHA-256 matches the training host's copy, starting `6a82198f5e93f3b3`, and the file differs from the published seed's. The config, tokenizer, chat
  template and the other three configuration files are byte-identical to the published seed's.
- **The prompt:** `_debug_render_only` on `/v1` renders the same text for both models, with one BOS and the empty thought channel.
- **The grader:** the 1.0 release binary, by hash, as on the published form.

## Next

- **Added on 2026-11-05:** a separate test on 1,776 fresh tasks found this seed stronger on first attempts, 100 tasks to 150 failed alone, p=0.0019
  (`../../distill/fs42-20261105/`). The reading above still stands for this test, and it is the owner's decision whether the new result changes what is published.
- **The published form stays.** The second seed's forms are not published: 12 GB in Ollama on the Mac and the 24 GB export on both machines.
- **The published cards' seed-noise line was updated on 2026-11-05,** on the owner's word: `Gemma-4-12B-Aether-int8` at `f502de665cd1`, `Gemma-4-12B-Aether` at
  `aa0adf8042a7`, `Gemma-4-12B-Aether-LoRA` at `b2d672f7cfb7`. Each read back with no token, and no weights changed.
