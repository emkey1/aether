#!/usr/bin/env python3
"""Generation speed and resident size through Ollama's native API, on the board's own prompts, one model at a time.

    python3 training/training_runs/gemma4-12b-v2-q8/measure_speed.py MODEL [MODEL ...]

Each model answers the first eleven tasks of the algorithm suite with no guide and thinking off. The first answer
carries the load, so only its load time counts.
"""

from __future__ import annotations

import json
import pathlib
import statistics
import subprocess
import sys
import urllib.request

REPO = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / "tools"))
import aether_doc_bench as adb  # noqa: E402


def chat(model: str, prompt: str) -> dict:
    body = {"model": model, "messages": [{"role": "user", "content": prompt}], "stream": False, "think": False,
            "options": {"temperature": 0, "seed": 42, "num_predict": 1024}}
    request = urllib.request.Request("http://localhost:11434/api/chat", json.dumps(body).encode(),
                                     {"Content-Type": "application/json"})
    with urllib.request.urlopen(request, timeout=900) as response:
        return json.load(response)


def rate(reply: dict, kind: str) -> float:
    return reply[f"{kind}_count"] / (reply[f"{kind}_duration"] / 1e9)


def main() -> int:
    tasks = adb.load_tasks(REPO / "bench" / "tasks_cs.json")[:11]
    for model in sys.argv[1:]:
        replies = [chat(model, adb.build_prompt("none", "", task)) for task in tasks]
        warm = replies[1:]
        print(f"{model}: {statistics.median(rate(r, 'eval') for r in warm):.1f} tok/s generating (median of {len(warm)}), "
              f"{statistics.median(r['eval_count'] for r in warm):.0f} tokens median, "
              f"load {replies[0].get('load_duration', 0) / 1e9:.1f} s")
        resident = subprocess.run(["ollama", "ps"], capture_output=True, text=True).stdout.splitlines()
        print("  " + next((line for line in resident if line.startswith(model)), "not listed by ollama ps"))
        subprocess.run(["ollama", "stop", model], capture_output=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
