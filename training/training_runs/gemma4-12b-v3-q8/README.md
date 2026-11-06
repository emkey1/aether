# gemma4-12b-v3 at 8 bits, for the model cards (set up 2026-11-06)

The owner chose `../gemma4-12b-v3/` (seed 3407, trained with the failure-targeted pairs) to replace the published Gemma 4 12B fine-tune, after it beat both v2 seeds on 1,026 fresh tasks (`../../distill/fv3-20261105/`). Its int8 form has not been scored on the benchmark, and the cards report that form, so this run measures what they need, on the same Mac and stack as `../gemma4-12b-v2-q8/`:
- `score.sh`: the eight suites, no guide, T=0, seed 42, two repair rounds, on the 1.0 release binary, as `../gemma4-12b-v2-s42-q8/` did.
- `card_numbers.sh`: generation speed against e3 at mxfp4 with nothing else loaded, then the fine-tune's Python on the same eight suites. The untrained base's numbers do not change.

The model is `aether-gemma4-12b-v3-q8-32k`, built from the merged export (SHA-256 starts `5c556ef2065b06a2`) with `ollama create -q int8`, the form the re-test scored. Its tensors are exported for publishing with `../gemma4-12b-v2-q8/export_mlx.py` (`model.safetensors` starts `cca2033cebaabf58`).
