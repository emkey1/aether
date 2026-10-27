#!/usr/bin/env python3
"""Paired analysis of a small-guide A/B: two guide variants answered per task with the same seed.

Each unit report holds both guides, answered per task with the same seed (42 + repeat), so a
case pairs with its twin by (suite, task, repeat). The D37d rule: the new guide is non-inferior
when the 95% lower bound of FA(new) - FA(old) is above -3 points, by a paired percentile
bootstrap that resamples task clusters (all repeats of a task under both guides together)
within each suite.

  analyze_small_ab.py --harness AETHER/tools/aether_doc_bench.py RESULTS_DIR [--old NAME --new NAME] [--json OUT]
"""
from __future__ import annotations

import argparse
import collections
import gzip
import importlib.util
import json
import math
import pathlib
import random
import re
import sys

UNIT_RE = re.compile(r"^(?P<dest>.+)__(?P<suite>tasks_[a-z0-9_]+)__r(?P<r>\d+)\.json(?:\.gz)?$")
MARGIN = 0.03
CLASSES = ("pass", "silent_wrong", "crash_hang", "no_answer", "uncoded_error", "coded_error", "infra_failed", "not_sent")


def load_harness(path: str):
    spec = importlib.util.spec_from_file_location("aether_doc_bench", path)
    mod = importlib.util.module_from_spec(spec)
    sys.modules["aether_doc_bench"] = mod
    spec.loader.exec_module(mod)
    return mod


def paired_boot(pairs: list[dict], key: str, draws: int = 4000, seed: int = 7):
    """95% percentile CI of rate(new) - rate(old), resampling task clusters per suite."""
    strata: dict[str, dict[str, list[tuple[int, int]]]] = {}
    for p in pairs:
        strata.setdefault(p["suite"], {}).setdefault(p["task"], []).append((int(p["old"][key]), int(p["new"][key])))
    groups = [list(t.values()) for t in strata.values()]
    rng = random.Random(seed)
    stats = []
    for _ in range(draws):
        o = n = tot = 0
        for clusters in groups:
            for _ in range(len(clusters)):
                pick = clusters[rng.randrange(len(clusters))]
                o += sum(a for a, _ in pick)
                n += sum(b for _, b in pick)
                tot += len(pick)
        stats.append((n - o) / tot)
    stats.sort()
    return stats[int(0.025 * draws)], stats[int(0.975 * draws) - 1]


