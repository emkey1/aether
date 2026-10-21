# Guided Qwen3.5-4B on Aether 1.0-beta2 (2026-10-21)

The untrained Qwen3.5-4B (bf16, Ollama on the scoring Mac) with the 1.0-beta2 medium guide in every prompt, on all
eight suites, graded on 2026-10-20-12. It is the guided comparison for `qwen35-4b-v1`.

- Guide: `docs/aether_for_llms_medium_contexts.md` at tag `1.0-beta2` (sha256 d8d22bb0b0418c78…), named `card` in
  the reports because the scoring script passes it as a custom document.
- Settings: T=0, seed 42, one repeat, two repair rounds, thinking off, 32K context.

| | first attempt (after repair) |
|---|---|
| main (73) | 46 (50) |
| held-out (53) | 12 (13) |

On the 2026-10-19-3 guide and grader it scored 52 and 13.
