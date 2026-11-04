# gemma4-12b-v3: does data from a fine-tune's own failures move its benchmark? (started 2026-11-04)

Three new runs test `../../PLAN-failure-targeted-batch.md`. That plan holds the question and the reading, both fixed before training
started. `compare.py` computes the reading and prints its verdict from the three conditions.

| run | dataset | seed | |
|---|---|---|---|
| `gemma4-12b-v2` | `v2-20261101` | 3407 | done on 2026-11-02 |
| `gemma4-12b-v3` | `v3-20261104` | 3407 | one change from `gemma4-12b-v2`: the dataset |
| `gemma4-12b-v2-s42` | `v2-20261101` | 42 | one change from `gemma4-12b-v2`: the seed; the first measure of held-out seed noise |
| `gemma4-12b-v3-s42` | `v3-20261104` | 42 | one change from `gemma4-12b-v3`: the seed |

All are scored at 16 bits with no guide, at T=0, on the eight suites, by the 1.0 grader on the training host.

## The dataset

`v3-20261104` is `v2-20261101` plus the 298 Aether pairs of batches `f1`, `f2` and `f3-20261103`, 11.2% of the records.

- **Checked against v2:** none of v2's 2,278 instruction records or 87 repair records is missing, and they keep their order. The only field
  that differs on them is the stamp naming the binary, since v3 was built with the 1.0 release binary and v2 with a build of the same
  language version. The gates match v2's, and no new pair is too close to a board reference.
- **Python pairs are left out.** The batches' 298 Python pairs stay in their folders. The cap of 220 chooses by a hash of each id, so
  adding them would replace some of v2's Python pairs, and the arm would differ by more than the targeted pairs.
- **Steps:** 978 at 3 epochs, against 867 on v2, so a v3 run takes about 3.5 hours to train.

## Running it

`run_chain.sh` runs on the training host in a detached session, in the order of the table. It stops at the first failure.

The host's image and video services are stopped for the chain and restarted at its end. Its resident llama-server was already stopped when
the chain started, not by this work, so the chain leaves it alone.

## Log

- 2026-11-04: dataset built and run configs committed before training.
- 2026-11-04 06:58Z: `run_chain.sh` started on the training host, with the first run training.
