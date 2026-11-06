---
license: apache-2.0
base_model: google/gemma-4-12B-it
language:
  - en
tags:
  - code
  - aether
  - gemma4
  - lora
  - fine-tuned
library_name: peft
pipeline_tag: text-generation
---

# Gemma-4-12B-Aether

> **Repositories:** 8-bit for Ollama [emkey1/Gemma-4-12B-Aether-int8](https://huggingface.co/emkey1/Gemma-4-12B-Aether-int8) · adapter [emkey1/Gemma-4-12B-Aether-LoRA](https://huggingface.co/emkey1/Gemma-4-12B-Aether-LoRA) · merged [emkey1/Gemma-4-12B-Aether](https://huggingface.co/emkey1/Gemma-4-12B-Aether) · larger siblings [emkey1/Qwen3.6-35B-A3B-Aether](https://huggingface.co/emkey1/Qwen3.6-35B-A3B-Aether), [emkey1/Qwen3.8-27B-Aether](https://huggingface.co/emkey1/Qwen3.8-27B-Aether)

This repository: the **LoRA adapter**.

A LoRA fine-tune of [google/gemma-4-12B-it](https://huggingface.co/google/gemma-4-12B-it) that writes **Aether** programs
with no language guide in the prompt. At 8 bits it is a 12 GB model, the smallest in this family, and [ready to download](https://huggingface.co/emkey1/Gemma-4-12B-Aether-int8).

[Aether](https://github.com/emkey1/aether) is a small, statically typed language with explicit effects
(`fx { }` blocks), contracts (`@pre` / `@post`), records with methods, arrays and 2-D arrays, maps
(`Map[Text, T]`), and a built-in JSON/TOON API. It is new, so general models do not know it. With a ~14K-token guide
in every prompt, a strong base model does well; this fine-tune puts that knowledge into the weights.

**Pick this model when the larger ones do not fit.** On the same Mac, the 35B mixture-of-experts sibling at 4 bits (20.9 GB)
scores about the same on the 126 benchmark tasks, but fails fewer first attempts on 1,776 fresh tasks (230 against 322 for this
model at 8 bits), and it generates 4 to 6 times faster. At full size it is clearly better on the held-out suites. It also
used more memory over a scoring pass, 29 GB against this model's 21 GB at 8 bits.

**This is an Aether specialist.** The fine-tuned model writes much worse Python, and probably worse code in other
languages; see below. For anything other than Aether, use the base model.

## Results

All scores use no guide in the prompt, temperature 0, seed 42, one sample per task, and up to two repair rounds in
which the model sees the compiler's diagnostic and the observed output.
- "First attempt" counts tasks solved by the first program; "after repair" counts tasks solved within the repairs.
- Graded with the Aether 1.0 release compiler (language version 2026-10-30-1).
- Columns: first attempt (after repair).
- One training seed of two scored. A second seed of the same recipe, not published, differs on 21 of 126 first attempts
  and scores 96 (116) at 16 bits and 96 (114) at int8, so a difference of a few tasks between two rows means little.

| suite | tasks | untrained base, no guide | **this model, bf16** | **this model, int8** | untrained base + 14K-token guide |
|---|---:|---:|---:|---:|---:|
| core | 35 | 0 (3) | **33 (34)** | **33 (34)** | 29 (35) |
| hard: TOON data processing | 14 | 0 (0) | **13 (13)** | **12 (14)** | 3 (11) |
| hard: other | 5 | 0 (0) | **3 (4)** | **2 (4)** | 2 (4) |
| algorithms | 19 | 0 (1) | **18 (18)** | **18 (18)** | 14 (17) |
| **main total** | **73** | **0 (4)** | **67 (69), 92%** | **65 (70), 89%** | **48 (67)** |
| held-out: surface | 15 | 0 (0) | **10 (13)** | **10 (13)** | 10 (13) |
| held-out: algorithmic | 14 | 0 (0) | **8 (10)** | **9 (11)** | 6 (11) |
| held-out: specification | 12 | 0 (0) | **6 (6)** | **5 (6)** | 6 (8) |
| held-out: TOON / memory streams | 12 | 0 (0) | **6 (7)** | **4 (6)** | 4 (9) |
| **held-out total** | **53** | **0 (0)** | **30 (36), 57%** | **28 (36), 53%** | **26 (41)** |

- **What the training buys is first attempts on the main suites without a 14K-token prompt.** At int8, it passes 20 of the 23
  main tasks where it and the guided base differ on first attempts. On the held-out suites, and after repair, the two are level.
- **Held-out suites:** these were never used to write training data or to mine failures. They measure how far the
  gains carry beyond the tasks the data was developed against.
- **Eight bits cost little:** only 8 of 126 first attempts differ from bf16 (2 gained, 6 lost). The two forms also differ in serving
  stack, so this cannot split the two effects.
- **Four bits cost much more, so do not go below 8 bits:** Ollama's nvfp4 (8.0 GB) scores 58 (65) on main and 21 (26) on held-out, and
  its mxfp4 (7.7 GB) 55 (59) and 22 (28), even though both keep 41 of the 329 quantized tensors at 8 bits, about a third of the file. Against
  int8 they take 5 tasks to 19 and 6 tasks to 22 on first attempts. NVFP4 with 4-bit activations on vLLM (8.25 GB) scored 53 (60) and
  16 (23), at about 3 times the 16-bit speed on one GPU host.
- **Failures:** on first attempts at bf16, 14 are compiler diagnostics, 7 wrong return codes and 8 wrong output. Wrong output
  gives a repair round no diagnostic to work from, so use the compiler and your own tests.
- The base's columns: no guide at int8, with the guide at Ollama's `q8_0`.

### Against the larger siblings

First attempt (after repair), no guide. Sizes are on disk.

| | size | main (73) | held-out (53) | generating, same Mac |
|---|---:|---|---|---:|
| **this model, int8** | **12 GB** | **65 (70)** | **28 (36)** | **20 tok/s** |
| Qwen3.6-35B-A3B-Aether, mxfp4 | 20.9 GB | 64 (68) | 33 (40) | 85 tok/s |
| Qwen3.6-35B-A3B-Aether, int8 | 39.1 GB | 65 (70) | 36 (44) | not measured |
| Qwen3.6-35B-A3B-Aether, bf16 | 67 GB | 65 (70) | 39 (47) | not measured |

- A sign test cannot separate this model from the mxfp4 sibling: over all 126 tasks, 12 first attempts to 16 and 10 repaired
  tasks to 12. The bf16 sibling is clearly ahead on held-out after repair, 14 tasks to 3.
- The speed gap comes from the architecture: this dense model reads all its weights for each token, and the sibling about a
  quarter of them. The likely cause; not isolated.
- **Speed:** the two readings were taken back to back on a busy Mac, so use the ratio, not the numbers. An earlier, separate pair
  read 38.8 and 217 tok/s, 5.6 times.
- **Memory:** Ollama reported 15 GB loaded and 21 GB by the end of a full scoring pass, at a 32K context, on a larger Mac. The mxfp4
  sibling reached 29 GB. Neither has been run on a 32 GB machine.

### Choosing the checkpoint

The last step of three epochs is released. It beat the end of epoch 2 on all four numbers (main 67 against 64, held-out 30
against 27, after repair 69 against 68 and 36 against 31), so the main suites alone give the same answer. The gap is not
significant (16 tasks to 10), and eval loss rose over the third pass while the scores went up.

### Python, same model, same tasks

Both columns were measured at int8 on the same machine, with the same harness and repair rounds.

| suite | tasks | untrained base | **this model** |
|---|---:|---:|---:|
| core | 35 | 28 (35) | 13 (20) |
| hard: TOON data processing | 14 | 10 (13) | 1 (1) |
| hard: other | 5 | 5 (5) | 2 (4) |
| algorithms | 19 | 19 (19) | 18 (19) |
| held-out: surface | 15 | 14 (15) | 5 (11) |
| held-out: algorithmic | 14 | 13 (14) | 12 (14) |
| held-out: specification | 12 | 10 (12) | 4 (5) |
| held-out: TOON / memory streams | 12 | 10 (12) | 3 (5) |
| **total** | **126** | **109 (125)** | **58 (79)** |

Many failed first attempts carry Aether into Python: `for i in 0..5:`, or `has_toon()` and `toon_parse()`, which Python does not
have. This is the forgetting that comes with specialising a model on one language, and Aether looks a lot like Python.

## How to use

The model was trained with thinking **off** and with **no system message**. Serve it the same way:

- **Ollama** (OpenAI-compatible `/v1`): send `"reasoning_effort": "none"`. On native `/api/chat`, send `"think": false`; with
  neither, thinking turns on and the model has not been trained for that.
  - The 12 GB model tested for this card is [emkey1/Gemma-4-12B-Aether-int8](https://huggingface.co/emkey1/Gemma-4-12B-Aether-int8), so there is nothing to quantize. It was built by
    importing the merged safetensors with `ollama create -q int8`.
  - Set the context in a Modelfile (`PARAMETER num_ctx 32768`); `/v1` ignores per-request context options. The int8 repository's
    Modelfile already does.
- **vLLM**: pass `chat_template_kwargs={"enable_thinking": false}` with each request.
  - This is how the bf16 scores above were measured, on vLLM 0.25.1 with `--max-model-len 32768`.
- **Prompt:** with thinking off, the template ends the prompt in an empty thought channel,
  `<|turn>model\n<|channel>thought\n<channel|>`. The model trained on exactly that. A server that renders anything else will
  not get these results.

### Loading the adapter

It needs a Transformers with `gemma4_unified` (trained on 5.17.0, with PEFT 0.19.1). A bf16 load of the base needs about 24 GB of
accelerator memory:

```python
from transformers import AutoModelForCausalLM, AutoTokenizer
from peft import PeftModel

base = AutoModelForCausalLM.from_pretrained("google/gemma-4-12B-it", dtype="bfloat16", device_map="auto")
model = PeftModel.from_pretrained(base, "emkey1/Gemma-4-12B-Aether-LoRA")
tok = AutoTokenizer.from_pretrained("emkey1/Gemma-4-12B-Aether-LoRA")
```

The scored model is the merged one, and the adapter has not been scored on its own. For serving, the merged repository is the
simpler path.

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
that form. Keep the Aether compiler in the loop.

## Training

- **Method**: LoRA r=32, alpha=64, dropout 0, on the text layers only: the seven attention and MLP projection names, 328 modules,
  131M trainable parameters, about 1% of the model. Vision and audio are untouched.
- **Loading.** The base model was loaded in Unsloth's 4-bit mode with `FastModel`. Training text is rendered exactly as served,
  keeping the template's one BOS and the empty thought channel.
- **Optimisation.**
  - lr 1e-4, linear decay over 3 epochs; batch 1 × 8 gradient accumulation; seed 3407.
  - Loss on responses only (prompt tokens masked). No system message on any record.
  - **No early stopping**: the last step of epoch 3 is released (see "Choosing the checkpoint").
  - Unsloth + TRL + PEFT on one GPU host, about 3 hours.
- **Data**: 2,278 instruction records and 87 repair records on Aether 1.0, 60 of them held for evaluation. Every Aether program
  compiles and prints its stated output with the compiler, checked on every build. The records are:
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
[emkey1/aether](https://github.com/emkey1/aether) (`bench/`, `training/`; this model is run `gemma4-12b-v2`).

## Limitations

- **Aether only.** Python, and probably other languages, are badly degraded; see above.
- **Language version.** The model knows Aether 1.0, whose language is frozen: valid programs keep compiling with the same output.
- **Program size.** It writes small, self-contained programs well. It has not been evaluated on multi-file projects
  or long programs.
- **Wrong output.** More of its failures compile and print the wrong thing than the larger siblings', so a clean compile is not
  proof the program is right.
- **Speed.** It is a dense 12B model, so it is slower per token than its size suggests.

## License and attribution

Apache-2.0, as the base model. Distilled training data was generated with MiMo-V2.6-Flash-RL (MIT, Xiaomi).
