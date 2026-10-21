#!/usr/bin/env python3
"""D78 evidence: which committed programs hold a `\\u` / `\\U` Text escape.

Counts, over every Aether source the repo keeps (tests, examples, the guides'
and spec's code, training pairs and datasets, archived benchmark attempts), the
escapes whose meaning D78 changes:

  full   `\\u` + 4 hex digits or `\\U` + 8: kept as written before, decoded now
         (or SYN-001 when the value is a lone surrogate or past U+10FFFF)
  brace  `\\u{...}` (the Rust / JavaScript spelling): unchanged, kept as written

A program's source sits in a plain file (.aether, .md) or inside JSON, where
its backslash is doubled (.json, .jsonl, .gz). Run from the repo root:

  python3 results/decisions/d78_unicode_escape_census.py
"""
import gzip
import os
import re
import sys

ROOTS = ["tests", "examples", "docs", "training", "bench", "results"]
PLAIN = (".aether", ".md", ".out")
JSON = (".json", ".jsonl", ".gz")

# One backslash in a plain file, two inside a JSON string; never preceded by
# another backslash (`\\u00e9` in source is a literal backslash and `u00e9`).
def patterns(bs):
    pre = rb"(?<!\\)" + bs
    return {
        "full": re.compile(pre + rb"(?:u[0-9a-fA-F]{4}|U[0-9a-fA-F]{8})"),
        "brace": re.compile(pre + rb"u\{"),
    }

PLAIN_PAT = patterns(rb"\\")
JSON_PAT = patterns(rb"\\\\")


def main():
    totals = {"full": 0, "brace": 0}
    scanned = 0
    for top in ROOTS:
        for root, _, files in os.walk(top):
            for name in files:
                path = os.path.join(root, name)
                if name.endswith(PLAIN):
                    pats = PLAIN_PAT
                elif name.endswith(JSON):
                    pats = JSON_PAT
                else:
                    continue
                try:
                    with open(path, "rb") as fh:
                        data = fh.read()
                    if name.endswith(".gz"):
                        data = gzip.decompress(data)
                except (OSError, EOFError, gzip.BadGzipFile):
                    continue
                scanned += 1
                for kind, pat in pats.items():
                    hits = pat.findall(data)
                    if hits:
                        totals[kind] += len(hits)
                        print(f"{kind}\t{path}\t{len(hits)}\t{hits[:3]}")
    print(f"files scanned: {scanned}; full escapes: {totals['full']}; "
          f"brace escapes: {totals['brace']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
