# qwen36-35b-a3b-v2: results (2026-11-01)

**v2 does not replace `qwen36-35b-a3b-e3`, and the new data is not what cost it.** At epoch 2 v2 is level with e3 at
epoch 2. The gap opens in the third pass, where v2 doubled its wrong-output failures while e3 halved them. So the
1.0 data neither helped nor hurt measurably on this benchmark, and the held-out gain that set e3's 3-epoch recipe did
not repeat.

Training took 12 h 44 min on host-c. Each scoring pass took about 37 min, on the 1.0 grader (2026-10-30-1). Both
passes had no infra failures, empty generations or length caps, and all 126 prompts are byte-identical to e3's.

## Results: no guide, first attempt (after repair)

| | main (73) | held-out (53) |
|---|---|---|
| **v2, epoch 3 (final)** | **63 (68)** | **31 (42)** |
| **v2, epoch 2 (step 575; `../qwen36-35b-a3b-v2-ep2/`)** | **63 (69)** | **35 (42)** |
| e3, epoch 3 (published) | 65 (70) | 39 (47) |
| e3, epoch 2 | 68 (70) | 33 (42) |
| `qwen36-35b-a3b-e2` | 67 (70) | 33 (39) |
| e3, int8 | 65 (70) | 36 (44) |
| e3, mxfp4 (20.9 GB) | 64 (68) | 33 (40) |
| untrained MoE + 1.0 guide, int8 | 60 (71) | 33 (43) |

The e3, e3-epoch-2 and e2 reports were graded on 2026-10-21-8. Replaying their programs on 2026-10-30-1 moves nothing.
e3's held-out score helped choose it for release (`../qwen36-35b-a3b-e3/RESULTS.md`), so its 39 is slightly flattering.

| suite | v2 | v2, epoch 2 | e3 | e3, epoch 2 |
|---|---|---|---|---|
| tasks_v2_pos (35) | 33 (34) | 33 (35) | 33 (35) | 35 (35) |
| tasks_hard_v2 (14) | 11 (14) | 10 (13) | 13 (14) | 13 (14) |
| tasks_hard_nontoon (5) | 3 (3) | 4 (4) | 2 (4) | 4 (5) |
| tasks_cs (19) | 16 (17) | 16 (17) | 17 (17) | 16 (16) |
| tasks_frontier (15) | 10 (14) | 10 (14) | 10 (14) | 8 (13) |
| tasks_frontier_algo (14) | 7 (10) | 11 (12) | 10 (11) | 9 (10) |
| tasks_frontier_spec (12) | 6 (8) | 6 (6) | 9 (11) | 8 (9) |
| tasks_sealed_toon (12) | 8 (10) | 8 (10) | 10 (11) | 8 (10) |

Tasks only one model passes, with two-sided sign tests:

| | first attempt | after repair |
|---|---|---|
| v2 against e3: main | 2 to 4 (p=0.69) | 0 to 2 (p=0.50) |
| v2 against e3: held-out | 4 to 12 (p=0.08) | 2 to 7 (p=0.18) |
| v2 against e3: all | 6 to 16 (p=0.05) | 2 to 9 (p=0.07) |
| v2 epoch 2 against e3 epoch 2: main | 2 to 7 (p=0.18) | 1 to 2 (p=1.00) |
| v2 epoch 2 against e3 epoch 2: held-out | 10 to 8 (p=0.82) | 7 to 7 (p=1.00) |
| v2 epoch 2 against e3 epoch 2: all | 12 to 15 (p=0.70) | 8 to 9 (p=1.00) |
| v2 epoch 2 against e3: all | 10 to 16 (p=0.33) | 4 to 10 (p=0.18) |

Against e3 at int8, the same weights on the same grader as v2, v2's gap is smaller: 9 to 16 on first attempts (p=0.23)
and 4 to 8 after repair (p=0.39).

## What v2 lost

Failing programs by class, coded diagnostic / wrong return code / wrong output:

