#!/usr/bin/env python3
"""Stamp or bump the version line on an Aether guide.

Scheme: ``YYYY-MM-DD-N``
  * ``N`` is 1 for the first revision of a given day,
  * incremented for each further revision on the **same** day,
  * reset to 1 when the date rolls over.

Run this once per revision, before committing a guide edit. If a guide has no
stamp yet, it is inserted just below the title.

Also bump on a benchmark-harness change that could plausibly move a score
(e.g. tools/aether_doc_bench.py's repair-feedback logic), even with no edit to
the guide text itself — the "guide ver" column in
docs/aether_guided_benchmark.md is the only per-row version marker, so it
doubles as "what produced this score." Pair a harness-only bump with a dated
note in that doc's Status section explaining there's no corresponding guide
text diff, so it doesn't read as unexplained drift later.

Usage:
    python3 tools/bump_guide_version.py docs/aether_for_llms_and_others.md \\
                                        docs/aether_for_llms_with_small_contexts.md
"""
import sys
import re
import datetime

STAMP = re.compile(r"^\*Guide version:\s*(\d{4}-\d{2}-\d{2})-(\d+)\*\s*$", re.M)


def bump(path: str) -> str:
    today = datetime.date.today().isoformat()
    txt = open(path, encoding="utf-8").read()
    m = STAMP.search(txt)
    if m:
        date, n = m.group(1), int(m.group(2))
        newn = n + 1 if date == today else 1
        line = f"*Guide version: {today}-{newn}*"
        txt = txt[: m.start()] + line + txt[m.end():]
    else:
        newn = 1
        line = f"*Guide version: {today}-1*"
        lines, out, inserted = txt.split("\n"), [], False
        for l in lines:
            out.append(l)
            if not inserted and l.startswith("# "):
                out.extend(["", line])
                inserted = True
        if not inserted:
            out = [line, ""] + lines
        txt = "\n".join(out)
    open(path, "w", encoding="utf-8").write(txt)
    return f"{today}-{newn}"


if __name__ == "__main__":
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    for p in sys.argv[1:]:
        print(f"{p}: {bump(p)}")
