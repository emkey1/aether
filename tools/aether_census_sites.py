#!/usr/bin/env python3
"""Tabulate AETHER_EXPERIMENT_LOG site rows (see src/aether/experiment.h).

Each log row is: arm, source path, line, use, sink label, sink type, action.
The census tool compiles every program twice per arm (--dump-ast-json, then
the run), so a site is logged twice; --invocations divides it back out.

    tools/aether_census_sites.py --invocations 2 \\
        --set tests=<repo>/tests --set corpus=<corpus dir> --set archive=<dir> \\
        current=<log> int=<log> ...

Prints, per log: sites by use and action, sites and programs by set, and the
per-program action mix.
"""
import argparse
import collections
import os
import sys


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("logs", nargs="+", help="NAME=PATH")
    ap.add_argument("--invocations", type=int, default=1)
    ap.add_argument("--set", action="append", default=[], help="NAME=DIR (longest prefix wins)")
    ap.add_argument("--arm", default=None, help="only rows whose first column is this")
    args = ap.parse_args()
    sets = []
    for s in args.set:
        n, _, d = s.partition("=")
        sets.append((n, os.path.abspath(d)))
    sets.sort(key=lambda x: -len(x[1]))

    def set_of(path):
        ap_ = os.path.abspath(path)
        for n, d in sets:
            if ap_.startswith(d + os.sep):
                return n
        return "other"

    for spec in args.logs:
        name, _, path = spec.partition("=")
        rows = collections.Counter()
        for line in open(path, encoding="utf-8", errors="replace"):
            parts = line.rstrip("\n").split("\t")
            if len(parts) != 7:
                continue
            if args.arm and parts[0] != args.arm:
                continue
            rows[tuple(parts)] += 1
        sites = []
        for key, n in rows.items():
            sites.extend([key] * max(1, n // args.invocations))
        print("== %s (%d sites)" % (name, len(sites)))
        by_use = collections.Counter()
        actions = sorted({s[6] for s in sites})
        for s in sites:
            use = s[3] if s[3] != "sink" else "sink:%s:%s" % (s[4], s[5])
            by_use[(use, s[6])] += 1
        uses = sorted({u for u, _ in by_use}, key=lambda u: -sum(by_use[(u, a)] for a in actions))
        print("  %-28s %s" % ("use", " ".join("%8s" % a for a in actions)))
        for u in uses:
            print("  %-28s %s" % (u, " ".join("%8d" % by_use[(u, a)] for a in actions)))
        by_set = collections.defaultdict(lambda: [0, set()])
        prog_mix = collections.Counter()
        per_prog = collections.defaultdict(set)
        for s in sites:
            st = set_of(s[1])
            by_set[st][0] += 1
            by_set[st][1].add(s[1])
            per_prog[s[1]].add(s[6])
        print("  sites / programs by set:")
        for st, (n, progs) in sorted(by_set.items()):
            print("    %-10s %5d sites in %4d programs" % (st, n, len(progs)))
        for prog, acts in per_prog.items():
            prog_mix[(set_of(prog), ",".join(sorted(acts)))] += 1
        print("  programs by action mix:")
        for (st, mix), n in sorted(prog_mix.items()):
            print("    %-10s %-24s %4d" % (st, mix, n))
    return 0


if __name__ == "__main__":
    sys.exit(main())
