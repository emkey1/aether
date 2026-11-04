# gemma4-12b-v2: results (2026-11-02)

**At 16-bit, Gemma 4 12B is level overall with the 35B MoE squeezed into 20.9 GB, and it does not replace e3.** It leads
that bar on main and trails it on held-out, and a sign test separates neither difference. Against the full e3 it loses
clearly on held-out after repair. The form a 32 GB machine would run, about 12 GB at 8-bit by estimate, has not been
built or scored, so the question in the README stays open until it is.

Training took 3 h 7 min on host-c, 12.9 s a step. Scoring took about 2.5 h for the final step and 2 h 46 min for step 575,
about four times the MoE's 37 min, on the 1.0 grader (2026-10-30-1). Both passes had no infra failures, empty generations
or length caps, and all 126 prompts are byte-identical to e3's. `compare.py` prints every table below.

## Results: no guide, first attempt (after repair)

| | main (73) | held-out (53) | all (126) |
|---|---|---|---|
| **Gemma 4 12B, epoch 3 (final), 16-bit** | **67 (69)** | **30 (36)** | **97 (105)** |
| **Gemma 4 12B, epoch 2 (step 575; `../gemma4-12b-v2-ep2/`), 16-bit** | **64 (68)** | **27 (31)** | **91 (99)** |
| e3, mxfp4 (20.9 GB) | 64 (68) | 33 (40) | 97 (108) |
| e3 (published) | 65 (70) | 39 (47) | 104 (117) |
| untrained Gemma 4 12B + 1.0 guide, q8_0 | 48 (67) | 26 (41) | 74 (108) |

e3's report was graded on 2026-10-21-8, and replaying its programs on 2026-10-30-1 moves nothing. Its held-out score
helped choose it for release (`../qwen36-35b-a3b-e3/RESULTS.md`), so its 39 is slightly flattering. The Gemma rows are
one seed (3407) at T=0.

| suite | final | epoch 2 | e3 | e3, mxfp4 | guided |
|---|---|---|---|---|---|
| tasks_v2_pos (35) | 33 (34) | 34 (35) | 33 (35) | 33 (34) | 29 (35) |
| tasks_hard_v2 (14) | 13 (13) | 8 (9) | 13 (14) | 13 (14) | 3 (11) |
| tasks_hard_nontoon (5) | 3 (4) | 3 (5) | 2 (4) | 2 (4) | 2 (4) |
| tasks_cs (19) | 18 (18) | 19 (19) | 17 (17) | 16 (16) | 14 (17) |
| tasks_frontier (15) | 10 (13) | 8 (11) | 10 (14) | 11 (13) | 10 (13) |
| tasks_frontier_algo (14) | 8 (10) | 9 (9) | 10 (11) | 11 (12) | 6 (11) |
| tasks_frontier_spec (12) | 6 (6) | 7 (7) | 9 (11) | 4 (6) | 6 (8) |
| tasks_sealed_toon (12) | 6 (7) | 3 (4) | 10 (11) | 7 (9) | 4 (9) |

Tasks only one model passes, with two-sided sign tests:

| final against | | first attempt | after repair |
|---|---|---|---|
| epoch 2 | main | 5 to 2 (p=0.45) | 4 to 3 (p=1.00) |
| | held-out | 11 to 8 (p=0.65) | 11 to 6 (p=0.33) |
| | all | 16 to 10 (p=0.33) | 15 to 9 (p=0.31) |
| e3, mxfp4 | main | 4 to 1 (p=0.38) | 3 to 2 (p=1.00) |
| | held-out | 11 to 14 (p=0.69) | 9 to 13 (p=0.52) |
| | all | 15 to 15 (p=1.00) | 12 to 15 (p=0.70) |
| e3 | main | 4 to 2 (p=0.69) | 3 to 4 (p=1.00) |
| | held-out | 8 to 17 (p=0.11) | 3 to 14 (p=0.01) |
| | all | 12 to 19 (p=0.28) | 6 to 18 (p=0.02) |
| guided | main | 21 to 2 (p<0.001) | 5 to 3 (p=0.73) |
| | held-out | 15 to 11 (p=0.56) | 7 to 12 (p=0.36) |
| | all | 36 to 13 (p=0.001) | 12 to 15 (p=0.70) |

## What the training bought

- **First attempts without a guide.** Against the same base with the guide, the fine-tune gains 23 first attempts, 19 of
  them on main, and hard_v2 moves from 3 to 13. After repair the two are level, as the guided model's repairs recover 34
  of its 52 failures.
- **Not repair.** The two Gemma steps fix the fewest failures of any row in the next table.

## Where it falls short

Failing programs by class, coded diagnostic / wrong return code / wrong output, over all 126 tasks:

