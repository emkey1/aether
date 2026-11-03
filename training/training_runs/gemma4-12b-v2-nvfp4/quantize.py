#!/usr/bin/env python3
"""The merged Gemma fine-tune to NVFP4 with NVIDIA's ModelOpt: text layers only, calibrated on its own training records.

    python3 quantize.py --model MERGED_16BIT --data aether_instruction_sft.jsonl --out OUT [--samples 64]

W4A4: weights and inputs at 4 bits, which is the form hardware FP4 kernels run. The inputs need calibrating. The
samples come only from the training data, so no benchmark task shapes the scales. The export keeps the base's non-weight
files, as the merged export does, because the trainer's transformers rewrites config.json in a form vLLM cannot read.
"""

from __future__ import annotations

import argparse
import copy
import json
import pathlib
import random
import shutil
import struct

import modelopt.torch.quantization as mtq
import torch
from modelopt.torch.export import export_hf_checkpoint
from transformers import AutoModelForCausalLM, AutoTokenizer

BASE_FILES = ("chat_template.jinja", "generation_config.json", "processor_config.json", "tokenizer.json",
              "tokenizer_config.json")
SKIP = ("*vision*", "*audio*", "*embed_*", "*multi_modal*")


def calibration_texts(path: str, samples: int, tokenizer) -> list[str]:
    with open(path, encoding="utf-8") as handle:
        records = [json.loads(line) for line in handle if line.strip()]
    records = [r for r in records if r.get("language") == "aether"]
    texts = []
    for record in random.Random(42).sample(records, samples):
        prompt = tokenizer.apply_chat_template(record["messages"][:-1], tokenize=False, add_generation_prompt=True,
                                               enable_thinking=False)
        texts.append(prompt + record["messages"][-1]["content"] + "<turn|>")
    return texts


def restore_checkpoint_names(path: pathlib.Path) -> int:
    """ModelOpt exports transformers' in-memory names. Vision's differ from the checkpoint's, and vLLM reads the latter."""
    with open(path, "rb") as source:
        size = struct.unpack("<Q", source.read(8))[0]
        header = json.loads(source.read(size))
        renamed = {}
        changed = 0
        for key, value in header.items():
            new = key.replace("model.embed_vision.multimodal_embedder.", "model.embed_vision.")
            for part in ("patch_dense", "patch_ln1", "patch_ln2", "pos_embedding", "pos_norm"):
                new = new.replace(f"model.embed_vision.{part}", f"model.vision_embedder.{part}")
            changed += new != key
            renamed[new] = value
        encoded = json.dumps(renamed, separators=(",", ":")).encode("utf-8")
        encoded += b" " * (-len(encoded) % 8)
        partial = path.with_suffix(".partial")
        with open(partial, "wb") as target:
            target.write(struct.pack("<Q", len(encoded)))
            target.write(encoded)
            shutil.copyfileobj(source, target, 1 << 24)
    partial.replace(path)
    return changed


def exclude_vllm_prefixes(out: pathlib.Path) -> None:
    """vLLM matches exclusions against its own module prefixes, which have no `model.` and name the vision embedder."""
    extra = ["embed_audio*", "embed_vision*", "vision_embedder*"]
    hf_quant = json.loads((out / "hf_quant_config.json").read_text(encoding="utf-8"))
    hf_quant["quantization"]["exclude_modules"] = sorted(set(hf_quant["quantization"]["exclude_modules"]) | set(extra))
    (out / "hf_quant_config.json").write_text(json.dumps(hf_quant, indent=4) + "\n", encoding="utf-8")
    config = json.loads((out / "config.json").read_text(encoding="utf-8"))
    config["quantization_config"]["ignore"] = sorted(set(config["quantization_config"]["ignore"]) | set(extra))
    (out / "config.json").write_text(json.dumps(config, indent=2) + "\n", encoding="utf-8")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--model", required=True)
    ap.add_argument("--data", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--samples", type=int, default=64)
    ap.add_argument("--rename-only", action="store_true", help="only restore the checkpoint names in an existing export")
    args = ap.parse_args()

    if args.rename_only:
        print("renamed", restore_checkpoint_names(pathlib.Path(args.out) / "model.safetensors"), "tensors", flush=True)
        exclude_vllm_prefixes(pathlib.Path(args.out))
        return

    tokenizer = AutoTokenizer.from_pretrained(args.model)
    model = AutoModelForCausalLM.from_pretrained(args.model, dtype=torch.bfloat16, device_map={"": 0})
    model.eval()
    texts = calibration_texts(args.data, args.samples, tokenizer)
    print("first calibration text starts:", repr(texts[0][:60]), "ends:", repr(texts[0][-40:]), flush=True)

    def forward_loop(net) -> None:
        with torch.no_grad():
            for index, text in enumerate(texts):
                batch = tokenizer(text, add_special_tokens=False, return_tensors="pt").to(0)
                net(**batch)
                if index % 16 == 0:
                    print(f"calibrated {index + 1} of {len(texts)}", flush=True)

    config = copy.deepcopy(mtq.config.NVFP4_DEFAULT_CFG)
    for pattern in SKIP:
        config["quant_cfg"][pattern] = {"enable": False}
    model = mtq.quantize(model, config, forward_loop)
    mtq.print_quant_summary(model)

    out = pathlib.Path(args.out)
    export_hf_checkpoint(model, dtype=torch.bfloat16, export_dir=out)
    print("renamed", restore_checkpoint_names(out / "model.safetensors"), "tensors", flush=True)

    source = pathlib.Path(args.model)
    exported = json.loads((out / "config.json").read_text(encoding="utf-8"))
    restored = json.loads((source / "config.json").read_text(encoding="utf-8"))
    restored["quantization_config"] = exported["quantization_config"]
    (out / "config.json").write_text(json.dumps(restored, indent=2) + "\n", encoding="utf-8")
    for name in BASE_FILES:
        shutil.copy2(source / name, out / name)
    exclude_vllm_prefixes(out)
    print("quantization_config:", json.dumps(restored["quantization_config"])[:600], flush=True)
    print("done", sorted(p.name for p in out.iterdir()), flush=True)


if __name__ == "__main__":
    main()
