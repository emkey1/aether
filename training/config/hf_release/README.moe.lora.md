---
license: apache-2.0
base_model: Qwen/Qwen3.6-35B-A3B
language:
  - en
tags:
  - code
  - aether
  - lora
  - moe
  - fine-tuned
library_name: peft
pipeline_tag: text-generation
---

# Qwen3.6-35B-A3B-Aether

> **Repositories:** adapter [emkey1/Qwen3.6-35B-A3B-Aether-LoRA](https://huggingface.co/emkey1/Qwen3.6-35B-A3B-Aether-LoRA) · merged [emkey1/Qwen3.6-35B-A3B-Aether](https://huggingface.co/emkey1/Qwen3.6-35B-A3B-Aether) · dense sibling [emkey1/Qwen3.8-27B-Aether](https://huggingface.co/emkey1/Qwen3.8-27B-Aether)

This repository: the **LoRA adapter**.

A LoRA fine-tune of [Qwen/Qwen3.6-35B-A3B](https://huggingface.co/Qwen/Qwen3.6-35B-A3B) that writes **Aether**
programs with no language guide in the prompt. The base is a mixture-of-experts model: 35B parameters, of which about
3B are active per token (256 routed experts, 8 per token, plus a shared expert). So it runs at roughly the speed of a
small dense model, with the knowledge of a larger one.

[Aether](https://github.com/emkey1/aether) is a small, statically typed language with explicit effects
(`fx { }` blocks), contracts (`@pre` / `@post`), records with methods, arrays and 2-D arrays, maps
(`Map[Text, T]`), and a built-in JSON/TOON API. It is new, so general models do not know it. With a ~14K-token guide
in every prompt, a strong base model does well; this fine-tune puts that knowledge into the weights.

It is trained on **Aether 1.0-beta** (language 2026-10-20-12) with the same data and recipe as the dense
[Qwen3.8-27B-Aether](https://huggingface.co/emkey1/Qwen3.8-27B-Aether), except that it trains for three full epochs
(see "Choosing the checkpoint").

**This is an Aether specialist.** The fine-tuned model writes much worse Python, and probably worse code in other
languages: it tends to answer in Aether syntax (see below). For anything other than Aether, use the base model, or unload the adapter.

## Results

All scores use no guide in the prompt, temperature 0, seed 42, one sample per task, and up to two repair rounds in
which the model sees the compiler's diagnostic and the observed output.
- "First attempt" counts tasks solved by the first program; "after repair" counts tasks solved within the repairs.
- Graded with the Aether compiler at language version 2026-10-21-8 (`1.0-beta4dev`).
  - Every earlier model replayed on it scores exactly as on 2026-10-20-12.
- Columns: first attempt (after repair).

| suite | tasks | untrained base, no guide | **this model, no guide** | untrained base + 14K-token guide |
|---|---:|---:|---:|---:|
| core | 35 | 0 (0) | **33 (35)** | 33 (35) |
| hard: TOON data processing | 14 | 0 (0) | **13 (14)** | 13 (14) |
| hard: other | 5 | 0 (0) | **2 (4)** | 1 (3) |
| algorithms | 19 | 0 (0) | **17 (17)** | 13 (17) |
| **main total** | **73** | **0 (0)** | **65 (70), 89%** | **60 (69)** |
| held-out: surface | 15 | 0 (0) | **10 (14)** | 12 (14) |
| held-out: algorithmic | 14 | 0 (0) | **10 (11)** | 5 (8) |
| held-out: specification | 12 | 0 (0) | **9 (11)** | 5 (7) |
| held-out: TOON / memory streams | 12 | 0 (0) | **10 (11)** | 7 (9) |
| **held-out total** | **53** | **0 (0)** | **39 (47), 74%** | **29 (38)** |

- **With no guide, it beats the base model reading the 14K-token guide on both measures:** 65 against 60 on the main
  suites, 39 against 29 on the held-out suites.
- **Held-out suites:** these were never used to write training data or to mine failures. They measure how far the
  gains carry beyond the tasks the data was developed against.
  - They were used once to choose between two finished checkpoints (next section), so the held-out score carries a
    small selection bias.
- **Against the dense 27B sibling** (same data, graded the same way): it trails on the main suites (65 against 71) and
  matches it on the held-out suites (39 against 37, within run-to-run noise).
- The untrained-base columns were measured with an 8-bit (int8) copy of the base model. On a smoke test, int8 and
  bf16 scoring differed by one task.

### Choosing the checkpoint

Eval loss on a held-out slice of the training data was the wrong guide for this model.
- With early stopping on eval loss, training stopped at epoch 0.76 and scored 62 (main) and 30 (held-out).
- Training on for three full epochs raised eval loss (the model memorises its training set), yet it lifted the
  held-out score to 39.

Four finished models were compared, first attempt, main / held-out:

| | main (73) | held-out (53) |
|---|---|---|
| early-stopped (epoch 0.76) | 62 | 30 |
| epoch 2 of the 3-epoch run | 68 | 33 |
| a run scheduled for exactly 2 epochs | 67 | 33 |
| **epoch 3, this model** | **65** | **39** |

A rule fixed before the last run would have picked the 2-epoch run, on a two-task first-attempt gap on the main
suites; after repair the two tie at 70. This model was chosen instead for its held-out lead of six tasks, eight after
repair. That decision used the held-out suites, which is the reason for the selection-bias note above.

### Python, same model, same tasks

| suite | tasks | untrained base | **this model** |
|---|---:|---:|---:|
| core | 35 | 30 (32) | 6 (18) |
| hard: TOON data processing | 14 | 14 (14) | 5 (11) |
| hard: other | 5 | 5 (5) | 0 (2) |
| algorithms | 19 | 19 (19) | 17 (19) |
| held-out: surface | 15 | 15 (15) | 0 (6) |
| held-out: algorithmic | 14 | 14 (14) | 10 (12) |
| held-out: specification | 12 | 11 (12) | 6 (9) |
| held-out: TOON / memory streams | 12 | 9 (11) | 0 (5) |
| **total** | **126** | **117 (122)** | **44 (82)** |

- Most failed Python first attempts are Aether syntax in a Python answer (68 of 82): `fn main`, `let x: Int`, `fx {`.
- This is the forgetting that comes with specialising a model on one language, and Aether looks a lot like Python.
  Three epochs of Aether deepen it: the early-stopped run kept 78.
- Both columns were measured with int8 copies on the same machine.

## How to use

The model was trained with thinking **off** and with **no system message**. Serve it the same way:

- **vLLM**: pass `chat_template_kwargs={"enable_thinking": false}` with each request.
- **Ollama** (OpenAI-compatible `/v1`, merged model): send `"reasoning_effort": "none"`.
  - A top-level `think: false` is ignored on that endpoint, and the model will reason first.
  - Set the context in a Modelfile (`PARAMETER num_ctx 32768`); `/v1` ignores per-request context options.
  - Importing the merged safetensors with `ollama create -q int8` gives a 39 GB model, tested for this card.

### Loading the adapter

The adapter targets the fused expert weights through PEFT's parameter targeting, so it needs a recent PEFT and
Transformers (Qwen3.5-generation models need Transformers 5). A bf16 load of the base needs about 70 GB of
accelerator memory:

```python
from transformers import AutoModelForCausalLM, AutoTokenizer
from peft import PeftModel

base = AutoModelForCausalLM.from_pretrained("Qwen/Qwen3.6-35B-A3B", dtype="bfloat16", device_map="auto")
model = PeftModel.from_pretrained(base, "emkey1/Qwen3.6-35B-A3B-Aether-LoRA")
tok = AutoTokenizer.from_pretrained("emkey1/Qwen3.6-35B-A3B-Aether-LoRA")
```

We tested the adapter by loading it with Unsloth (`FastLanguageModel.from_pretrained(<adapter>, load_in_4bit=True)`)
and generating; the scored model is the merged one. For serving, the merged repository is the simpler path.

Prompt format used in training and evaluation (a single user message):

```
You are writing Aether code.

Do not assume you have an Aether reference guide for this task.
Use only what you already know about the language and infer cautiously.

Write exactly one complete Aether program that solves the task below.

Requirements:
- Return only raw Aether source code.
...
Task:
<task statement>

Expected stdout:
<exact expected output>

Aether source:
```

Shorter natural-language requests ("Write an Aether program that ...") also work; part of the training data is in
that form. Keep the Aether compiler in the loop: feeding its diagnostic back fixes most first-attempt misses.

## Training

- **Method**: LoRA r=32, alpha=64, dropout 0.
  - LoRA covers the 256 routed experts (their fused `gate_up_proj` / `down_proj` weights), the shared expert, and the
    projections of the 10 full-attention layers: 1.86B trainable parameters, 5% of the model.
  - The router is not trained.
  - The 30 linear-attention layers carry no LoRA.
- **Loading.** The base model was loaded in Unsloth's 4-bit mode. bitsandbytes cannot quantize the fused expert
  weights, so the experts, most of the model, stay in bf16; attention and the shared expert are 4-bit.
- **Optimisation.**
  - lr 1e-4, linear decay over 3 epochs; batch 1 × 8 gradient accumulation; seed 3407.
  - Loss on responses only (prompt tokens masked). No system message on any record.
  - **No early stopping**: the last step of epoch 3 is released (see "Choosing the checkpoint").
  - Unsloth + TRL + PEFT on one GPU host, about 11 hours.
- **Data**: the same 1,981 instruction records (1,761 Aether, 220 Python) and 87 repair records as the dense
  sibling, on Aether 1.0-beta. Every Aether program compiles and prints its stated output with the compiler, checked
  on every build. The records are:
  - a curated corpus of Aether programs and natural-language task/solution pairs;
  - repair drills: a broken program, the compiler's diagnostic, the fix;
  - targeted packs for the idioms earlier runs got wrong (file reads inside `fx`, JSON/TOON, memory streams,
    modules, tuple contracts, array value semantics). They were written from failure classes on the main suites,
    never from task text;
  - a standard-library pack covering every builtin family in the official list, and seed pairs for maps;
  - **distilled pairs**:
    - task statements and independent Python solutions written by
      [MiMo-V2.6-Flash-RL](https://huggingface.co/XiaomiMiMo/MiMo-V2.6-Flash-RL) (MIT);
    - Aether solutions written with the guide, by an untrained base model or, for the tasks it could not solve, by
      MiMo;
    - kept only where the Aether program prints exactly what the Python program prints.
  - Python pairs (MiMo's programs for the same tasks) to limit the drift away from Python.
- **Decontamination**:
  - Every training example is checked against every benchmark task: an identical expected output is dropped, and
    5-gram similarity to any reference solution must stay below 0.5.
  - The held-out suites are excluded from data authoring and from failure mining.

The language, its compiler, the guides, the benchmark tasks, the training data and every scoring report are at
[emkey1/aether](https://github.com/emkey1/aether) (`bench/`, `training/`; this model is run `qwen36-35b-a3b-e3`).

## Limitations

- **Aether only.** Python, and probably other languages, are badly degraded; see above.
- **Language version.** The model knows Aether 1.0-beta (2026-10-20). Aether 1.0 freezes the language: valid
  programs keep compiling with the same output, so later releases should add to what it knows rather than break it.
  Until the final 1.0, a break is still possible but needs an explicit, recorded decision.
- **Program size.** It writes small, self-contained programs well. It has not been evaluated on multi-file projects
  or long programs.
- **First attempts still fail on some idioms that one repair round fixes.** Use it with the compiler in the loop.

## License and attribution

Apache-2.0, as the base model. Distilled training data was generated with MiMo-V2.6-Flash-RL (MIT, Xiaomi).
