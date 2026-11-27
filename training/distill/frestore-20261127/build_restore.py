#!/usr/bin/env python3
"""Build a frestore-20261127 candidate (README.md): a fine-tune's merged export with some layers' LoRA-target tensors
put back to the base's bytes. Standard library only; it streams, so it needs no GPU and little memory.

    build_restore.py --base DIR --tuned DIR --tuned-layers 16-47 --out DIR

Every one of the 328 LoRA-target tensors (the seven target projections in each of the 48 decoder layers) is copied
byte for byte from --tuned when its layer is in --tuned-layers, and from --base otherwise. Every other tensor must
already be byte-identical in the two files, so a candidate is exactly "the fine-tune with these layers' changes
removed"; the builder stops if one is not. The output keeps the tuned file's header byte for byte, and the
non-weight files (config, tokenizer, chat template) are copied from --tuned. restore_manifest.json records the
inputs' and the output's SHA-256 and the tensor counts.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import struct
import sys
import time

TARGET = re.compile(r"language_model\.(?:model\.)?layers\.(\d+)\.(self_attn\.[qkvo]_proj|mlp\.(?:gate|up|down)_proj)\.weight$")
LAYERS = 48
TARGETS = 328
CHUNK = 64 << 20


def header(path: str) -> tuple[int, bytes, dict]:
    with open(path, "rb") as handle:
        (length,) = struct.unpack("<Q", handle.read(8))
        raw = handle.read(length)
    return 8 + length, raw, json.loads(raw)


def layer_set(text: str) -> set[int]:
    low, _, high = text.partition("-")
    layers = set(range(int(low), int(high or low) + 1))
    if not layers or min(layers) < 0 or max(layers) >= LAYERS:
        raise SystemExit(f"STOP: --tuned-layers {text!r} is not a range inside 0-{LAYERS - 1}")
    return layers


def sha256(path: str) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        while block := handle.read(CHUNK):
            digest.update(block)
    return digest.hexdigest()


def read_range(handle, start: int, size: int):
    handle.seek(start)
    while size > 0:
        block = handle.read(min(CHUNK, size))
        if not block:
            raise SystemExit("STOP: a file ended inside a tensor")
        size -= len(block)
        yield block


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", required=True)
    ap.add_argument("--tuned", required=True)
    ap.add_argument("--tuned-layers", required=True, help="the layers whose target tensors come from --tuned, e.g. 16-47")
    ap.add_argument("--out", required=True)
    args = ap.parse_args()
    started = time.time()
    tuned_layers = layer_set(args.tuned_layers)
    base_file, tuned_file = (os.path.join(d, "model.safetensors") for d in (args.base, args.tuned))
    base_start, _, base_header = header(base_file)
    tuned_start, tuned_raw, tuned_header = header(tuned_file)
    base_header.pop("__metadata__", None)
    tuned_meta = tuned_header.pop("__metadata__", None)
    if set(base_header) != set(tuned_header):
        raise SystemExit(f"STOP: the two files name different tensors ({len(base_header)} and {len(tuned_header)})")
    for name, entry in tuned_header.items():
        if (entry["dtype"], entry["shape"]) != (base_header[name]["dtype"], base_header[name]["shape"]):
            raise SystemExit(f"STOP: {name}: dtype or shape differs between the two files")

    # The tuned file's data must be one contiguous run in header order of offsets, so its header can be kept as it is.
    order = sorted(tuned_header, key=lambda n: tuned_header[n]["data_offsets"][0])
    position = 0
    for name in order:
        begin, end = tuned_header[name]["data_offsets"]
        if begin != position:
            raise SystemExit(f"STOP: the tuned file's data is not contiguous at {name}")
        position = end
    if os.path.getsize(tuned_file) != tuned_start + position:
        raise SystemExit("STOP: the tuned file's size does not match its header")

    targets = {}
    for name in tuned_header:
        match = TARGET.search(name)
        if match:
            targets[name] = int(match.group(1))
    if len(targets) != TARGETS or set(targets.values()) != set(range(LAYERS)):
        raise SystemExit(f"STOP: expected {TARGETS} target tensors over {LAYERS} layers, found {len(targets)}")
    print(f"{len(tuned_header)} tensors, {TARGETS} targets; layers from the tuned file: "
          f"{min(tuned_layers)}-{max(tuned_layers)} ({len(tuned_layers)})", flush=True)

    os.makedirs(args.out, exist_ok=True)
    out_file = os.path.join(args.out, "model.safetensors")
    digest = hashlib.sha256()
    counts = {"from_tuned": 0, "from_base": 0, "untouched_equal": 0, "targets_equal_to_base": 0}
    with open(base_file, "rb") as base, open(tuned_file, "rb") as tuned, open(out_file + ".tmp", "wb") as out:
        lead = struct.pack("<Q", len(tuned_raw)) + tuned_raw
        out.write(lead)
        digest.update(lead)
        for name in order:
            entry, other = tuned_header[name], base_header[name]
            size = entry["data_offsets"][1] - entry["data_offsets"][0]
            tuned_at, base_at = tuned_start + entry["data_offsets"][0], base_start + other["data_offsets"][0]
            is_target = name in targets
            from_tuned = is_target and targets[name] in tuned_layers
            same = True
            for a, b in zip(read_range(tuned, tuned_at, size), read_range(base, base_at, size)):
                if a != b:
                    if not is_target:
                        raise SystemExit(f"STOP: {name} is not a LoRA target but differs from the base")
                    same = False
                block = a if from_tuned else b
                out.write(block)
                digest.update(block)
            if is_target:
                counts["from_tuned" if from_tuned else "from_base"] += 1
                # A target the fine-tune left as the base's would make the edit a no-op there.
                counts["targets_equal_to_base"] += same
            else:
                counts["untouched_equal"] += 1
    if os.path.getsize(out_file + ".tmp") != os.path.getsize(tuned_file):
        raise SystemExit("STOP: the output's size differs from the tuned file's")
    if counts["targets_equal_to_base"]:
        raise SystemExit(f"STOP: {counts['targets_equal_to_base']} target tensors of the tuned file equal the base's")
    os.replace(out_file + ".tmp", out_file)
    for entry in sorted(os.listdir(args.tuned)):
        if entry != "model.safetensors" and os.path.isfile(os.path.join(args.tuned, entry)):
            shutil.copyfile(os.path.join(args.tuned, entry), os.path.join(args.out, entry))
    manifest = {
        "built_by": "training/distill/frestore-20261127/build_restore.py",
        "base": args.base, "tuned": args.tuned, "tuned_layers": [min(tuned_layers), max(tuned_layers)],
        "base_sha256": sha256(base_file), "tuned_sha256": sha256(tuned_file), "out_sha256": digest.hexdigest(),
        "tensors": len(tuned_header), "metadata": tuned_meta, **counts,
        "seconds": round(time.time() - started),
    }
    with open(os.path.join(args.out, "restore_manifest.json"), "w", encoding="utf-8") as handle:
        json.dump(manifest, handle, indent=1)
    print(json.dumps(manifest, indent=1), flush=True)
    print("BUILT", flush=True)


if __name__ == "__main__":
    sys.exit(main())