def sign_test(b: int, c: int) -> float:
    """Two-sided exact binomial p for b vs c discordant pairs."""
    n = b + c
    if n == 0:
        return 1.0
    k = min(b, c)
    p = sum(math.comb(n, i) for i in range(k + 1)) / 2 ** n
    return min(1.0, 2 * p)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--harness", required=True)
    ap.add_argument("dir")
    ap.add_argument("--json")
    ap.add_argument("--old", default="small_current")
    ap.add_argument("--new", default="small_dense")
    args = ap.parse_args()
    bench = load_harness(args.harness)
    OLD, NEW = args.old, args.new

    cases: dict[tuple, dict] = {}
    usage = collections.defaultdict(lambda: collections.Counter())
    units = 0
    for path in sorted(pathlib.Path(args.dir).glob("*.json*")):
        m = UNIT_RE.match(path.name)
        if not m:
            continue
        units += 1
        report = json.loads(gzip.decompress(path.read_bytes()) if path.suffix == ".gz" else path.read_text())
        for v in report["destinations"][0]["variants"]:
            doc = v["doc_name"]
            for case in v["results"]:
                first = bench.classify_attempt(bench.first_attempt_of(case))
                attempts = case.get("attempts") or [case]
                for a in attempts:
                    u = a.get("usage") or {}
                    for k in ("prompt_tokens", "completion_tokens", "reasoning_tokens", "cached_tokens"):
                        usage[doc][k] += u.get(k) or 0
                    usage[doc]["requests"] += 1
                usage[doc]["first_prompt_tokens"] += ((attempts[0].get("usage") or {}).get("prompt_tokens") or 0)
                key = (m["suite"], case["task_id"], int(m["r"]))
                cases.setdefault(key, {})[doc] = {
                    "fa": first == "pass",
                    "fx": bool(case["run"]["exact_stdout_match"]),
                    "first": first,
                    "repaired": bool(case.get("resolved_after_repair")),
                    "seed": case.get("seed"),
                }
    pairs = []
    unpaired = 0
    unmeasured = []
    for (suite, task, r), docs in sorted(cases.items()):
        if OLD in docs and NEW in docs:
            if docs[OLD]["seed"] != docs[NEW]["seed"]:
                print(f"WARNING seed differs for {suite} {task} r{r}", file=sys.stderr)
            pair = {"suite": suite, "task": task, "repeat": r, "old": docs[OLD], "new": docs[NEW]}
            # A first attempt the provider never answered is missing data, not a failure: the
            # harness withholds its headline for it, so the pair leaves the comparison.
            if {docs[OLD]["first"], docs[NEW]["first"]} & {"infra_failed", "not_sent"}:
                unmeasured.append(pair)
            else:
                pairs.append(pair)
        else:
            unpaired += 1
    n = len(pairs)
    out: dict = {"units": units, "pairs": n, "unpaired": unpaired, "tasks": len({(p["suite"], p["task"]) for p in pairs}),
                 "unmeasured_pairs": [[p["suite"], p["task"], p["repeat"]] for p in unmeasured]}

    def rate(sel, doc, key):
        return sum(p[doc][key] for p in sel) / len(sel) if sel else float("nan")

    for key in ("fa", "fx"):
        lo, hi = paired_boot(pairs, key)
        o, nw = rate(pairs, "old", key), rate(pairs, "new", key)
        b = sum(1 for p in pairs if p["old"][key] and not p["new"][key])
        c = sum(1 for p in pairs if p["new"][key] and not p["old"][key])
        out[key] = {"old": o, "new": nw, "diff": nw - o, "ci95": [lo, hi], "old_only": b, "new_only": c,
                    "sign_p": sign_test(b, c), "noninferior": lo > -MARGIN}
    out["first_classes"] = {d: dict(collections.Counter(p[d]["first"] for p in pairs)) for d in ("old", "new")}
    out["by_suite"] = {}
    for suite in sorted({p["suite"] for p in pairs}):
        sel = [p for p in pairs if p["suite"] == suite]
        out["by_suite"][suite] = {"cases": len(sel), "fa_old": sum(p["old"]["fa"] for p in sel), "fa_new": sum(p["new"]["fa"] for p in sel),
                                  "fx_old": sum(p["old"]["fx"] for p in sel), "fx_new": sum(p["new"]["fx"] for p in sel)}
    out["by_repeat"] = {}
    for r in sorted({p["repeat"] for p in pairs}):
        sel = [p for p in pairs if p["repeat"] == r]
        out["by_repeat"][r] = {"cases": len(sel), "fa_old": rate(sel, "old", "fa"), "fa_new": rate(sel, "new", "fa")}
    per_task = collections.defaultdict(lambda: [0, 0, 0])
    for p in pairs:
        t = per_task[(p["suite"], p["task"])]
        t[0] += p["old"]["fa"]; t[1] += p["new"]["fa"]; t[2] += 1
    out["task_moves"] = sorted(([s, t, o, nw, k] for (s, t), (o, nw, k) in per_task.items() if o != nw), key=lambda x: (x[3] - x[2], x[0], x[1]))
    names = {OLD: "old", NEW: "new"}
    out["usage"] = {names.get(d, d): dict(c) for d, c in usage.items()}

    pc = lambda x: f"{100 * x:.1f}"
    print(f"units {units}, paired cases {n} over {out['tasks']} tasks, unpaired {unpaired}, "
          f"dropped with an unmeasured side {len(unmeasured)}")
    for s, t, r in out["unmeasured_pairs"]:
        print(f"  unmeasured: {s} {t} r{r}")
    for key, label in (("fa", "first attempt"), ("fx", "after repair (no-retry rule)")):
        r = out[key]
        print(f"{label}: old {pc(r['old'])}%  new {pc(r['new'])}%  diff {r['diff'] * 100:+.1f} pts  95% CI [{r['ci95'][0] * 100:+.1f}, {r['ci95'][1] * 100:+.1f}]"
              f"  discordant old-only {r['old_only']} new-only {r['new_only']} (sign p {r['sign_p']:.3f})"
              f"  non-inferior at -3: {'YES' if r['noninferior'] else 'NO'}")
    for d in ("old", "new"):
        fc = out["first_classes"][d]
        print(f"first-attempt classes {d}: " + "  ".join(f"{c}={fc.get(c, 0)}" for c in CLASSES))
    for s, r in out["by_suite"].items():
        print(f"  {s:22s} cases {r['cases']:3d}  FA old {r['fa_old']:3d} new {r['fa_new']:3d}   FX old {r['fx_old']:3d} new {r['fx_new']:3d}")
    for rr, r in out["by_repeat"].items():
        print(f"  repeat {rr}: FA old {pc(r['fa_old'])} new {pc(r['fa_new'])}")
    print("task moves (suite task old new of k):")
    for s, t, o, nw, k in out["task_moves"]:
        print(f"  {s:20s} {t:34s} {o}/{k} -> {nw}/{k}")
    for d, u in out["usage"].items():
        print(f"usage {d}: requests {u.get('requests')} prompt {u.get('prompt_tokens')} (first attempts {u.get('first_prompt_tokens')}) "
              f"completion {u.get('completion_tokens')} reasoning {u.get('reasoning_tokens')} cached {u.get('cached_tokens')}")
    if args.json:
        pathlib.Path(args.json).write_text(json.dumps(out, indent=1) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
