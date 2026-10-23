# Untrained Qwen3.6-35B-A3B baselines (2026-10-22)

The untrained model as an int8 Ollama import on the scoring Mac (`ollama create -q int8`, 39 GB; bf16 does not fit
beside the Mac's other resident model), graded on 2026-10-21-8. T=0, seed 42, one repeat, two repair rounds, thinking off.

| | first attempt (after repair) |
|---|---|
| `aether__*`: no guide, main (73) / held-out (53) | 0 (0) / 0 (0) |
| `guided__*`: the 1.0-beta2 medium guide (named `card` in the reports, ~13.5K tokens), main / held-out | 60 (69) / 29 (38) |
| `python__*`: Python, all eight suites (126) | 117 (122) |

A first guided attempt passed the guide document without selecting it, so it ran with no guide; it was caught by its
prompt size (333 tokens) and discarded. These reports are the rerun, every suite measuring about 13.5K prompt tokens.
