#!/usr/bin/env python3
"""Recompute the card A/B table in README.md from the packed reports beside it.

    python3 bench/results/card_ab_20261119/tabulate.py
"""
import collections
import gzip
import json
import pathlib

HERE = pathlib.Path(__file__).resolve().parent


def exact(attempt: dict) -> bool:
    return bool((attempt.get("run") or {}).get("exact_stdout_match"))


def main() -> None:
    totals = collections.defaultdict(collections.Counter)
    cases = {}
    for path in sorted(HERE.glob("qwen35-9b__tasks_*.json.gz")):
        suite = path.name.split("__")[1].split(".")[0]
        report = json.loads(gzip.open(path, "rt", encoding="utf-8").read())
        for dest in report["destinations"]:
            for variant in dest["variants"]:
                name = variant["doc_name"]
                for case in variant["results"]:
                    attempts = case["attempts"]
                    row = (exact(attempts[0]), any(exact(a) for a in attempts),
                           bool(attempts[0].get("no_answer")), bool(case.get("infra_failed")))
                    cases[(suite, case["task_id"], name)] = row
                    for key in (suite, "all"):
                        t = totals[(key, name)]
                        t["n"] += 1
                        t["first"] += row[0]
                        t["repaired"] += row[1]
                        t["no_answer"] += row[2]
                        t["infra"] += row[3]
    for key in sorted(totals):
        print(key[0], key[1], dict(totals[key]))
    for label, i in (("first attempt", 0), ("after repair", 1)):
        old_only = sum(1 for (s, t, n), r in cases.items()
                       if n == "card_old" and r[i] and not cases[(s, t, "card_new")][i])
        new_only = sum(1 for (s, t, n), r in cases.items()
                       if n == "card_new" and r[i] and not cases[(s, t, "card_old")][i])
        print(f"{label}: right under the old card only {old_only}, under the new card only {new_only}")
    both = [(r, cases[(s, t, "card_new")]) for (s, t, n), r in cases.items()
            if n == "card_old" and not r[2] and not cases[(s, t, "card_new")][2]]
    print(f"both cards answered on the first attempt: {len(both)} tasks; "
          f"right: old {sum(o[0] for o, _ in both)}, new {sum(n[0] for _, n in both)}")


if __name__ == "__main__":
    main()
