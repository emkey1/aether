#!/usr/bin/env python3
"""Averages two merged BF16 exports of one recipe, tensor by tensor, into a new export. Needs numpy.

    average_weights.py A_DIR B_DIR OUT_DIR --expect-a SHA_PREFIX --expect-b SHA_PREFIX --base BASE_DIR --stats STATS.json

Identical tensors are copied unchanged. A differing tensor becomes the mean of the two in float32, rounded to BF16, and every value is checked to lie between its
parents'. The config, tokenizer and template must match in both parents and are copied. --base adds the similarity of each parent's change to the base to the stats.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import pathlib
import shutil
import struct

import numpy as np

WEIGHTS = "model.safetensors"
COPIED = ["config.json", "tokenizer.json", "chat_template.jinja", "generation_config.json", "processor_config.json", "tokenizer_config.json"]


def sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        while chunk := handle.read(16 << 20):
            digest.update(chunk)
    return digest.hexdigest()


def open_export(folder: pathlib.Path) -> dict:
    path = folder / WEIGHTS
    with path.open("rb") as handle:
        prefix = handle.read(8)
        body = handle.read(struct.unpack("<Q", prefix)[0])
    tensors = json.loads(body)
    tensors.pop("__metadata__", None)
    return {"path": path, "prefix": prefix + body, "start": len(prefix) + len(body), "tensors": tensors, "map": np.memmap(path, dtype=np.uint8, mode="r")}


def raw(export: dict, name: str) -> np.ndarray:
    begin, end = export["tensors"][name]["data_offsets"]
    return export["map"][export["start"] + begin:export["start"] + end]


def to_float(data: np.ndarray) -> np.ndarray:
    """BF16 is the top 16 bits of a float32."""
    return (np.frombuffer(data, dtype="<u2").astype(np.uint32) << 16).view(np.float32)


def to_bf16(values: np.ndarray) -> np.ndarray:
    """Rounds to the nearest BF16: a carry out of the low 16 bits moves to the next value, exponent included."""
    return ((values.view(np.uint32) + 0x8000) >> 16).astype("<u2")


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("a", type=pathlib.Path)
    ap.add_argument("b", type=pathlib.Path)
    ap.add_argument("out", type=pathlib.Path)
    ap.add_argument("--expect-a", required=True)
    ap.add_argument("--expect-b", required=True)
    ap.add_argument("--base", type=pathlib.Path)
    ap.add_argument("--stats", type=pathlib.Path, required=True)
    args = ap.parse_args()

    for name in COPIED:
        if (args.a / name).read_bytes() != (args.b / name).read_bytes():
            raise SystemExit(f"STOP: {name} differs between the parents")
    hashes = {"a": sha256(args.a / WEIGHTS), "b": sha256(args.b / WEIGHTS)}
    if not hashes["a"].startswith(args.expect_a) or not hashes["b"].startswith(args.expect_b):
        raise SystemExit(f"STOP: a parent's hash is not the expected one: {hashes}")
    a, b = open_export(args.a), open_export(args.b)
    base = open_export(args.base) if args.base else None
    if a["prefix"] != b["prefix"]:
        raise SystemExit("STOP: the parents' headers differ")
    if base and any(base["tensors"].get(n, {}).get("shape") != t["shape"] or base["tensors"][n]["dtype"] != t["dtype"] for n, t in a["tensors"].items()):
        raise SystemExit("STOP: the base's layout differs")
    if {t["dtype"] for t in a["tensors"].values()} != {"BF16"}:
        raise SystemExit("STOP: this script handles BF16 only")

    args.out.mkdir(parents=True, exist_ok=True)
    temporary = args.out / (WEIGHTS + ".tmp")
    differing, values_total, values_differing = 0, 0, 0
    dot = norm_a = norm_b = 0.0
    names = sorted(a["tensors"], key=lambda n: a["tensors"][n]["data_offsets"][0])
    with temporary.open("wb") as out:
        out.write(a["prefix"])
        position = 0
        for name in names:
            begin, end = a["tensors"][name]["data_offsets"]
            if begin != position:
                raise SystemExit(f"STOP: {name} does not follow the tensor before it")
            position = end
            data_a, data_b = raw(a, name), raw(b, name)
            values_total += (end - begin) // 2
            if np.array_equal(data_a, data_b):
                out.write(data_a.tobytes())
                continue
            differing += 1
            float_a, float_b = to_float(data_a), to_float(data_b)
            if not (np.isfinite(float_a).all() and np.isfinite(float_b).all()):
                raise SystemExit(f"STOP: {name} has a non-finite value")
            mean = to_bf16(float_a * 0.5 + float_b * 0.5)
            as_float = to_float(mean.tobytes())
            if (as_float < np.minimum(float_a, float_b)).any() or (as_float > np.maximum(float_a, float_b)).any():
                raise SystemExit(f"STOP: {name} has a value outside its parents'")
            values_differing += int((data_a.reshape(-1, 2) != data_b.reshape(-1, 2)).any(axis=1).sum())
            out.write(mean.tobytes())
            if base:
                reference = to_float(raw(base, name))
                delta_a, delta_b = (float_a - reference).astype(np.float64), (float_b - reference).astype(np.float64)
                dot += float(delta_a @ delta_b)
                norm_a += float(delta_a @ delta_a)
                norm_b += float(delta_b @ delta_b)
    os.replace(temporary, args.out / WEIGHTS)
    for name in COPIED:
        shutil.copyfile(args.a / name, args.out / name)

    stats = {
        "parents_sha256": hashes,
        "average_sha256": sha256(args.out / WEIGHTS),
        "tensors": len(names),
        "tensors_differing": differing,
        "values": values_total,
        "values_differing": values_differing,
    }
    if base:
        stats["cosine_of_changes_to_base"] = round(dot / math.sqrt(norm_a * norm_b), 4)
        stats["change_norm_a"] = round(math.sqrt(norm_a), 2)
        stats["change_norm_b"] = round(math.sqrt(norm_b), 2)
    args.stats.write_text(json.dumps(stats, indent=1) + "\n", encoding="utf-8")
    print(json.dumps(stats))


if __name__ == "__main__":
    main()
