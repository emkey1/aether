#!/usr/bin/env python3
"""The fgraft-20261126 screen (README.md): eval loss of whole-layer grafts of two fine-tunes, built in memory.

Loads the base once in bf16, then for each candidate copies every LoRA-target tensor whole from the source its layer
names (base, a, b) and computes the loss on held-out chat records the way the trainer does: rendered as served with
thinking off, loss on the response tokens only. Writes per-record losses after every candidate. Run in the family's
trainer image with the exports mounted:

    graft_eval.py --base DIR --a DIR --b DIR --records control.jsonl --out losses.json [--candidates v3,g24_a] [--limit N]
"""
from __future__ import annotations

import argparse
import json
import os
import re
import time

import torch
import transformers
from safetensors import safe_open
from transformers import AutoConfig, AutoTokenizer

TARGET = re.compile(r"language_model\.(?:model\.)?layers\.(\d+)\.(self_attn\.[qkvo]_proj|mlp\.(?:gate|up|down)_proj)\.weight$")
LAYERS = 48
RESPONSE_PART = "<|turn>model\n<|channel>thought\n<channel|>"


def split(k: int, low: str, high: str):
    return lambda layer: low if layer < k else high


CANDIDATES = {  # name: layer -> source ("base", "a" = gemma4-12b-v3, "b" = gemma4-12b-v3-s42)
    "v3": split(LAYERS, "a", "a"),
    "v3_s42": split(LAYERS, "b", "b"),
    "g24_a": split(24, "a", "b"),
    "g24_b": split(24, "b", "a"),
    "g16_a": split(16, "a", "b"),
    "g16_b": split(16, "b", "a"),
    "late_v3": split(20, "base", "a"),
    "early_v3": split(20, "a", "base"),
    "base": split(LAYERS, "base", "base"),
}


def load_model(base: str):
    config = AutoConfig.from_pretrained(base)
    cls = getattr(transformers, (config.architectures or [""])[0], None) or transformers.AutoModelForImageTextToText
    model = cls.from_pretrained(base, dtype=torch.bfloat16, device_map="cuda")
    model.eval()
    return model


def encode(tokenizer, records: list[dict]) -> list[tuple[list[int], int]]:
    encoded = []
    for record in records:
        messages = record["messages"]
        full = tokenizer.apply_chat_template(messages, tokenize=False, add_generation_prompt=False, enable_thinking=False)
        prompt = tokenizer.apply_chat_template(messages[:-1], tokenize=False, add_generation_prompt=True, enable_thinking=False)
        full = prompt + full[len(os.path.commonprefix([prompt, full])):]  # the trainer's --render-as-served
        if not (prompt.endswith(RESPONSE_PART) and full.startswith("<bos>") and full.count("<bos>") == 1):
            raise SystemExit(f"STOP: {record.get('id')}: the rendering is not the served one: {prompt[-60:]!r}")
        p = tokenizer(prompt, add_special_tokens=False)["input_ids"]
        f = tokenizer(full, add_special_tokens=False)["input_ids"]
        if f[:len(p)] != p or len(f) <= len(p):
            raise SystemExit(f"STOP: {record.get('id')}: the prompt's tokens are not a prefix of the record's")
        encoded.append((f, len(p)))
    return encoded


@torch.inference_mode()
def record_losses(model, encoded) -> list[tuple[float, int]]:
    out = []
    for ids, p in encoded:
        x = torch.tensor([ids], device="cuda")
        n = len(ids) - p
        try:
            logits = model(input_ids=x, attention_mask=torch.ones_like(x), logits_to_keep=n + 1).logits[0, :-1]
        except TypeError:
            logits = model(input_ids=x, attention_mask=torch.ones_like(x)).logits[0, p - 1:-1]
        total = torch.nn.functional.cross_entropy(logits.float(), x[0, p:], reduction="sum").item()
        out.append((total, n))
    return out


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", required=True)
    ap.add_argument("--a", required=True)
    ap.add_argument("--b", required=True)
    ap.add_argument("--records", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--candidates", default=",".join(CANDIDATES))
    ap.add_argument("--limit", type=int, default=0)
    args = ap.parse_args()

    started = time.time()
    tokenizer = AutoTokenizer.from_pretrained(args.base)
    records = [json.loads(line) for line in open(args.records, encoding="utf-8") if line.strip()]
    if args.limit:
        records = records[:args.limit]
    encoded = encode(tokenizer, records)
    print(f"{len(encoded)} records, {sum(len(f) - p for f, p in encoded)} response tokens, longest {max(len(f) for f, _ in encoded)}", flush=True)

    model = load_model(args.base)
    params = dict(model.named_parameters())
    handles = {key: safe_open(os.path.join(path, "model.safetensors"), framework="pt", device="cpu")
               for key, path in (("base", args.base), ("a", args.a), ("b", args.b))}
    targets = {}
    for name in handles["base"].keys():
        match = TARGET.search(name)
        if not match:
            continue
        suffix = f"layers.{match.group(1)}.{match.group(2)}.weight"
        hits = [p for p in params if p.endswith(suffix) and "language_model" in p]
        if len(hits) != 1:
            raise SystemExit(f"STOP: {name} matches {len(hits)} parameters")
        targets[name] = (int(match.group(1)), params[hits[0]])
    if len(targets) != 328 or len({id(t[1]) for t in targets.values()}) != 328:
        raise SystemExit(f"STOP: expected 328 distinct targets, found {len(targets)}")
    for name, (_, param) in targets.items():  # the mapping is right only if every target still holds the base's tensor
        tensor = handles["base"].get_tensor(name)
        if tensor.shape != param.shape or not torch.equal(tensor.to(param.device), param.data):
            raise SystemExit(f"STOP: {name} is not the base's tensor after loading")
    print(f"model loaded, 328 targets mapped and checked against the base, {time.time() - started:.0f} s", flush=True)

    current = {name: "base" for name in targets}
    results = {"records": [r.get("id") for r in records], "candidates": {}}
    for candidate in args.candidates.split(","):
        rule = CANDIDATES[candidate]
        t0 = time.time()
        copied = 0
        for name, (layer, param) in targets.items():
            source = rule(layer)
            if current[name] != source:
                param.data.copy_(handles[source].get_tensor(name).to(param.device))
                current[name] = source
                copied += 1
        sources = {s: sum(1 for v in current.values() if v == s) for s in ("base", "a", "b")}
        losses = record_losses(model, encoded)
        per_record = [total / n for total, n in losses]
        mean = sum(per_record) / len(per_record)
        token_mean = sum(t for t, _ in losses) / sum(n for _, n in losses)
        results["candidates"][candidate] = {"sources": sources, "mean": mean, "token_mean": token_mean, "per_record": per_record}
        with open(args.out + ".tmp", "w", encoding="utf-8") as handle:
            json.dump(results, handle)
        os.replace(args.out + ".tmp", args.out)
        print(f"{candidate:9s} mean {mean:.4f} token-weighted {token_mean:.4f}  tensors {sources}, copied {copied}, "
              f"{time.time() - t0:.0f} s", flush=True)
    print(f"DONE {len(results['candidates'])} candidates in {time.time() - started:.0f} s", flush=True)


if __name__ == "__main__":
    main()
