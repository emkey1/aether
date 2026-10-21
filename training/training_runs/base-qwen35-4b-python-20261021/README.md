# Untrained Qwen3.5-4B, Python check (2026-10-21)

The untrained Qwen3.5-4B (bf16, Ollama on the scoring Mac) writing Python for all eight suites, with the same
settings as every fine-tune's Python check: T=0, seed 42, one repeat, two repair rounds, thinking off.

First attempt 65 of 126, after repair 80. `toon_type_probe` (tasks_v2_pos) and `mstream_field_parse`
(tasks_frontier) return only the end marker at T=0 on every run; the harness files those as infrastructure failures
and withholds the two suites' headlines, and they count as misses here.
