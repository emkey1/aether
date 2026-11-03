# gemma4-12b-v2-q8: results (2026-11-03)

**At 8 bits and 12 GB, the Gemma 4 12B fine-tune keeps its scores, and a sign test cannot separate it from the 35B MoE
at 20.9 GB, but it generates about 4 to 6 times slower.** It loses 2 first attempts on each set of suites against its own 16-bit
form and is level after repair, 106 against 105. That answers the question `../gemma4-12b-v2/` left open: a model about 9 GB
smaller does the same job on this benchmark. Where e3 at mxfp4 fits, e3 is still the better choice, for its speed. Gemma's
case is a machine it does not fit. The full e3 is clearly ahead of both on held-out.

The form also clears the bar set for publishing it: 28 held-out first attempts, not below the guided base's 26.

The scoring pass took 48 min on the scoring Mac. All eight suites ran with no infrastructure failures, empty answers or
length caps, and all 126 prompts are byte-identical to e3's. `compare.py` prints every table below.

## Results: no guide, first attempt (after repair)

| | size | main (73) | held-out (53) | all (126) |
|---|---:|---|---|---|
| **Gemma 4 12B fine-tune, int8** | **12 GB** | **65 (70)** | **28 (36)** | **93 (106)** |
| Gemma 4 12B fine-tune, 16-bit (`../gemma4-12b-v2/`) | 24 GB | 67 (69) | 30 (36) | 97 (105) |
| e3, mxfp4 (`../e3-int4-1.0-20261101/`) | 20.9 GB | 64 (68) | 33 (40) | 97 (108) |
| e3, int8 | 39.1 GB | 65 (70) | 36 (44) | 101 (114) |
| e3 (published), bf16 | 67 GB | 65 (70) | 39 (47) | 104 (117) |
| untrained Gemma 4 12B + 1.0 guide, q8_0 (`../guided-gemma4-12b-1.0-20261031/`) | 12 GB | 48 (67) | 26 (41) | 74 (108) |

e3's published row was graded on 2026-10-21-8, and replaying its programs on 2026-10-30-1 moves nothing. Its held-out score
helped choose it for release, so its 39 is slightly flattering. The Gemma rows are one seed (3407) at T=0.

| suite | int8 | 16-bit | e3 | e3, int8 | e3, mxfp4 | guided |
|---|---|---|---|---|---|---|
| tasks_v2_pos (35) | 33 (34) | 33 (34) | 33 (35) | 33 (35) | 33 (34) | 29 (35) |
| tasks_hard_v2 (14) | 12 (14) | 13 (13) | 13 (14) | 13 (14) | 13 (14) | 3 (11) |
| tasks_hard_nontoon (5) | 2 (4) | 3 (4) | 2 (4) | 2 (4) | 2 (4) | 2 (4) |
| tasks_cs (19) | 18 (18) | 18 (18) | 17 (17) | 17 (17) | 16 (16) | 14 (17) |
| tasks_frontier (15) | 10 (13) | 10 (13) | 10 (14) | 9 (14) | 11 (13) | 10 (13) |
| tasks_frontier_algo (14) | 9 (11) | 8 (10) | 10 (11) | 10 (11) | 11 (12) | 6 (11) |
| tasks_frontier_spec (12) | 5 (6) | 6 (6) | 9 (11) | 7 (8) | 4 (6) | 6 (8) |
| tasks_sealed_toon (12) | 4 (6) | 6 (7) | 10 (11) | 10 (11) | 7 (9) | 4 (9) |

Tasks only one model passes, with two-sided sign tests:

| int8 against | | first attempt | after repair |
|---|---|---|---|
| 16-bit | main | 1 to 3 (p=0.63) | 1 to 0 (p=1.00) |
| | held-out | 1 to 3 (p=0.63) | 2 to 2 (p=1.00) |
| | all | 2 to 6 (p=0.29) | 3 to 2 (p=1.00) |
| e3, mxfp4 | main | 3 to 2 (p=1.00) | 3 to 1 (p=0.63) |
| | held-out | 9 to 14 (p=0.40) | 7 to 11 (p=0.48) |
| | all | 12 to 16 (p=0.57) | 10 to 12 (p=0.83) |
| e3, int8 | main | 3 to 3 (p=1.00) | 3 to 3 (p=1.00) |
| | held-out | 9 to 17 (p=0.17) | 5 to 13 (p=0.10) |
| | all | 12 to 20 (p=0.22) | 8 to 16 (p=0.15) |
| e3 | main | 3 to 3 (p=1.00) | 3 to 3 (p=1.00) |
| | held-out | 7 to 18 (p=0.04) | 3 to 14 (p=0.01) |
| | all | 10 to 21 (p=0.07) | 6 to 17 (p=0.03) |
| guided | main | 20 to 3 (p<0.001) | 6 to 3 (p=0.51) |
| | held-out | 14 to 12 (p=0.85) | 7 to 12 (p=0.36) |
| | all | 34 to 15 (p=0.009) | 13 to 15 (p=0.85) |

