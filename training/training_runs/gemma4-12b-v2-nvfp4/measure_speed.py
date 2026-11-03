#!/usr/bin/env python3
"""Generation speed through vLLM's streaming API, on the prompts ../gemma4-12b-v2-q8/measure_speed.py uses on the Mac.

    python3 measure_speed.py --bench BENCH_CHECKOUT --model SERVED_NAME [--url http://localhost:8000]

Each answer is timed from its first streamed token to its last, so prompt processing does not count. The first answer
is left out. Prints the first answer's start, to show the model still writes Aether.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import statistics
import sys
import time
import urllib.request


def answer(url: str, model: str, prompt: str) -> tuple[str, int, float, float]:
    body = {"model": model, "messages": [{"role": "user", "content": prompt}], "stream": True, "temperature": 0,
            "seed": 42, "max_tokens": 1024, "chat_template_kwargs": {"enable_thinking": False},
            "stream_options": {"include_usage": True}}
    request = urllib.request.Request(f"{url}/v1/chat/completions", json.dumps(body).encode(),
                                     {"Content-Type": "application/json"})
    started = time.monotonic()
    first = last = None
    text, tokens = [], 0
    with urllib.request.urlopen(request, timeout=900) as response:
        for raw in response:
            line = raw.decode("utf-8").strip()
            if not line.startswith("data: ") or line == "data: [DONE]":
                continue
            chunk = json.loads(line[6:])
            if chunk.get("usage"):
                tokens = chunk["usage"]["completion_tokens"]
            for choice in chunk.get("choices") or []:
                if (choice.get("delta") or {}).get("content"):
                    last = time.monotonic()
                    first = first or last
                    text.append(choice["delta"]["content"])
    return "".join(text), tokens, (first or started) - started, (last or started) - (first or started)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--bench", required=True, help="a checkout with tools/ and bench/")
    ap.add_argument("--model", required=True)
    ap.add_argument("--url", default="http://localhost:8000")
    args = ap.parse_args()
    root = pathlib.Path(args.bench)
    sys.path.insert(0, str(root / "tools"))
    import aether_doc_bench as adb

    tasks = adb.load_tasks(root / "bench" / "tasks_cs.json")[:11]
    rows = [answer(args.url, args.model, adb.build_prompt("none", "", task)) for task in tasks]
    print("first answer starts:", repr(rows[0][0][:160]))
    warm = rows[1:]
    rates = [(tokens - 1) / span for _, tokens, _, span in warm if span > 0]
    print(f"{args.model}: {statistics.median(rates):.1f} tok/s generating (median of {len(rates)}), "
          f"{statistics.median(r[1] for r in warm):.0f} tokens median, "
          f"first token {statistics.median(r[2] for r in warm):.2f} s median")
    return 0


if __name__ == "__main__":
    sys.exit(main())
