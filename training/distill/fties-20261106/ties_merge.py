#!/usr/bin/env python3
"""TIES-merges two merged BF16 exports of one recipe against their base, tensor by tensor, into a new export. Needs numpy.

    ties_merge.py A_DIR B_DIR OUT_DIR --expect-a SHA_PREFIX --expect-b SHA_PREFIX --base BASE_DIR --stats STATS.json

Identical tensors are copied unchanged. For a differing tensor, each parent's change to the base keeps its top DENSITY share by magnitude; each value's sign is the sign of
the sum of the trimmed changes; the merged change is the mean of the trimmed changes that agree with it, added to the base at full scale and rounded to BF16.
The config, tokenizer and template must match in both parents and are copied.
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
DENSITY = 0.2
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
    ap.add_argument("--base", type=pathlib.Path, required=True)
    ap.add_argument("--stats", type=pathlib.Path, required=True)
    args = ap.parse_args()

    for name in COPIED:
        if (args.a / name).read_bytes() != (args.b / name).read_bytes():
            raise SystemExit(f"STOP: {name} differs between the parents")
    hashes = {"a": sha256(args.a / WEIGHTS), "b": sha256(args.b / WEIGHTS)}
    if not hashes["a"].startswith(args.expect_a) or not hashes["b"].startswith(args.expect_b):
        raise SystemExit(f"STOP: a parent's hash is not the expected one: {hashes}")
    a, b = open_export(args.a), open_export(args.b)
    base = open_export(args.base)
    if a["prefix"] != b["prefix"]:
        raise SystemExit("STOP: the parents' headers differ")
    if any(base["tensors"].get(n, {}).get("shape") != t["shape"] or base["tensors"][n]["dtype"] != t["dtype"] for n, t in a["tensors"].items()):
        raise SystemExit("STOP: the base's layout differs")
    if {t["dtype"] for t in a["tensors"].values()} != {"BF16"}:
        raise SystemExit("STOP: this script handles BF16 only")

    args.out.mkdir(parents=True, exist_ok=True)
    temporary = args.out / (WEIGHTS + ".tmp")
    differing, values_total, values_differing = 0, 0, 0
    dot = norm_a = norm_b = norm_m = 0.0
    kept_values = 0
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
            reference = to_float(raw(base, name))
            delta_a, delta_b = float_a - reference, float_b - reference
            trimmed = []
            for delta in (delta_a, delta_b):
                keep = max(1, int(DENSITY * delta.size))
                threshold = np.partition(np.abs(delta), delta.size - keep)[delta.size - keep]
                trimmed.append(np.where(np.abs(delta) >= threshold, delta, 0.0).astype(np.float32))
            sign = np.sign(trimmed[0] + trimmed[1])
            agree = [(np.sign(t) == sign) & (t != 0) for t in trimmed]
            count = agree[0].astype(np.float32) + agree[1].astype(np.float32)
            total = np.where(agree[0], trimmed[0], 0.0) + np.where(agree[1], trimmed[1], 0.0)
            change = np.where(count > 0, total / np.maximum(count, 1), 0.0).astype(np.float32)
            merged = to_bf16((reference + change).astype(np.float32))
            if not np.isfinite(to_float(merged.tobytes())).all():
                raise SystemExit(f"STOP: {name} has a non-finite merged value")
            values_differing += int((data_a.reshape(-1, 2) != data_b.reshape(-1, 2)).any(axis=1).sum())
            kept_values += int((count > 0).sum())
            out.write(merged.tobytes())
            d_a, d_b, d_m = delta_a.astype(np.float64), delta_b.astype(np.float64), change.astype(np.float64)
            dot += float(d_a @ d_b)
            norm_a += float(d_a @ d_a)
            norm_b += float(d_b @ d_b)
            norm_m += float(d_m @ d_m)
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
    stats["density"] = DENSITY
    stats["values_changed_from_base"] = kept_values
    stats["cosine_of_changes_to_base"] = round(dot / math.sqrt(norm_a * norm_b), 4)
    stats["change_norm_a"] = round(math.sqrt(norm_a), 2)
    stats["change_norm_b"] = round(math.sqrt(norm_b), 2)
    stats["change_norm_merged"] = round(math.sqrt(norm_m), 2)
    args.stats.write_text(json.dumps(stats, indent=1) + "\n", encoding="utf-8")
    print(json.dumps(stats))


if __name__ == "__main__":
    main()
