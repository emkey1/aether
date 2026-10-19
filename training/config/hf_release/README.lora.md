---
license: apache-2.0
base_model: Qwen/Qwen3.8-27B
language:
  - en
tags:
  - code
  - aether
  - lora
  - fine-tuned
library_name: peft
pipeline_tag: text-generation
---

# Qwen3.8-27B-Aether

> **Repositories:** adapter [emkey1/Qwen3.8-27B-Aether-LoRA](https://huggingface.co/emkey1/Qwen3.8-27B-Aether-LoRA) · merged [emkey1/Qwen3.8-27B-Aether](https://huggingface.co/emkey1/Qwen3.8-27B-Aether)

This repository: the **LoRA adapter**.

A LoRA adapter for [Qwen/Qwen3.8-27B](https://huggingface.co/Qwen/Qwen3.8-27B) that writes **Aether**
programs with no language guide in the prompt. It ships in two forms:
- the **adapter** on its own, to load onto the base model when writing Aether;
- a **merged** model for convenience.

[Aether](https://github.com/emkey1/aether) is a small, statically typed language with explicit effects
(`fx { }` blocks), contracts (`@pre` / `@post`), records with methods, arrays and 2-D arrays, and a built-in
JSON/TOON API. It is new, so general models do not know it. With a ~14K-token guide in every prompt, a strong base
model does well; this adapter puts that knowledge into the weights.

**This is an Aether specialist.** With the adapter loaded the model also writes noticeably worse Python, and
probably worse code in other languages: it tends to answer in Aether syntax (see below). For anything other than
Aether, use the base model, or unload the adapter.

## Results

All scores use no guide in the prompt, temperature 0, seed 42, one sample per task, and up to two repair rounds in
which the model sees the compiler's diagnostic and the observed output.
- "First attempt" counts tasks solved by the first program; "after repair" counts tasks solved within the repairs.
- Every number below is graded with the Aether compiler at language version 2026-10-19-3.
- Columns: first attempt (after repair).

| suite | tasks | untrained base, no guide | **this model, no guide** | untrained base + 14K-token guide |
|---|---:|---:|---:|---:|
| core | 35 | 0 (2) | **33 (35)** | 34 (35) |
| hard: TOON data processing | 14 | 0 (0) | **13 (14)** | 14 (14) |
| hard: other | 5 | 0 (0) | **4 (5)** | 4 (5) |
| algorithms | 19 | 0 (1) | **18 (19)** | 15 (17) |
| **main total** | **73** | **0 (3)** | **68 (73), 93%** | **67 (71)** |
| held-out: surface | 15 | 0 (0) | **10 (13)** | 15 (15) |
| held-out: algorithmic | 14 | 0 (0) | **10 (13)** | 11 (13) |
| held-out: specification | 12 | 0 (0) | **12 (12)** | 11 (12) |
| held-out: TOON / memory streams | 12 | 0 (0) | **7 (7)** | 10 (11) |
| **held-out total** | **53** | **0 (0)** | **39 (45), 74%** | **47 (51)** |

- **Main suites:** with no guide, the model matches the base model reading a 14K-token guide on the first
  attempt, and solves every task after repair.
- **Held-out suites:** these were never used to choose or write training data, so they measure how far the
  gains carry beyond the tasks the training data was developed against. There the guided base is still ahead.

**Run-to-run variation:** two more training runs of the same recipe with other seeds scored main 69 (72) and
68 (73), and held-out 40 (47) and 40 (47). Across three seeds the first-attempt spread is 68–69 of 73 and 39–40 of
53. The released adapter is the first run, chosen before the other seeds were trained.

### Python, same model, same tasks

| suite | tasks | untrained base | with the Aether adapter |
|---|---:|---:|---:|
| core | 35 | 29 (33) | 16 (30) |
| hard: TOON data processing | 14 | 10 (13) | 11 (14) |
| hard: other | 5 | 5 (5) | 3 (5) |
| algorithms | 19 | 19 (19) | 18 (19) |
| held-out: surface | 15 | 15 (15) | 3 (12) |
| held-out: algorithmic | 14 | 13 (14) | 11 (13) |
| held-out: specification | 12 | 12 (12) | 12 (12) |
| held-out: TOON / memory streams | 12 | 11 (12) | 11 (11) |

Almost every Python first attempt the adapter gets wrong is Aether syntax in a Python answer (`fn main`,
`let x: Int`, `fx {`). Shown the Python error, it usually recovers. This is the familiar forgetting that comes with
specialising a model on one language, and Aether looks a lot like Python. Mixing Python pairs into the training
data held it off in short training runs but not at the length that gives the best Aether. Hence the advice: load
the adapter for Aether only.

## How to use

The adapter was trained with thinking **off** and with **no system message**. Serve it the same way:

- **vLLM / transformers**: pass `chat_template_kwargs={"enable_thinking": false}`. With vLLM, `--enable-lora`
  should serve the base model and the adapter side by side (not tested by us; the snippet below is).
- **Ollama** (OpenAI-compatible `/v1`, merged model): send `"reasoning_effort": "none"`.
  - A top-level `think: false` is ignored on that endpoint, and the model will reason first.
  - Set the context in a Modelfile (`PARAMETER num_ctx 32768`); `/v1` ignores per-request context options.


### Loading the adapter

```python
from transformers import AutoModelForCausalLM, AutoTokenizer
from peft import PeftModel

base = AutoModelForCausalLM.from_pretrained("Qwen/Qwen3.8-27B", torch_dtype="bfloat16", device_map="auto")
model = PeftModel.from_pretrained(base, "emkey1/Qwen3.8-27B-Aether-LoRA")
tok = AutoTokenizer.from_pretrained("emkey1/Qwen3.8-27B-Aether-LoRA")
msgs = [{"role": "user", "content": "Write an Aether program that prints the first ten squares."}]
inputs = tok.apply_chat_template(msgs, add_generation_prompt=True, enable_thinking=False,
                                 return_tensors="pt", return_dict=True).to(model.device)
out = model.generate(**inputs, max_new_tokens=800, do_sample=False)
print(tok.decode(out[0][inputs["input_ids"].shape[1]:], skip_special_tokens=True))
```

vLLM should be able to serve the base model and the adapter side by side (pick `aether` as the model name per
request). We have not tested this; the merged model on vLLM is what our scoring used:

```
vllm serve Qwen/Qwen3.8-27B --enable-lora --max-lora-rank 32 --lora-modules aether=emkey1/Qwen3.8-27B-Aether-LoRA
```

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

Shorter natural-language requests ("Write an Aether program that ...") also work; part of the training data is
in that form. Keep the Aether compiler in the loop: feeding its diagnostic back fixes most first-attempt misses.

## Training

- **Method**: LoRA r=32, alpha=64, dropout 0, on the attention and MLP projections.
  - Base model loaded in 4-bit (bitsandbytes); lr 1e-4; batch 1 × 8 gradient accumulation; seed 3407.
  - Loss on responses only (prompt tokens masked). No system message on any record.
  - Early stopping on 60 held-out training records, evaluated every 10 steps, patience 3. It stopped at epoch
    1.31; the best checkpoint, at epoch 1.17, is the one released.
  - Unsloth + TRL + PEFT on one GPU host, about 2¼ hours.
- **Data**: 1,681 instruction records (1,461 Aether, 220 Python) and 87 repair records. Every Aether program
  compiles and prints its stated output with the compiler, checked on every build. The records are:
  - a curated corpus of Aether programs and natural-language task/solution pairs;
  - repair drills: a broken program, the compiler's diagnostic, the fix;
  - targeted packs for the idioms earlier runs got wrong (file reads inside `fx`, JSON/TOON, memory streams,
    modules, tuple contracts, array value semantics). They were written from failure classes on the main suites,
    never from task text;
  - **distilled pairs**:
    - task statements and independent Python solutions written by
      [MiMo-V2.6-Flash-RL](https://huggingface.co/XiaomiMiMo/MiMo-V2.6-Flash-RL) (MIT);
    - Aether solutions written with the guide, by the untrained base model or, for the tasks it could not solve,
      by MiMo;
    - kept only where the Aether program prints exactly what the Python program prints;
    - all generation requests audited to come from MiMo.
  - Python pairs (MiMo's programs for the same tasks) to limit the drift away from Python.
- **Decontamination**:
  - Every training example is checked against every benchmark task: an identical expected output is dropped, and
    5-gram similarity to any reference solution must stay below 0.5.
  - The held-out suites are excluded from data authoring and from failure mining.

The language, its compiler, the guides, the benchmark tasks, the training data and the scoring reports are at
[emkey1/aether](https://github.com/emkey1/aether) (`bench/`, `training/`).

## Limitations

- **Aether only.** Python, and probably other languages, are degraded with the adapter loaded; see above.
- **Language version.** Aether evolves; the model knows the language as of 2026-10-19. Features added later will
  be unknown to it.
- **Program size.** It writes small, self-contained programs well. It has not been evaluated on multi-file
  projects or long programs.
- **First attempts still fail on some idioms that one repair round fixes.** Examples: an invented builtin name
  (`mstream_len` for `mstream_length`), a wrong argument count, or mutating an array parameter under Aether's
  value semantics. Use it with the compiler in the loop.

## License and attribution

Apache-2.0, as the base model. Distilled training data was generated with MiMo-V2.6-Flash-RL (MIT, Xiaomi).
