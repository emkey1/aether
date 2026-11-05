# gemma4-12b-v3: results (2026-11-05)

**No gain by the reading fixed before training.** The 298 pairs drawn from Gemma's own failures give 33.5 held-out first attempts of 53, averaged over
two seeds, against 30.5. That is +5.7 points with a 95% interval of [-5.7, +17.0], and a gain needs the interval above zero. After repair the direction
reverses, 38.5 against 40.5. The control arm does not run, and `gemma4-12b-v2` stays the adapter to carry forward.

The interval still allows a small real gain, and it excludes one above about 9 of the 53 tasks.

**The second baseline seed says more than the verdict does.** With the same recipe and data it scores 31 (45) on held-out, against the published seed's
30 (36). Changing the seed moved about as many tasks as adding the data did, and several earlier statements about Gemma turn out to describe one set of
weights rather than the recipe. They are corrected in `../gemma4-12b-v2/RESULTS.md` and `../gemma4-12b-v2-q8/RESULTS.md`.

The chain took 18 h 2 min on host-c: 6 h 17 min, 5 h 37 min and 6 h 7 min per run. All passes had no infra failures, empty generations or length caps, on
the 1.0 grader (2026-10-30-1, the same hash on all four runs), and all 126 prompts match `gemma4-12b-v2`'s. `compare.py` prints the reading and `noise.py`
the seed tables. Both read pass or fail and the failure's class, so held-out failures stay unread.

## Results: no guide, T=0, first attempt (after repair)

| | dataset | seed | main (73) | held-out (53) | all (126) |
|---|---|---|---|---|---|
| baseline, `gemma4-12b-v2` | `v2-20261101` | 3407 | 67 (69) | 30 (36) | 97 (105) |
| baseline, `gemma4-12b-v2-s42` | `v2-20261101` | 42 | 65 (71) | 31 (45) | 96 (116) |
| targeted, `gemma4-12b-v3` | `v3-20261104` | 3407 | 65 (70) | 33 (37) | 98 (107) |
| targeted, `gemma4-12b-v3-s42` | `v3-20261104` | 42 | 65 (69) | 34 (40) | 99 (109) |
| e3 (published), for scale | `v1-20261021` | 3407 | 65 (70) | 39 (47) | 104 (117) |

## The fixed reading

| first attempt, mean over two seeds | targeted | baseline | difference | 95% interval |
|---|---:|---:|---:|---|
| **held-out (53)** | **33.5** | **30.5** | **+5.7 points** | **[-5.7, +17.0]** |
| main (73) | 65.0 | 66.0 | -1.4 points | [-7.5, +4.8] |

| a gain needs all three | |
|---|---|
| the 95% interval is above zero | **no** |
| the difference is larger than the gap between the baseline seeds (1.9 points, one task) | yes |
| main's interval is not wholly below zero | yes |

The second condition is weaker than it looked when it was fixed. The baseline seeds' totals differ by one task, while 15 of their 53 first attempts
differ. It decided nothing here, since the first condition failed, but a later reading should not lean on a gap between totals.

## Reported, not deciding

| after repair, mean over two seeds | targeted | baseline | difference | 95% interval |
|---|---:|---:|---:|---|
| held-out (53) | 38.5 | 40.5 | -3.8 points | [-13.2, +5.7] |
| main (73) | 69.5 | 70.0 | -0.7 points | [-4.8, +4.1] |

| suite, mean over two seeds | targeted | baseline |
|---|---|---|
| tasks_v2_pos (35) | 32.5 (34.0) | 32.5 (34.5) |
| tasks_hard_v2 (14) | 13.0 (14.0) | 13.0 (13.0) |
| tasks_hard_nontoon (5) | 1.0 (3.0) | 3.0 (4.0) |
| tasks_cs (19) | 18.5 (18.5) | 17.5 (18.5) |
| tasks_frontier (15) | 10.5 (12.5) | 10.0 (13.5) |
| tasks_frontier_algo (14) | 11.5 (12.5) | 9.0 (12.0) |
| tasks_frontier_spec (12) | 7.5 (9.0) | 6.5 (7.5) |
| tasks_sealed_toon (12) | 4.0 (4.5) | 5.0 (7.5) |

