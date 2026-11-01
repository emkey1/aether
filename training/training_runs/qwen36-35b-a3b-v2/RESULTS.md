# qwen36-35b-a3b-v2: results (2026-11-01)

**v2 does not replace `qwen36-35b-a3b-e3`.** It is level with e3 on the language and worse on program logic, so it
loses on the benchmark, mostly on held-out. Why is not established. The dataset is the only thing that changed, but
one run cannot separate the data from a poor third pass.

Training took 12 h 44 min on host-c and scoring 37 min, on the 1.0 grader (2026-10-30-1). There were no infra failures,
empty generations or length caps, and all 126 prompts are byte-identical to e3's.

## Results: no guide, first attempt (after repair)

| | main (73) | held-out (53) |
|---|---|---|
| **v2, epoch 3 (final)** | **63 (68)** | **31 (42)** |
| e3, epoch 3 (published) | 65 (70) | 39 (47) |
| e3, epoch 2 | 68 (70) | 33 (42) |
| e3, int8 | 65 (70) | 36 (44) |
| e3, mxfp4 (20.9 GB) | 64 (68) | 33 (40) |
| untrained MoE + 1.0 guide, int8 | 60 (71) | 33 (43) |

The e3 and e3-epoch-2 reports were graded on 2026-10-21-8. Replaying their programs on 2026-10-30-1 moves nothing.
e3's held-out score helped choose it for release (`../qwen36-35b-a3b-e3/RESULTS.md`), so its 39 is slightly flattering.

| suite | v2 | e3 |
|---|---|---|
| tasks_v2_pos (35) | 33 (34) | 33 (35) |
| tasks_hard_v2 (14) | 11 (14) | 13 (14) |
| tasks_hard_nontoon (5) | 3 (3) | 2 (4) |
| tasks_cs (19) | 16 (17) | 17 (17) |
| tasks_frontier (15) | 10 (14) | 10 (14) |
| tasks_frontier_algo (14) | 7 (10) | 10 (11) |
| tasks_frontier_spec (12) | 6 (8) | 9 (11) |
| tasks_sealed_toon (12) | 8 (10) | 10 (11) |

Tasks only one model passes, v2 against e3, with two-sided sign tests:

| | first attempt | after repair |
|---|---|---|
| main | 2 to 4 (p=0.69) | 0 to 2 (p=0.50) |
| held-out | 4 to 12 (p=0.08) | 2 to 7 (p=0.18) |
| all | 6 to 16 (p=0.05) | 2 to 9 (p=0.07) |

Against e3 at int8, the same weights on the same grader as v2, the gap is smaller: 9 to 16 on first attempts (p=0.23)
and 4 to 8 after repair (p=0.39).

## What v2 lost

Failing programs by class, coded diagnostic / wrong return code / wrong output:

| | first attempt | after repair |
|---|---|---|
| e3 | 16 / 2 / 4 | 6 / 0 / 3 |
| e3, epoch 2 | 16 / 1 / 8 | 10 / 0 / 4 |
| `qwen36-35b-a3b-e2` | 16 / 2 / 8 | 8 / 2 / 7 |
| **v2** | **15 / 1 / 16** | **5 / 0 / 11** |

- **The loss is all wrong output.** Coded failures are level with e3's, and after repair v2 has the fewest of these
  runs. Repair also recovers less, because a program that runs and prints the wrong answer gets no diagnostic to work from.
- **These are ordinary logic slips, not language errors:**
  - `spec_leap_year_rules`: the day of year for 1 March is computed as `31 + feb`, one short.
  - `algo_bignum_add`: the digits are reversed twice, so the sum comes out backwards.
  - `spec_histogram_bins`: 30 counts as outside, although the last bin is closed.
  - `hard2_event_sessionize`: the user's last time is updated before the gap test, so every event reads "continues".
- **Maps barely show up in the benchmark.** 7 first attempts use `Map`, against e3's 3, and 4 of the 7 pass. That is too
  few programs to say what d5's 65 map pairs taught.

## What was checked and ruled out

- **Training stack:** the same trainer image and libraries as e3. `train.sh` differs only in the run name, and
  `run_metadata.json` differs only in the dataset, its record count and the kept steps.
- **Serving stack:** the host's only vLLM image was built before e3 was trained. Requests and harness settings are identical.
- **Grader:** e3's 126 first attempts replay on 2026-10-30-1 as 104 passes to 104, with no flips.
- **Program shape:** median completion 280 tokens against 269, and 85 single-function programs against 83.

## How much of this is chance

- **Same data, nearly the same training:** `qwen36-35b-a3b-e2` and e3 at epoch 2 differ on 9 of 126 first attempts, split
  5 to 4.
- **Same data, different seeds:** the three `qwen38-27b-r7` seeds differ on 1 to 5 of the 73 main tasks. No seed
  runs have held-out scores.
- **v2 against e3:** 22 tasks, split 6 to 16. That is more churn than either control, and lopsided, so it is
  probably not all chance. It is still one run against a model partly chosen for this score.

## Eval loss

Eval loss was lower than e3's throughout, and that did not carry over to the benchmark. The minimum was 0.0793 at
epoch 1.82 against e3's 0.0905, and the last eval was 0.0898 at epoch 2.94 against 0.1052 at epoch 2.99. The two runs
use different 60-record splits. As in e3, eval loss did not predict the benchmark.

## Next

Not yet run: the step-575 adapter (`train/keep/checkpoint-575` on the host), about an hour to merge, serve and score. Its
natural comparison is e3 at epoch 2, 68 (70) / 33 (42), where the same-data noise is known from the 9-task control.

- If v2 at epoch 2 matches it, the data is sound and this third pass did the damage.
- If it trails there too, the data becomes the main suspect, and d5 is the place to look.

The held-out suites should not pick between these adapters, since they already helped choose e3.
