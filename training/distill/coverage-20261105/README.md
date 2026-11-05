# coverage-20261105: are Gemma's failures a gap in its training data?

Comparing two seeds of `gemma4-12b-v2` on the 1,776 fresh tasks of `f1`, `f2` and `f3-20261103` showed private habits, and one of them traced back to data that barely teaches the idiom (`../fs42-20261105/`). This audit asks how far that explanation goes. `audit.py` reads pass or fail, diagnostics and which builtins a program calls, against the records of `v2-20261101`: 2,058 Aether instruction records and 87 repair records. It needs those two files, which stay on the training host.

**Mostly no. Coverage explains a few narrow gaps, not the bulk of the failures.**

## Where coverage is thin

| idiom | training records | published seed | seed 42 | e3 | evidence |
|---|---:|---:|---:|---:|---|
| `sorted_by`, `sorted_by_desc` | 3, 6 | 7, 11 programs | 2, 0 programs | 0, 3 programs | seed 42 invents a sort spec or comparator: 15 `sorted` argument errors and 4 unclosed comparators |
| `toon_key_or` | 10 | 0 | 2 | | on the 298 tasks with a pair: solvers use it 13 times |
| `toon_null` | 10 | 2 | 2 | | on the same tasks: solvers use it 13 times |
| repair records, all kinds | 87 | | | | failures repair fixed vary from 8 of 29 to 20 of 30 on the benchmark |

Of the 195 builtins in the guide's list, 18 never appear in training and 102 appear in at most 5 records. Most are never reached for on these tasks, so that count alone does not show a gap.

The other private habits are not a matter of records either: `Map[Int, ...]` fails 5 and 11 times, against 0 on e3, and names that do not exist, such as `pad_left`, `reverse` and `map`, take 2 to 6 tasks each.

## Where it is not

- **JSON fails despite generous coverage.** The `toon_*` family has 50 to 250 records each, and programs calling it fail at 23% to 74%, far above the 18% and 15% overall. `toon_parse_file` has 102 records, and programs using it fail at 74% on the published seed and 36% on seed 42. These tasks may simply be harder, which this cannot separate, but the gap between the seeds shows the count of records is not what decides it. 100 of the 298 targeted pairs were JSON and data files, and `../../training_runs/gemma4-12b-v3/` saw no gain on the nearest held-out suite.
- **Wrong output is about 60% of failures** and has no diagnostic to count. Nothing here reaches it.
- **Reach:** the sort habit and `Map[Int, ...]` together account for at most 30 of seed 42's 272 failures (11%) and fewer of the published seed's 322, before counting that some would still fail once fixed.

## What this suggests

A small batch teaching two-key sorting, and perhaps `toon_key_or` and `toon_null`, is a reasonable next test once a fresh task set exists to read it. It would not be expected to move much alone. More JSON examples, by this count, are not the obvious lever. Nothing here has been trained or tested.