| | first attempt | after repair | failures repair fixed |
|---|---|---|---|
| **Gemma, final** | **14 / 7 / 8** | **8 / 2 / 11** | **8 of 29** |
| Gemma, epoch 2 | 18 / 2 / 15 | 12 / 3 / 12 | 8 of 35 |
| e3 | 16 / 2 / 4 | 6 / 0 / 3 | 13 of 22 |
| e3, mxfp4 | 19 / 2 / 8 | 11 / 2 / 5 | 11 of 29 |
| guided | 31 / 1 / 20 | 9 / 0 / 9 | 34 of 52 |

- **Coded failures on first attempts are not the gap:** 14 against e3's 16.
- **Return codes and wrong output are:** 15 first attempts against e3's 6. After repair, wrong output is 11 against 3, and
  those programs give repair no diagnostic to work from.
- **The gap to e3 is on held-out:** main is 67 to 65, held-out 30 to 39. Held-out's failures were not read, so the suites
  can go on judging training data they did not shape.

## Epoch 2 or epoch 3

The final step stays. It is ahead on all four numbers, so main alone gives the same answer as all 126 tasks, and the
held-out suites do not have to choose.

| third pass | tasks changed | main | held-out | wrong output |
|---|---|---|---|---|
| e3 | 19 (8 lost, 11 gained) | 68 to 65 | 33 to 39 | 8 to 4 |
| v2 | 20 (12 lost, 8 gained) | 63 to 63 | 35 to 31 | 8 to 16 |
| **Gemma** | **26 (10 lost, 16 gained)** | **64 to 67** | **27 to 30** | **15 to 8** |

- **No run's swing is significant alone.** Gemma's is p=0.33. It is the largest of the three, and only 20 of the 126 first
  programs are byte-identical between the two steps.
- **hard_v2 carries most of the main gain,** 8 to 13, with 5 tasks gained and none lost. That split alone is p=0.06, and it
  was spotted after the fact.
- **The third pass now stands at two gains and one loss on held-out.** That is still no measured basis for preferring 3
  epochs over 2, and `../qwen36-35b-a3b-e3-t02/` found none either.
- **Eval loss did not follow.** The minimum was 0.0936 at epoch 1.91 and the last eval was 0.1018 at epoch 3, a rise like e3's and
  v2's, while the benchmark went up.

## What was checked

- **Served prompt:** vLLM's `/tokenize` on a chat message gives the same tokens as the trainer's template, with one BOS and
  the empty thought channel at the end. The reports' constant 11-token gap is the template trimming the harness prompt,
  which starts with spaces and ends with a newline.
- **Which model was scored:** each pass's reports name its own served model, and both used the same grader binary.
- **Program shape:** median completion 278 tokens at the final step and 305.5 at step 575, against e3's 269.

## How much of this is chance

The controls in `../qwen36-35b-a3b-v2/RESULTS.md` apply here too: nearly identical training differs on 9 of 126 first
attempts, and held-out seed noise has not been measured on this model. Corrected on 2026-11-04: this line said never, but the
27B's three seeds were. A difference of a few tasks between any two rows above cannot be read as more than that.

## Next

- **The final step is the adapter to carry forward.** Step 575 is not worth converting.
- **The open test is the 8-bit Mac form.** It needs the merged export converted, which is untried for this architecture, and
  scored on all 126 tasks with its size and speed measured. e3 lost 3 held-out first attempts at int8 and 6 at mxfp4, so
  Gemma's 30 may not keep its lead over the guided base's 26, which is already at 8-bit. If it falls clearly below that,
  the guide is the better advice for hard tasks on such a machine.
- **Scoring time hints at the cost.** This 12B took about four times as long to score as the MoE, whose active parameter
  count is about 3B. That may be the dense model's cost per token. It was not measured separately, so the Mac test should.

## Published (2026-11-03)

The final step is public under apache-2.0, with `google/gemma-4-12B-it` as its base. The cards are
`training/config/hf_release/README.gemma.{merged,lora}.md`.

| repo | Hub revision | weights | SHA-256 starts |
|---|---|---|---|
| `emkey1/Gemma-4-12B-Aether` | `f50652224d82` | `model.safetensors`, 23.9 GB, one file | `cee81587546e80c9` |
| `emkey1/Gemma-4-12B-Aether-LoRA` | `e17a798a09bf` | `adapter_model.safetensors`, 525 MB | `459ce08deb9a7879` |

- **Same bytes:** the merged file's hash matches the export copied to the Mac for the 8-bit and 4-bit forms, and the adapter's
  matches `train/final`. The one change is the adapter config's `base_model_name_or_path`, now the Hub id.
- **Checked before going public:** every file against its local hash, with both repos still private.
- **Checked after, with no token:** both repos are open, the cards match the committed ones, and the adapter config, merged
  config and both tokenizers load from the Hub. The chat template renders one BOS.
- **Not checked:** downloading the published weights and generating from them.
- **Upload time:** 3 h 8 min for both repos.
- **Later card commits, same day:** linking the int8 form (`../gemma4-12b-v2-q8/RESULTS.md`) moved the revisions to `23f4fd77b0ed` and
  `ea051eda4b79`. Their weights did not change.