| | first attempt | after repair |
|---|---|---|
| e3, epoch 2 | 16 / 1 / 8 | 10 / 0 / 4 |
| `qwen36-35b-a3b-e2` | 16 / 2 / 8 | 8 / 2 / 7 |
| e3, epoch 3 | 16 / 2 / 4 | 6 / 0 / 3 |
| **v2, epoch 2** | **16 / 4 / 8** | **6 / 1 / 8** |
| **v2, epoch 3** | **15 / 1 / 16** | **5 / 0 / 11** |

- **The loss is all wrong output.** Coded failures are level with e3's at both epochs, and after repair v2 has the
  fewest of these runs. Repair also recovers less, because a program that runs and prints the wrong answer gets no
  diagnostic to work from.
- **These are ordinary logic slips, not language errors:**
  - `spec_leap_year_rules`: the day of year for 1 March is computed as `31 + feb`, one short.
  - `algo_bignum_add`: the digits are reversed twice, so the sum comes out backwards.
  - `spec_histogram_bins`: 30 counts as outside, although the last bin is closed.
  - `hard2_event_sessionize`: the user's last time is updated before the gap test, so every event reads "continues".
- **Maps barely show up in the benchmark.** 7 first attempts use `Map`, against e3's 3, and 4 of the 7 pass. That is too
  few programs to say what d5's 65 map pairs taught.

## Data or third pass?

- **The data.** At epoch 2, wrong output is 8 in all three runs (v2, e3, e2), and v2 and e3 are level overall. The
  1.0 data cost nothing by then, and gained nothing measurable either.
- **The third pass.** It changes about 20 of the 126 first attempts in both runs, in opposite directions:

  | third pass | tasks changed | main | held-out | wrong output |
  |---|---|---|---|---|
  | e3 | 19 (8 lost, 11 gained) | 68 to 65 | 33 to 39 | 8 to 4 |
  | v2 | 20 (12 lost, 8 gained) | 63 to 63 | 35 to 31 | 8 to 16 |

  Neither swing is significant alone: v2's is p=0.50 and e3's is p=0.65. They are also about double the 9-task
  control below. The recipe's "the third pass carries the held-out gain" rests on e3 alone, and this run does not repeat it.
- **A later peak is not supported.** A larger dataset could have delayed the peak, but wrong output rises from epoch 2
  to epoch 3 rather than falling, and v2 took more optimizer steps than e3 (867 against 753). The evidence does
  not justify a 4-epoch run.

## What was checked and ruled out

- **Training stack:** the same trainer image and libraries as e3. `train.sh` differs only in the run name, and
  `run_metadata.json` differs only in the dataset, its record count and the kept steps.
- **Serving stack:** the host's only vLLM image was built before e3 was trained. Requests and harness settings are identical.
- **Grader:** e3's 126 first attempts replay on 2026-10-30-1 as 104 passes to 104, with no flips.
- **Program shape:** median completion 280 tokens against 269, and 85 single-function programs against 83.
- **The merge:** the step-575 adapter was merged by `merge_checkpoint.sh`, the trainer's own export path.

## How much of this is chance

- **Same data, nearly the same training:** `qwen36-35b-a3b-e2` and e3 at epoch 2 differ on 9 of 126 first attempts, split
  5 to 4.
- **Same data, different seeds:** the three `qwen38-27b-r7` seeds differ on 1 to 5 of the 73 main tasks. No seed
  runs have held-out scores, so held-out's seed noise has never been measured.
- **v2 against e3:** 22 tasks, split 6 to 16. e3 was partly chosen for this score, and the epoch 2 result shows
  the gap comes from the third pass alone. A second seed would show whether that pass is a real effect or luck, in either run.

## Eval loss

Eval loss was lower than e3's throughout, and that did not carry over to the benchmark. The minimum was 0.0793 at
epoch 1.82 against e3's 0.0905, and the last eval was 0.0898 at epoch 2.94 against 0.1052 at epoch 2.99. The two runs
use different 60-record splits. It rose in both third passes, while the benchmark went up in one and down in the other.

## Next

- **e3 stays the release model**, and e3 at mxfp4 stays the 32 GB candidate. v2 at epoch 2 does not beat e3 either.
- **The cheap test left is a second seed of v2**, about 13 hours on the training host. It would measure held-out seed
  noise for the first time and show whether the third pass is worth its 4 hours in future runs.
- The held-out suites should not pick between these adapters, since they already helped choose e3.
