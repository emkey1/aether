#!/usr/bin/env python3
"""Stamp an edited Aether guide, idempotently, and record the stamp.

Scheme: ``YYYY-MM-DD-N``
  * ``N`` is 1 for the first revision of a given day,
  * incremented for each further revision on the **same** day,
  * reset to 1 when the date rolls over.

Run it on every guide whose text you changed, before committing:

    python3 tools/bump_guide_version.py docs/aether_for_llms_medium_contexts.md

The next stamp is computed from the stamp in ``git show HEAD:<guide>``, never
from the working file, so running the tool twice (or after an earlier run)
bumps once: if the working-tree stamp already differs from HEAD's, the stamp is
left alone. Medium is stamped exactly like small and full.

Each run also writes the guide's entry for its current stamp into
docs/guide_stamps.json -- the body sha256 (the guide's bytes with the stamp
line removed), the whole-document o200k count when tiktoken is installed, and
the date -- replacing the entry if the stamp is already there, so further edits
before the commit only refresh it. Then it prints the changelog row to paste
into docs/aether_guide_changelog.md, with net tokens when tiktoken is present.
Commit the guide, the manifest and the row together: `ctest` (the
`aether_guide_stamps` test) fails until all three agree. The row's commit
column reads ``(this commit)``; the commit is derived from git when needed
(`tools/check_guide_stamps.py --commits`), so no follow-up fill-in commit.

A stamp without a text change is refused. Harness-only bumps are retired: the
benchmark MANIFEST's harness and prompt-template sha256s identify the harness,
so a guide stamp names guide text and nothing else.

If a guide has no stamp yet, one is inserted just below the title.
"""
import argparse
import datetime
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from check_guide_stamps import MANIFEST, STAMP_LINE, body_sha256, stamp_key  # noqa: E402

REPO = os.path.dirname(HERE)


def head_text(path):
    """The guide as committed at HEAD, or None if HEAD does not have it."""
    top = subprocess.run(["git", "-C", os.path.dirname(os.path.abspath(path)),
                          "rev-parse", "--show-toplevel"], capture_output=True, text=True)
    if top.returncode != 0:
        sys.exit(f"{path}: not inside a git checkout; the next stamp comes from HEAD")
    root = top.stdout.strip()
    rel = os.path.relpath(os.path.realpath(path), os.path.realpath(root))
    shown = subprocess.run(["git", "-C", root, "show", f"HEAD:{rel}"],
                           capture_output=True)
    if shown.returncode != 0:
        return root, rel, None
    return root, rel, shown.stdout.decode("utf-8")


def next_stamp(head_stamp, today):
    if head_stamp and head_stamp.startswith(today + "-"):
        return f"{today}-{int(head_stamp.rsplit('-', 1)[1]) + 1}"
    return f"{today}-1"


def set_stamp(text, stamp):
    line = f"*Guide version: {stamp}*"
    m = STAMP_LINE.search(text)
    if m:
        return text[:m.start()] + line + text[m.end():]
    lines, out, inserted = text.split("\n"), [], False
    for l in lines:
        out.append(l)
        if not inserted and l.startswith("# "):
            out.extend(["", line])
            inserted = True
    return "\n".join(out if inserted else [line, ""] + lines)


def o200k_counter():
    """Whole-document o200k counter, or None. Never downloads (see check_guide_tokens)."""
    from check_guide_tokens import find_o200k
    enc, _ = find_o200k()
    return (lambda s: len(enc.encode(s, disallowed_special=()))) if enc else None


def bump(path, today, count):
    with open(path, encoding="utf-8") as fh:
        text = fh.read()
    root, rel, old = head_text(path)
    old_stamp = STAMP_LINE.search(old).group(1) if old and STAMP_LINE.search(old) else None
    cur = STAMP_LINE.search(text)
    cur_stamp = cur.group(1) if cur else None

    if old is not None and body_sha256(old)[1] == body_sha256(text)[1]:
        hint = "" if cur_stamp == old_stamp else f"; restore HEAD's stamp {old_stamp}"
        print(f"{rel}: text unchanged since HEAD, so nothing to stamp (harness-only bumps "
              f"are retired){hint}", file=sys.stderr)
        return False

    if cur_stamp is not None and cur_stamp != old_stamp:
        stamp, action = cur_stamp, f"already stamped {cur_stamp} since HEAD; stamp left alone"
    else:
        stamp = next_stamp(old_stamp, today)
        text = set_stamp(text, stamp)
        with open(path, "w", encoding="utf-8") as fh:
            fh.write(text)
        action = f"{old_stamp or '(no stamp)'} -> {stamp}"

    stamp_, digest = body_sha256(text)
    tokens = count(text) if count else None
    manifest_path = os.path.join(root, MANIFEST)
    try:
        with open(manifest_path, encoding="utf-8") as fh:
            manifest = json.load(fh)
    except FileNotFoundError:
        manifest = {"guides": {}}
    entries = manifest["guides"].setdefault(rel, [])
    entry = {"stamp": stamp, "date": stamp.rsplit("-", 1)[0], "body_sha256": digest,
             "o200k": tokens}
    for i, e in enumerate(entries):
        if e["stamp"] == stamp:
            entries[i] = entry
            break
    else:
        if entries and stamp_key(entries[-1]["stamp"]) >= stamp_key(stamp):
            sys.exit(f"{rel}: {MANIFEST} already ends at {entries[-1]['stamp']}, "
                     f"not before {stamp}")
        entries.append(entry)
    with open(manifest_path, "w", encoding="utf-8") as fh:
        json.dump(manifest, fh, indent=2, ensure_ascii=False)
        fh.write("\n")

    print(f"{rel}: {action}; {MANIFEST} entry written")
    net = ""
    if count and old is not None:
        before = count(old)
        net = f" Net {tokens - before:+,} tokens: {before:,} → {tokens:,}."
    elif count:
        net = f" {tokens:,} tokens."
    print("changelog row (docs/aether_guide_changelog.md, top of this guide's table):")
    print(f"| `{stamp}` | {entry['date']} | (this commit) | <what changed>.{net} |")
    return True


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("guides", nargs="+", help="guide files to stamp")
    ap.add_argument("--today", default=datetime.date.today().isoformat(),
                    help=argparse.SUPPRESS)
    args = ap.parse_args()
    count = o200k_counter()
    ok = True
    for p in args.guides:
        ok = bump(p, args.today, count) and ok
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
