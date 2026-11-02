# qwen36-35b-a3b-e3-t02: e3's third pass, measured with pass rates (started 2026-11-02)

e3 and its epoch 2 adapter, re-scored with no new training. `../qwen36-35b-a3b-e3-ep2-t02/` is the epoch 2 half.

**Why.** e3's 3-epoch recipe rests on one result: the third pass took held-out from 33 to 39 at T=0, one sample per
task. v2 did not repeat it (`../qwen36-35b-a3b-v2/RESULTS.md`), and neither third pass is significant alone. T=0 gives
each task one bit, and a flipped near-tie can change a whole program. Several samples per task give a pass rate instead.

**What runs.**
- T=0.2, 5 repeats, seeds 42+r (D37b's sampling at more repeats), 2 repair attempts, no guide, the eight suites.
- Grader 2026-10-30-1 (the 1.0 release source), the same one as v2.
- Each `merged_16bit` is a symlink to the original export, so these are the same weights as the T=0 results.
- Both exports have the same sampling defaults (`top_k` 20, `top_p` 0.95), the same stop tokens and the same chat
  template, and the server applies those defaults under the requested temperature.

## The reading, fixed before the results

**Measure.** Each task's first-attempt pass rate and after-repair pass rate, out of 5. Within main (73) and held-out
(53), the mean rate at epoch 3 minus the mean rate at epoch 2, with a 95% interval from a paired bootstrap over tasks
(10,000 resamples, seed 42). Repeats of one task are not independent, so the task is the unit. Wrong-output rates are
reported the same way.

**The question** is the held-out first-attempt difference, the one the recipe rests on:

| 95% interval | Reading |
|---|---|
| above 0 | The gain is real for these weights. It still takes another seed to show it comes from the recipe. |
| includes 0 | The T=0 gain of 6 tasks is not supported. The 3-epoch recipe has no measured basis over 2 epochs. |
| below 0 | The third pass hurt, and the T=0 gain was luck. |

Main and the after-repair differences are reported beside it, and they do not change the reading.

**Limits.**
- T=0.2 measures how robust the weights are, which is not the T=0 behaviour the other results report.
- These are two adapters from one training run, so this says nothing about seed noise.
- Held-out helped choose e3 for release, so it should not pick between adapters here. This test is about the recipe,
  and e3 stays the release model whichever way it comes out.

## Log

- 2026-11-02 04:28Z: scoring started on host-c, e3 first and then its epoch 2 adapter. The owner had the host's image
  and video generation services and its resident llama-server stopped again. They are restarted by hand after the run.
- 2026-11-02 10:36Z: scoring finished, with no infra failures. The held-out interval includes 0; see `RESULTS.md`.
  The host stayed down for the Gemma 4 12B run that follows, with the owner's OK.