## What the numbers say

- **Eight bits cost little.** e3 lost 3 held-out first attempts going to int8; Gemma lost 2. Only 8 first attempts differ, and
  78 of the 126 first programs are byte-identical. The two rows differ in serving stack as well as bits (vLLM against
  Ollama), so this run cannot split those.
- **At the 32 GB size, the two fine-tunes are not separable.** Gemma is about 9 GB smaller. e3 at mxfp4 is 4 first attempts and 2
  repaired tasks ahead overall, all of it on held-out (33 to 28, 40 to 36); Gemma is slightly ahead on main (65 to 64, 70 to 68).
- **The fine-tune's gain over the guide is first attempts on main.** It takes 20 of the 23 main tasks where the two differ, and
  hard_v2 moves from 3 to 12. On held-out, and after repair anywhere, it is level with the guided base.
- **It still has room on held-out.** sealed_toon and frontier_spec are where it trails e3 most: 9 (12) on those two against 19 (22).
  Those failures were not read, so the suites can go on judging training data they did not shape.

Failing programs by class, coded diagnostic / wrong return code / wrong output, over all 126 tasks:

| | first attempt | after repair | failures repair fixed |
|---|---|---|---|
| **Gemma, int8** | **17 / 7 / 9** | **9 / 3 / 8** | **13 of 33** |
| Gemma, 16-bit | 14 / 7 / 8 | 8 / 2 / 11 | 8 of 29 |
| e3 | 16 / 2 / 4 | 6 / 0 / 3 | 13 of 22 |
| e3, int8 | 18 / 2 / 5 | 7 / 0 / 5 | 13 of 25 |
| e3, mxfp4 | 19 / 2 / 8 | 11 / 2 / 5 | 11 of 29 |
| guided | 31 / 1 / 20 | 9 / 0 / 9 | 34 of 52 |

Return codes are still Gemma's own weakness: 7 first attempts against e3's 2 in every form. The repair count is noisier than
it looked at 16-bit: 8 of 29 there, 13 of 33 here.

## Speed and memory

The same Mac and stack, `measure_speed.py`: the first eleven algorithm-suite prompts, no guide, the median of the ten answers
after the load.

| | generating | median answer | `ollama ps` after 11 answers |
|---|---:|---:|---:|
| **Gemma 4 12B fine-tune, int8** | **38.8 tok/s** | **168 tokens** | **15 GB** |
| e3, mxfp4 | 217.0 tok/s | 166 tokens | 22 GB |

- **e3 at mxfp4 is 5.6 times faster.** The likely cause is that the dense 12B reads all its weights for each token and the MoE
  about 3B, as `../gemma4-12b-v2/RESULTS.md` guessed. This measurement does not isolate it. A later back-to-back pair on the same Mac
  under outside load read 20 and 85 tok/s, 4.2 times (`../gemma4-12b-v2-4bit/README.md`), so these speeds depend on the load.
- **The resident size grows in use.** Gemma's reading rose from 15 GB to 21 GB by the end of the scoring pass, at a 32K context.
  e3's reached 29 GB over a full pass (`../gemma4-12b-v2-4bit/README.md`).
- **Neither has run on a 32 GB Mac,** where macOS limits what the GPU may wire down. These numbers come from a larger Mac,
  so the ratio is the result, not the speeds.

## Python, and the untrained base

`card_numbers.sh`, for the model card, at int8 on the same Mac: the fine-tune writes Python at 58 (79) of 126 against the untrained
base's 109 (125). The base with no guide writes Aether at 0 (4) on main and 0 (0) on held-out
(`../base-gemma4-12b-int8-20261103/`).

## What was checked

- **Weights:** the merged export's SHA-256 matches the training host's after the copy.
- **Served prompt:** on `/v1` with `"reasoning_effort": "none"`, Ollama renders the training shape (see `README.md`).
- **Which model and grader:** the reports name `aether-gemma4-12b-v2-q8-32k` and the 1.0 release binary, checked by hash.
- **Program shape:** median first-attempt completion 277 tokens, against 278 at 16-bit.

## How much of this is chance

The controls in `../qwen36-35b-a3b-v2/RESULTS.md` apply here too: nearly identical training differs on 9 of 126 first
attempts, and held-out seed noise has never been measured. A difference of a few tasks between any two rows above cannot
be read as more than that.
