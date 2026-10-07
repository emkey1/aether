"""Build capabilities of an aether binary, for capability-gated test laps.

A fixture or probe that needs an optional backend names it (curl, yyjson,
openai, sdl) and is skipped, not failed, on a build without it. The probes
mirror tests/run.sh and tests/run_examples.sh: the OpenAI and yyjson ext
builtins show in --dump-ext-builtins, and the curl and SDL builtins in
builtins_json(true).
"""
import json
import os
import subprocess
import tempfile

KNOWN = ("curl", "yyjson", "openai", "sdl")

_PROGRAM = "fn main() -> Void {\n    fx { println(builtins_json(true)); }\n    ret;\n}\n"


def detect(aether, timeout=30):
    """Return the set of capability names this binary has."""
    aether = os.path.abspath(aether)
    caps = set()
    try:
        ext = subprocess.run([aether, "--dump-ext-builtins"], capture_output=True,
                             text=True, timeout=timeout).stdout
    except (OSError, subprocess.TimeoutExpired):
        ext = ""
    if "openaichatcompletions" in ext.lower():
        caps.add("openai")
    if "yyjson" in ext.lower():
        caps.add("yyjson")
    with tempfile.TemporaryDirectory(prefix="aether_caps_") as tmp:
        src = os.path.join(tmp, "caps.aether")
        with open(src, "w", encoding="utf-8") as fh:
            fh.write(_PROGRAM)
        try:
            listing = subprocess.run([aether, "--no-cache", src], capture_output=True, text=True,
                                     timeout=timeout, cwd=tmp).stdout
        except (OSError, subprocess.TimeoutExpired):
            listing = ""
    try:
        entries = json.loads(listing.strip() or "[]")
    except json.JSONDecodeError:
        entries = []
    if isinstance(entries, dict):
        entries = entries.get("builtins", [])
    real = {e.get("name") for e in entries
            if isinstance(e, dict) and e.get("kind") not in (None, "unknown")}
    # A build without the backend still lists its names, as kind "unknown"
    # placeholders (see tools/gen_builtin_appendix.py).
    if "httprequest" in real:
        caps.add("curl")
    if "initgraph" in real or any(isinstance(e, dict) and e.get("category") == "graphics"
                                  for e in entries):
        caps.add("sdl")
    return caps


def read_caps_file(path):
    """The capability names a .cap sidecar lists (whitespace or comma separated)."""
    if not os.path.exists(path):
        return set()
    with open(path, encoding="utf-8") as fh:
        words = fh.read().replace(",", " ").split()
    unknown = [w for w in words if w not in KNOWN]
    if unknown:
        raise SystemExit(f"{path}: unknown capability {unknown[0]!r} (known: {', '.join(KNOWN)})")
    return set(words)


if __name__ == "__main__":
    import sys
    if len(sys.argv) != 2:
        raise SystemExit("usage: python3 tools/aether_caps.py AETHER_BIN  (prints the build's capabilities)")
    print(" ".join(sorted(detect(sys.argv[1]))))
