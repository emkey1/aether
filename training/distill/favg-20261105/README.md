# favg-20261105: averaging two seeds' weights, read on 1,776 fresh tasks

Two seeds of `gemma4-12b-v2` disagree on many tasks (`../fs42-20261105/`). Choosing one throws the other away, and running both costs twice the memory. This batch tests the third way, which adds nothing at inference: average the two merged 16-bit exports tensor by tensor, build int8 the same way as each parent, and let it answer the same 1,776 frozen tasks with no guide and no repair, on the 1.0 release binary. No training is involved. Neither parent trained on these tasks.

The published work is mixed. [Model soups](https://proceedings.mlr.press/v162/wortsman22a.html) gained by averaging fine-tuned models that differed in hyperparameters. [AdapterSoup](https://ar5iv.labs.arxiv.org/html/2302.07027) reports, without numbers, that averaging bottleneck adapters from different random seeds performed poorly. Averaging merged weights averages each LoRA's whole change to the base, which should avoid the mismatch between two LoRAs' factor matrices, but that has not been tested here.

Nothing here is solved, harvested or trained on, and the programs are only read for pass or fail. `average_weights.py` builds the export, and `overlap.py` prints the numbers from the repo root.

## The reading, fixed before the pass

The comparison is against seed 42, the stronger parent on these tasks: 272 failures against the published seed's 322. The measure is tasks only one of the two fails, with an exact two-sided sign test.

| result | reading | what follows |
|---|---|---|
| fewer failures than seed 42, p < 0.05 | **better than both parents** | a candidate to publish, which is the owner's decision; the eight suites at int8 come first, on the owner's word |
| p >= 0.05 | **no better than seed 42** | averaging is not worth the extra step here, and seed 42 stays the alternative to the published form |
| more failures than seed 42, p < 0.05 | **worse than seed 42** | as above, and averaging these two seeds costs something |

**Stop early if it is broken.** After the first 246 tasks, the pass stops if the average has at least twice the failures of the worse parent on those tasks, which would save about 7 hours. `early_check.py` applies this, and a stopped pass reads as worse than seed 42.

**Reported, not deciding:**
- Of the 250 tasks the parents disagree on, how many the average passes. About half is what picking one parent at random would give, and more suggests it keeps the better of each.
- Of the 172 tasks both parents fail, how many it rescues, and of the 1,354 both pass, how many it breaks.
- Each failure class, area and category, and the comparison with the published seed and e3.
- From `average_stats.json`: how many tensors differ between the parents, and the cosine between each parent's change to the base.

## Limits, stated now

- **First attempts only,** at int8, with these batches' task mix. Repair is not tested.
- **One pair of seeds.** A win here says nothing certain about other seeds or about the MoE, which would need a second seed trained first, about 13 hours.
- **Seed 42 was shown stronger on these same tasks,** so its score may be slightly flattering, which makes this comparison a little harder to win.
