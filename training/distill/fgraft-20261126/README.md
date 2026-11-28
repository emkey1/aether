# fgraft-20261126: whole-layer grafts of the two v3 seeds (written 2026-11-26, before any graft was evaluated)

Averaging (`../favg-20261105/`) and TIES (`../fties-20261106/`) of two seeds of one recipe both lost knowledge: each
weakened every layer of both runs. A graft keeps each layer exactly as one parent trained it. The owner asked whether
whole-layer replacement could gain the best of both seeds ("Yes, screen them", 2026-11-26).

**What is known before the screen.** On fv3's 1,026 tasks in the c1 session, `gemma4-12b-v3` (seed 3407) and
`gemma4-12b-v3-s42` pass 886 and 879 first attempts and differ on 125 tasks (66 / 59); the same weights re-answered 20 days
apart differ on 12 and 36. Per layer, the cosine between the two seeds' changes to the base is 0.000 in layers 0 to 15,
rises from layer 16, and is 0.18 to 0.45 in layers 28 to 47. [Layer swapping](https://arxiv.org/abs/2410.01335) gained
by grafting two fine-tunes of one base that learned different skills living in different layers; these two seeds learned
the same skill.

## Candidates

Each is the base `gemma-4-12B-it` with every one of the 328 LoRA-target tensors (the only tensors any run changes) taken
whole from one source, chosen by layer. No training, no averaging.

| name | layers 0..k-1 | layers k..47 | why |
|---|---|---|---|
| `v3`, `v3_s42` | the parent | the parent | the parents, loaded the same way (check) |
| `g24_a`, `g24_b` | v3 / v3_s42 | v3_s42 / v3 | grafts at the middle, both ways |
| `g16_a`, `g16_b` | v3 / v3_s42 | v3_s42 / v3 | grafts at the end of the zone where the seeds' changes do not overlap |
| `late_v3` | base (k = 20) | v3 | probe: does v3 need its early-layer changes? |
| `early_v3` | v3 (k = 20) | base | probe: the reverse |
| `base` | base | base | scale |

## The screen, fixed now

- **Data:** the 298 control pairs of `../c1-20261118/` (the records of `v3c-20261118` that are not in `v3-20261104`;
  none of their prompts is in v3's data), so the set is held out of both parents. The trainer's own eval split cannot
  serve: it is drawn with each run's seed, so each parent's split is partly the other's training data.
- **Loss:** as the trainer computes it: the family's rendering as served (thinking off), loss on the response tokens only,
  bf16, mean over records of each record's mean token loss. The token-weighted mean is reported too.
- **Rule:** a graft goes on to scoring only if its mean loss is at or below the higher of the two parents'. The probes
  are reported, not screened. Eval loss only catches breakage: runs at the same loss differ on 125 tasks, so passing the
  screen says nothing about a gain.

## The reading, if a graft is scored (each scoring pass needs the owner's go-ahead)

A surviving graft is imported at int8 and answers fv3's 1,026 tasks on the scoring Mac exactly as the c1 session's
passes did (no guide, no repair, the 1.0 release grader), against that session's parent reports `filter_v3_c1s` and
`filter_v3_s42_c1s`. If the Ollama version is no longer the session's 0.40.17, the better parent is answered again first.

| sign test on first-attempt pass/fail, graft against the better parent | reading |
|---|---|
| more tasks gained than lost, two-sided p < 0.05 | **a graft beats both seeds** |
| fewer gained than lost, p < 0.05, against the worse parent | **worse than both** |
| otherwise | **no gain** |

## Screen result (2026-11-26)

On a training host in the family's trainer image (`graft_eval.py`; the 328 targets were mapped and checked against
the base before any copy), 298 records, 96,013 response tokens. `losses.json` holds every record's loss,
`summary.txt` the paired differences.

| candidate | mean loss | against the better parent (v3_s42), 95% interval | screen |
|---|---|---|---|
| `v3` | 0.0430 | +0.0008 [-0.0005, +0.0021] | parent |
| `v3_s42` | 0.0423 | | parent |
| `g24_b` (v3_s42 0-23, v3 24-47) | 0.0422 | -0.0001 [-0.0010, +0.0008] | passes |
| `g16_b` (v3_s42 0-15, v3 16-47) | 0.0428 | +0.0005 [-0.0007, +0.0017] | passes |
| `g16_a` (v3 0-15, v3_s42 16-47) | 0.0448 | +0.0025 [+0.0014, +0.0037] | fails |
| `g24_a` (v3 0-23, v3_s42 24-47) | 0.0451 | +0.0028 [+0.0016, +0.0041] | fails |
| `late_v3` (base 0-19, v3 20-47) | 0.0456 | +0.0033 against v3_s42, +0.0026 [+0.0010, +0.0040] against v3 | reported |
| `early_v3` (v3 0-19, base 20-47) | 0.7878 | | reported |
| `base` | 0.7696 | | reported |

- No graft breaks the model, unlike the average and TIES: every graft is within 7% of the parents' loss. Two pass
  the screen and are level with the better parent; neither is lower on more records than chance (147 and 146 of 298).
- The grafts are not symmetric: v3_s42's early layers under v3's late layers match the parents, while v3's early
  layers under v3_s42's late layers are about 6% worse.
- **The probes:** v3's changes to layers 20 to 47 alone keep almost all of its gain (0.0456 against v3's 0.0430 and
  the base's 0.7696, about 99.6% of the drop). Its changes to layers 0 to 19 alone do nothing: 0.7878, slightly worse
  than the base. On this loss, the early-layer changes, the part where the seeds do not overlap, are almost idle.

Eval loss is not pass rate, so none of this shows a gain. Scoring a surviving graft is the owner's call.

## Owner decision (2026-11-28)

No surviving graft is scored: on eval loss neither is lower than the better parent on more records than chance
(`docs/aether_decisions.md`, D97).