Failing programs by class, coded diagnostic / wrong return code / wrong output, over all 126 tasks:

| | first attempt | after repair | failures repair fixed |
|---|---|---|---|
| baseline, seed 3407 | 14 / 7 / 8 | 8 / 2 / 11 | 8 of 29 |
| baseline, seed 42 | 23 / 1 / 6 | 4 / 1 / 5 | 20 of 30 |
| targeted, seed 3407 | 17 / 1 / 10 | 8 / 2 / 9 | 9 of 28 |
| targeted, seed 42 | 16 / 1 / 10 | 8 / 3 / 6 | 10 of 27 |

- **+3 at both seeds is not the same +3.** On held-out, seed 3407 splits 11 to 8 and seed 42 splits 7 to 4. One task was gained at both seeds and three
  were lost at both. The totals repeat and the tasks do not.
- **Wrong output did not respond.** It is 180 of the 298 pairs' failure class, and on first attempts it reads 8 and 6 at baseline, 10 and 10 targeted. The
  plan warned that it might not.
- **Nor did the JSON pairs, as far as one suite can show.** 100 pairs are JSON and data files. `tasks_sealed_toon`, the nearest held-out suite, goes from
  5.0 (7.5) to 4.0 (4.5). This was spotted after the fact, on 12 tasks.

## What a seed alone does

| | first attempts differing, main / held-out | held-out, first attempt | held-out, after repair |
|---|---|---|---|
| baseline, seed 42 against 3407 | 6 / 15 | 8 to 7 | 11 to 2 (p=0.02) |
| targeted, seed 42 against 3407 | 6 / 9 | 5 to 4 | 5 to 2 (p=0.45) |
| targeted against baseline, seed 3407 | 8 / 19 | 11 to 8 (p=0.65) | 9 to 8 (p=1.00) |
| targeted against baseline, seed 42 | 6 / 11 | 7 to 4 (p=0.55) | 2 to 7 (p=0.18) |

- **A seed moves about as many tasks as the data.** The seed changes 21 and 15 of 126 first attempts, and the data 27 and 17. Across any of these pairs only
  20 to 22 of the 126 first programs are byte-identical. Median completions stay within a token, 277.5 to 278.5.
- **A sign test between two runs compares those weights, not their recipes.** Two seeds of one recipe separate at p=0.02 on held-out after repair and p=0.01
  over all 126. Earlier tests between single runs have to be read that way, including e3 over seed 3407 on held-out after repair, 14 to 3. Seed 42 is
  within 2 tasks of e3 there.
- **e3's first-attempt lead held.** No Gemma run passes more than 34 held-out first attempts, against e3's 39. Only one seed of e3 has been scored, so its 39
  carries the same uncertainty.
- **Repair is what varies most.** Three of the four runs fix about a third of their failures, and seed 42's baseline fixes two-thirds. It starts with the
  most coded failures, 23, and ends with the fewest, 4. Why that one run repairs so much better was not looked into.
- **Seed 42's 116 is flattering.** It is the best of four runs, seen on the same suites that report it, and on main alone the after-repair split is 3 to 1.
- **Eval loss did not track any of this.** The minimum and last values are 0.0936 and 0.1018 for the baseline at 3407, 0.1126 and 0.1201 at 42, 0.0924 and
  0.1069 for the targeted run at 3407, 0.0905 and 0.1006 at 42. The run with the highest eval loss has the best score after repair. The 60 eval records may
  differ by seed, which was not checked.

## Next

- **The plan stops here.** The control arm does not run. e3's unsolved failures (`../../distill/fe3-20261104/`) are not solved, and no MoE run follows.
- **The 298 pairs stay in `distilled/`,** which the dataset builder takes by default. Nothing here shows that they help or harm, so the next dataset should
  keep or drop them on purpose.
- **The weights stay on host-c,** about 27 GB per run, and none of them is published.
- **Seed 42's baseline at int8:** 65 (72) / 31 (42), 12 to 4 after repair against the published int8 (p=0.08), which misses the bar fixed for it
  (`../gemma4-12b-v2-s42-q8/RESULTS.md`). The published form stays.
- **Not decided:** whether to update the published cards' seed-noise line, which still says about 9 of 126.
