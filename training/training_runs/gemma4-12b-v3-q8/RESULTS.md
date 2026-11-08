# gemma4-12b-v3-q8: results, and the published update (2026-11-08)

`gemma4-12b-v3` at int8 on the eight suites, no guide, T=0, two repair rounds, on the 1.0 release binary, with Python and speed for the model cards.

| int8, first attempt (after repair) | main (73) | held-out (53) | all (126) | failures repair fixed |
|---|---|---|---|---|
| **v3, seed 3407** | **63 (68)** | **32 (34)** | **95 (102)** | **7 of 31** |
| v2, seed 3407 (the first release) | 65 (70) | 28 (36) | 93 (106) | 13 of 33 |
| v2, seed 42 | 65 (72) | 31 (42) | 96 (114) | 18 of 30 |
| e3, mxfp4 | 64 (68) | 33 (40) | 97 (108) | 11 of 29 |

- **Against the first release** sign tests separate nothing: 13 to 11 on first attempts, 9 to 13 after repair.
- **Against seed 42 after repair** it is behind, 2 to 14 (p=0.004). On 1,026 fresh tasks with repair it is ahead instead, 51 to 33 (p=0.063), so the benchmark's gap did not repeat (`../../distill/fv3-20261105/`).
- **Python:** 68 (94), against the first release's 58 (79).
- **Speed:** 39.4 tok/s, against 219.7 for e3 at mxfp4, back to back with nothing else loaded.
- **Export:** `export_mlx.py compare` against `aether-gemma4-12b-v3-q8-32k` finds 0 of 677 tensors differing and the same Ollama config.

## Published (2026-11-08)

On the owner's word, these weights replace the first release in all three repositories, each through a pull request checked by hash before it was merged and read back from `main` with no token afterwards. The cards are `training/config/hf_release/README.gemma.{int8,merged,lora}.md`.

| repo | new revision | weights SHA-256 starts | the first release stays at |
|---|---|---|---|
| `emkey1/Gemma-4-12B-Aether-int8` | `5b3f3b3e3bba` | `cca2033cebaabf58` | `ed0c2c31544c` |
| `emkey1/Gemma-4-12B-Aether` | `79eac68410c8` | `5c556ef2065b06a2` | `733fa66b5713` |
| `emkey1/Gemma-4-12B-Aether-LoRA` | `fbcc59a33777` | `2dee012e278fda88` | `0d59ea9295fd` |

A pull request with seed 42's v2 weights was opened first on each repository and closed unmerged when the owner chose v3 after the fresh-task re-test.
