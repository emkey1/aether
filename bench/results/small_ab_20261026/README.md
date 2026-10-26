# Small-guide A/B: the current small guide against a dense rewrite (2026-10-26)

One board, both guides in each report and interleaved per task, so every task is
answered under both guides by the same model with the same seed. The question was
the D37d one: is the rewrite non-inferior on first attempt (95% lower bound of the
paired difference above -3 points)? **It is not: first attempt fell from 69.9% to
35.6%.** The rewrite was not adopted; the small guide stays at 2026-10-26-1.

| | |
|---|---|
| compiler | aether `849cad0`, language 2026-10-26-1 (label 1.0-beta15dev), Release, sha256 `b29d276e…1b016f` |
| current guide (`small_current`) | `aether_for_llms_with_small_contexts.md` 2026-10-26-1 as of aether `849cad0`, sha256 `5b286812…985254`; 11,849 o200k / 12,614 Qwen3.5 tokens |
| dense rewrite (`small_dense`) | stamp 2026-10-26-2, a local draft that was never pushed; the file as run is `small_dense_2026-10-26-2.md.gz` here, sha256 (uncompressed) `1ed1196f…c6a4`; 4,106 o200k / 4,423 Qwen3.5 tokens, under the D11 ceiling of 8,000 |
| model | qwen3.5-9b on OpenRouter, provider pinned (Parasail, bf16), destination `or-qwen35-9b` of `../b0_openrouter_20261010/destinations.openrouter_20261010.json`: T 0.2, output cap 96,000, stop null |
| tasks | the current manifests `bench/tasks_v2_pos.json` (35), `tasks_cs.json` (19), `tasks_hard_v2.json` (14), `tasks_hard_nontoon.json` (5): 73 tasks. Not the frozen B0 copies, which predate the snake_case surface |
| settings | 3 repeats (seed 42 + r), 2 repair rounds, the no-retry rule (an empty first attempt is final); one report per suite and repeat |
| run | host-e, 2026-10-26 09:11Z to 17:46Z, three repeat lanes of 6 workers. A second pass resumed 21 cases that ended in a provider or transport failure on the first pass. Every unit holds every case for both guides, none unmeasured (`status.log`) |
| cost | $5.40 of key usage, including a 4-case smoke and the probe below |

`analyze_small_ab.py --harness tools/aether_doc_bench.py bench/results/small_ab_20261026`
reproduces every number below from the packed reports.

## Results

219 paired cases (73 tasks x 3 repeats). FA = first attempt; FX = after repair
under the no-retry rule. Intervals are 95% percentile bootstrap of the paired
difference, resampling task clusters within each suite (4,000 draws).

| | current | dense | dense - current |
|---|---|---|---|
| FA | 153 (69.9%) | 78 (35.6%) | -34.2 pts [-44.3, -23.7] |
| FX | 173 (79.0%) | 79 (36.1%) | -42.9 pts [-52.5, -32.9] |
| no answer within the output budget | 35 | **117** | |
| silent wrong / coded / uncoded error | 8 / 22 / 1 | 6 / 18 / 0 | |
| FA among first attempts that held a program | 153 / 184 (83.2%) | 78 / 102 (76.5%) | |
| completion tokens (all attempts) | 4.92 M | **15.52 M** | x 3.2 |
| prompt tokens (all attempts) | 3.46 M | 1.27 M | x 0.37 |

Case by case, the first attempt differs in 103 pairs: 89 pass only under the
current guide, 14 only under the dense one (two-sided sign test p < 0.001).

| suite | cases | FA current | FA dense | FX current | FX dense |
|---|---|---|---|---|---|
| tasks_v2_pos | 105 | 93 | 57 | 97 | 57 |
| tasks_cs | 57 | 38 | 16 | 42 | 17 |
| tasks_hard_v2 | 42 | 20 | 3 | 30 | 3 |
| tasks_hard_nontoon | 15 | 2 | 2 | 4 | 2 |

FA by repeat: current 74.0 / 65.8 / 69.9, dense 35.6 / 37.0 / 34.2. Twelve tasks
pass 3 of 3 under the current guide and 0 of 3 under the dense one, `hello_fx`,
`clamp_scores` and `nested_multiply_grid` among them; eight tasks move the other
way by one or two repeats (`cs_dijkstra`, `count_by_category`, `tuple_order_post`
0 -> 2 of 3).

The dense guide saved 2.2 M prompt tokens over the board and spent 10.6 M more
completion tokens, so it also cost more to run: about $4.01 against $1.58 at the
provider's list prices.

## Why: the model does not stop reasoning

The loss is almost all `no_answer`: the model was still reasoning when it reached
the 96,000-token output cap. When it did answer, it did a little worse than under
the current guide (76.5% against 83.2%), not much worse.

Both guides open with a list of rules keyed by diagnostic code. What the rewrite
changed, all in one step:

- the rules are terse imperatives with no code formatting, so code and prose run
  together ("or end the file with main();.");
- one list replaces the current guide's rules, repair rules and validation
  checklist;
- it shows 4 complete programs where the current guide shows 7;
- its ENTRY-001 line gained an alternative from the compiler's hint, "move them
  into fn main() -> Void, or end the file with main();". The current guide says
  only "put them in `fn main() -> Void`".

`probe/dense_hello_fx.json.gz` is one extra request: `hello_fx` (print one line)
under the dense guide, same model, provider and seed, output capped at 16,000,
with the reasoning returned. It ends at the cap with no answer and 53,834
characters of reasoning:

- The correct three-line program is written out at character 2,072.
- The model then walks the rules as a checklist ("Wait, check `FX-001` ...
  `ENTRY-001` ... `OUT-001` ..."), 49 times for OUT-001 alone.
- It never settles the ENTRY-001 alternative: should a file that already has
  `fn main` also end with `main();`? It quotes "end the file with main();" 89
  times.
- From character 14,564 one block repeats verbatim to the end.

The rewrite followed the instruction that the guide need not be human-readable
if a model can parse it. In the probe the model reads each rule correctly; it
just cannot stop re-checking them, and the either/or line is where it stalls.
The current guide answers `hello_fx` 3 of 3 on the board.

## Limits

- One model, the only one the small board runs (qwen3.5-9b, the 16K cohort).
  Nothing here says how a stronger model or a non-reasoning model reads the dense
  guide.
- The rewrite was one step (11,849 -> 4,106 o200k). D11 asks for the cut to 8,000
  in A/B-validated steps, so this board cannot say which change did the damage.
  The probe points at the ENTRY-001 either/or line and the terse rules list; it is
  one request, an illustration rather than a measurement.
- Not comparable with B0-small (`../b0_openrouter_20261010/`, small guide
  2026-09-05-1: FA 61.6%, 57 empty first attempts of 219): guide, compiler, task
  manifests and harness all differ.
