# Qwen3.6-35B-A3B, a 2-epoch schedule (2026-10-23), and the release decision

The question: `qwen36-35b-a3b-e3`'s epoch 3 beat its own epoch 2 on held-out (39 vs 33). Its epoch 2 was cut
mid-schedule (learning rate still about a third of its peak), while epoch 3 ended with the rate decayed to zero. So
epoch 3 differed in two ways: one more pass, and a fully decayed learning rate. This run's schedule ends at epoch 2,
which separates the two. Same data, recipe, seed and scoring as `../qwen36-35b-a3b-e3/`; no early stopping; last step
exported. Training: 7 h 25 min; final eval loss 0.0918 (e3 ended at 0.1052).

## Results: no guide, first attempt (after repair)

| suite | e3 at epoch 2 (mid-schedule) | **this run (2-epoch schedule)** | e3 final (epoch 3) |
|---|---|---|---|
| tasks_v2_pos (35) | 35 (35) | 35 (35) | 33 (35) |
| tasks_hard_v2 (14) | 13 (14) | 12 (13) | 13 (14) |
| tasks_hard_nontoon (5) | 4 (5) | 3 (4) | 2 (4) |
| tasks_cs (19) | 16 (16) | 17 (18) | 17 (17) |
| **main (73)** | 68 (70) | **67 (70)** | 65 (70) |
| tasks_frontier (15) | 8 (13) | 9 (12) | 10 (14) |
| tasks_frontier_algo (14) | 9 (10) | 9 (10) | 10 (11) |
| tasks_frontier_spec (12) | 8 (9) | 7 (8) | 9 (11) |
| tasks_sealed_toon (12) | 8 (10) | 8 (9) | 10 (11) |
| **held-out (53)** | 33 (42) | **33 (39)** | 39 (47) |
| **Python (126)** | 48 (91) | **43 (82)** | 44 (82) |

Aether on host-c, bf16, grader 2026-10-21-8; Python on the scoring Mac, int8.

## Reading

- **The decayed learning rate did not bring epoch 3's held-out gain to epoch 2.** This run lands where e3's
  mid-schedule epoch 2 did (33 held-out). The third pass itself carries the gain.
- On the main suites the three are effectively tied: 70 after repair each, 65-68 on the first attempt.
- Eval loss pointed the wrong way again: this run ends lower (0.0918 vs 0.1052) and generalises worse.

## Release decision (owner, 2026-10-24): `qwen36-35b-a3b-e3`, overriding the pre-set rule

- **The rule fixed in this run's config before training:** higher main after repair, then higher main first attempt,
  then this run. After repair the two tie (70 = 70). On the first attempt this run leads 67 to 65, so the rule picks
  this run.
- **The override.** That deciding gap is two tasks, about the run-to-run noise. Meanwhile e3 leads on held-out by 6
  first attempts and 8 after repair, ahead on 3 of the 4 held-out suites. Python costs the same (82 after repair
  each). The owner chose e3 as the model most likely to serve users well.
- **Disclosure.** This choice used the held-out suites to choose between two finalists, so e3's held-out score (39)
  is no longer an untouched estimate. With two candidates the selection bias is small, but it is not zero. Every
  report of e3's held-out number says so.
