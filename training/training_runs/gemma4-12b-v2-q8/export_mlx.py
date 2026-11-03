#!/usr/bin/env python3
"""Write an Ollama int8 import as one standard MLX folder, for publishing: the scored tensors, not a second quantization.

Ollama keeps each tensor in its own blob, as `<base>.weight`, `.weight.scale` and `.weight.bias`. MLX names the last two
`<base>.scales` and `<base>.biases`, and `ollama create` fuses that layout back into the same blobs, so `compare` can show
the round trip changed nothing. Standard library only; the data is copied, never decoded.

    export_mlx.py export --model NAME --merged MERGED_DIR --out OUT_DIR
    export_mlx.py compare --scored NAME --imported NAME
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import shutil
import struct
import sys

STORE = pathlib.Path(os.environ.get("OLLAMA_MODELS", "~/.ollama/models")).expanduser()
QUANTIZATION = {"group_size": 64, "bits": 8, "mode": "affine"}
COPIED = ["generation_config.json", "processor_config.json", "tokenizer.json", "tokenizer_config.json", "chat_template.jinja"]
MODELFILE = "FROM .\nPARAMETER num_ctx 32768\n"
CHUNK = 16 << 20


def blob(digest: str) -> pathlib.Path:
    return STORE / "blobs" / digest.replace(":", "-")


def manifest(model: str) -> dict:
    name, _, tag = model.partition(":")
    return json.loads((STORE / "manifests-v2/ollama.com/library" / name / (tag or "latest")).read_text())


def read_header(path: pathlib.Path) -> tuple[dict, int]:
    with path.open("rb") as handle:
        size = struct.unpack("<Q", handle.read(8))[0]
        return json.loads(handle.read(size)), 8 + size


def tensor_digests(path: pathlib.Path) -> dict:
    """Every tensor in a blob: its type, shape and the SHA-256 of its bytes, wherever the blob stores them."""
    header, start = read_header(path)
    result = {"__metadata__": header.get("__metadata__")}
    with path.open("rb") as handle:
        for name, info in header.items():
            if name == "__metadata__":
                continue
            first, last = info["data_offsets"]
            handle.seek(start + first)
            digest, left = hashlib.sha256(), last - first
            while left:
                chunk = handle.read(min(CHUNK, left))
                if not chunk:
                    sys.exit(f"STOP: {path} ended early")
                digest.update(chunk)
                left -= len(chunk)
            result[name] = (info["dtype"], tuple(info["shape"]), digest.hexdigest())
    return result


def mlx_name(name: str) -> str:
    if name.endswith(".weight.scale"):
        return name[: -len(".weight.scale")] + ".scales"
    if name.endswith(".weight.bias"):
        return name[: -len(".weight.bias")] + ".biases"
    return name


def export(args: argparse.Namespace) -> None:
    out = pathlib.Path(args.out)
    out.mkdir(parents=True, exist_ok=False)
    merged = pathlib.Path(args.merged)

    entries = []
    for layer in manifest(args.model)["layers"]:
        if not layer["mediaType"].endswith(".tensor"):
            continue
        path = blob(layer["digest"])
        header, start = read_header(path)
        kinds = {header.get("__metadata__", {}).get("quant_type"), None}
        if kinds - {"int8", None}:
            sys.exit(f"STOP: {layer['name']} is {kinds}, not int8")
        for name, info in header.items():
            if name != "__metadata__":
                entries.append((mlx_name(name), info, path, start))
    entries.sort(key=lambda entry: entry[0])
    if len({entry[0] for entry in entries}) != len(entries):
        sys.exit("STOP: two tensors share a name")

    table, offset = {"__metadata__": {"format": "mlx"}}, 0
    for name, info, _, _ in entries:
        size = info["data_offsets"][1] - info["data_offsets"][0]
        table[name] = {"dtype": info["dtype"], "shape": info["shape"], "data_offsets": [offset, offset + size]}
        offset += size
    raw = json.dumps(table, separators=(",", ":")).encode()
    raw += b" " * (-len(raw) % 8)

    with (out / "model.safetensors").open("wb") as target:
        target.write(struct.pack("<Q", len(raw)) + raw)
        for _, info, path, start in entries:
            first, last = info["data_offsets"]
            with path.open("rb") as source:
                source.seek(start + first)
                left = last - first
                while left:
                    chunk = source.read(min(CHUNK, left))
                    if not chunk:
                        sys.exit(f"STOP: {path} ended early")
                    target.write(chunk)
                    left -= len(chunk)

    config = json.loads((merged / "config.json").read_text())
    config["quantization"] = QUANTIZATION
    (out / "config.json").write_text(json.dumps(config, indent=2) + "\n")
    for name in COPIED:
        shutil.copyfile(merged / name, out / name)
    (out / "Modelfile").write_text(MODELFILE)
    print(f"{len(entries)} tensors, {offset} data bytes, {(out / 'model.safetensors').stat().st_size} bytes on disk")


def compare(args: argparse.Namespace) -> None:
    scored = {layer.get("name") or layer["mediaType"]: layer for layer in manifest(args.scored)["layers"]}
    imported = {layer.get("name") or layer["mediaType"]: layer for layer in manifest(args.imported)["layers"]}
    if set(scored) != set(imported):
        print("only scored:", sorted(set(scored) - set(imported))[:8])
        print("only imported:", sorted(set(imported) - set(scored))[:8])
    same = [n for n in scored if n in imported and scored[n]["digest"] == imported[n]["digest"]]
    differ = [n for n in scored if n in imported and scored[n]["digest"] != imported[n]["digest"]]
    tensors = [n for n in scored if scored[n]["mediaType"].endswith(".tensor")]
    print(f"{len(same)} of {len(scored)} layers identical; tensors {len([n for n in tensors if n in same])} of {len(tensors)}")
    # A blob's digest also covers its header's key order and where each tensor sits, which the two writers order differently.
    rewritten = [n for n in differ if n in tensors]
    wrong = [n for n in rewritten if tensor_digests(blob(scored[n]["digest"])) != tensor_digests(blob(imported[n]["digest"]))]
    print(f"{len(rewritten)} blobs differ as files; tensor name, type, shape or bytes differ in {len(wrong)}: {wrong[:8]}")
    print("other layers that differ:", [n for n in differ if n not in tensors])
    configs = [blob(manifest(model)["config"]["digest"]).read_text() for model in (args.scored, args.imported)]
    print("Ollama config identical:", configs[0] == configs[1])
    if wrong or set(scored) != set(imported) or configs[0] != configs[1]:
        sys.exit("STOP: the import is not the scored model")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    one = commands.add_parser("export")
    one.add_argument("--model", required=True)
    one.add_argument("--merged", required=True)
    one.add_argument("--out", required=True)
    one.set_defaults(run=export)
    two = commands.add_parser("compare")
    two.add_argument("--scored", required=True)
    two.add_argument("--imported", required=True)
    two.set_defaults(run=compare)
    args = parser.parse_args()
    args.run(args)


if __name__ == "__main__":
    main()
