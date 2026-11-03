# Untrained Gemma 4 12B baselines (2026-11-03)

The untrained `google/gemma-4-12B-it` at revision `43c9d014` as an int8 Ollama import on the scoring Mac (`ollama create -q int8`,
the same import as the fine-tune's), graded by the 1.0 release binary. T=0, seed 42, one repeat, two repair rounds, thinking off.
`../gemma4-12b-v2-q8/card_numbers.sh` ran them, for the model card.

| | first attempt (after repair) |
|---|---|
| `aether__*`: no guide, main (73) / held-out (53) | 0 (4) / 0 (0) |
| `python__*`: Python, all eight suites (126) | 109 (125) |

The base's guided numbers are in `../guided-gemma4-12b-1.0-20261031/`, from Ollama's own `q8_0` build rather than this import.
The fine-tune's Python, from the same pass, is `../gemma4-12b-v2-q8/score/python__*`: 58 (79).
