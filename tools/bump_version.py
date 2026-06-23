#!/usr/bin/env python3
"""Bump the Aether *language* version in components/aether/VERSION.

Scheme: ``YYYY-MM-DD-N`` (the same stamp the guides use) --
  * ``N`` is 1 for the first language change on a given day,
  * incremented for each further language change the **same** day,
  * reset to 1 when the date rolls over.

Run this ONCE per language-affecting change -- a syntax, semantic, or builtin
change that alters what programs compile or how they behave -- before committing,
and add a matching entry to CHANGELOG.md. Do NOT run it on a plain rebuild: the
date is meant to be the *last language-change* date, not a build timestamp, which
is what makes node version-skew self-evident (a stale node reports an old date).

The value is checked in, so every node that builds the same commit reports the
same version via ``aether --version``.

Usage:
    python3 tools/bump_version.py          # bump VERSION to today
    python3 tools/bump_version.py --show    # print current version, no change
"""
import sys
import re
import datetime
import pathlib

VERSION_FILE = pathlib.Path(__file__).resolve().parent.parent / "VERSION"
PAT = re.compile(r"^(\d{4}-\d{2}-\d{2})-(\d+)\s*$")


def read() -> str:
    if VERSION_FILE.exists():
        return VERSION_FILE.read_text(encoding="utf-8").strip()
    return ""


def bump() -> str:
    today = datetime.date.today().isoformat()
    m = PAT.match(read())
    newn = int(m.group(2)) + 1 if (m and m.group(1) == today) else 1
    ver = f"{today}-{newn}"
    VERSION_FILE.write_text(ver + "\n", encoding="utf-8")
    return ver


if __name__ == "__main__":
    if "--show" in sys.argv:
        print(read() or "(unset)")
    else:
        old = read()
        print(f"{old or '(unset)'} -> {bump()}")
