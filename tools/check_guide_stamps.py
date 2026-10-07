#!/usr/bin/env python3
"""Check that every guide's text is named by its stamp, and VERSION by CHANGELOG.

"Guide" here covers the three LLM-facing guides and the prior-alignment card
(docs/aether_card.md), whose stamp line reads `*Card version: ...*` instead.

Run by CTest as `aether_guide_stamps`. Benchmark rows are attributed by guide
stamp, so a guide whose text changes under an unchanged stamp produces scores
nothing can be traced back to. This verifies instead of mutating:

 1. Each guide's body hash (the sha256 of its UTF-8 bytes with the one
    `*Guide version: ...*` line removed) equals the docs/guide_stamps.json
    entry for its current stamp, and that stamp is the manifest's newest
    entry for the guide.
 2. Every manifest stamp has a row in docs/aether_guide_changelog.md, and the
    current stamp is the first (newest) row of its guide's section. A row for
    a manifest stamp names its commit as `(this commit)` -- it lands in the
    same commit as the stamp -- or as the commit that introduced the stamp,
    which is derived from git, never typed (owner decision D28).
 3. No two consecutive manifest entries share a body hash. A stamp bump with
    no text change was a "harness-only" bump; those are retired (D41): the
    benchmark MANIFEST's harness and prompt-template sha256s identify the
    harness now. Older changelog rows that say "harness" keep their meaning.
 4. VERSION equals the version of the top `## YYYY-MM-DD-N` entry in
    CHANGELOG.md.

Fix a failure with `python3 tools/bump_guide_version.py <guide>`, which stamps
the guide from HEAD's stamp (idempotently), updates the manifest and prints the
changelog row to paste.

Usage:
    python3 tools/check_guide_stamps.py [--root DIR]
    python3 tools/check_guide_stamps.py --commits   # stamp -> introducing commit
Exit status: 0 consistent, 1 a check failed, 2 usage or I/O error.
"""
import argparse
import hashlib
import json
import os
import re
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

GUIDES = (
    "docs/aether_for_llms_and_others.md",
    "docs/aether_for_llms_medium_contexts.md",
    "docs/aether_for_llms_with_small_contexts.md",
    "docs/aether_card.md",
)
CARD = "aether_card.md"
MANIFEST = "docs/guide_stamps.json"
GUIDE_CHANGELOG = "docs/aether_guide_changelog.md"

STAMP_LINE = re.compile(r"^\*(?:Guide|Card) version:\s*(\d{4}-\d{2}-\d{2}-\d+)\*[ \t]*$", re.M)
STAMP_VALUE = re.compile(r"^(\d{4})-(\d{2})-(\d{2})-(\d+)$")
ROW = re.compile(r"^\|\s*`(\d{4}-\d{2}-\d{2}-\d+)`\s*\|\s*([^|]*)\|\s*([^|]*)\|")
SECTION = re.compile(r"^##\s.*`([^`]+\.md)`")
LANG_ENTRY = re.compile(r"^##\s+(\d{4}-\d{2}-\d{2}-\d+)\s*$")
THIS_COMMIT = "(this commit)"


def stamp_label(rel):
    """The word before 'version:' in a document's stamp line."""
    return "Card" if os.path.basename(rel) == CARD else "Guide"


def split_stamp(text):
    """Return (stamp, body_bytes). body excludes the stamp line and its newline."""
    matches = list(STAMP_LINE.finditer(text))
    if len(matches) != 1:
        return None, None
    m = matches[0]
    end = m.end()
    if text[end:end + 1] == "\n":
        end += 1
    return m.group(1), (text[:m.start()] + text[end:]).encode("utf-8")


def body_sha256(text):
    stamp, body = split_stamp(text)
    if stamp is None:
        return None, None
    return stamp, hashlib.sha256(body).hexdigest()


def stamp_key(stamp):
    m = STAMP_VALUE.match(stamp or "")
    return (m.group(1), m.group(2), m.group(3), int(m.group(4))) if m else None


def read(root, rel):
    with open(os.path.join(root, rel), encoding="utf-8") as fh:
        return fh.read()


def changelog_sections(text):
    """{guide basename: [(stamp, commit cell), ...]} in file order (newest first)."""
    sections, current = {}, None
    for line in text.split("\n"):
        sec = SECTION.match(line)
        if sec:
            current = sections.setdefault(sec.group(1), [])
            continue
        row = ROW.match(line)
        if row and current is not None:
            current.append((row.group(1), row.group(3).strip()))
    return sections


def top_language_entry(text):
    in_fence = False
    for line in text.split("\n"):
        if line.lstrip().startswith("```"):
            in_fence = not in_fence
            continue
        if not in_fence:
            m = LANG_ENTRY.match(line)
            if m:
                return m.group(1)
    return None


def git(root, *args):
    try:
        r = subprocess.run(["git", "-C", root] + list(args), capture_output=True,
                           text=True, timeout=60)
    except (OSError, subprocess.TimeoutExpired):
        return None
    return r.stdout if r.returncode == 0 else None


def history_available(root):
    if git(root, "rev-parse", "--git-dir") is None:
        return False
    return (git(root, "rev-parse", "--is-shallow-repository") or "").strip() != "true"


def introducing_commit(root, rel, stamp):
    """Oldest commit whose diff changes the count of this stamp line, or None."""
    out = git(root, "log", "--format=%H", "--reverse", "-S",
              f"*{stamp_label(rel)} version: {stamp}*", "--", rel)
    if not out:
        return None
    return out.split()[0]


def main():
    ap = argparse.ArgumentParser(description="Check guide stamps, manifest and changelogs.")
    ap.add_argument("--root", default=REPO, help="repository root to check (default: this repo)")
    ap.add_argument("--commits", action="store_true",
                    help="print each manifest stamp's introducing commit and exit")
    args = ap.parse_args()
    root = args.root

    try:
        manifest = json.loads(read(root, MANIFEST))
        entries_by_guide = manifest["guides"]
        changelog = changelog_sections(read(root, GUIDE_CHANGELOG))
        version = read(root, "VERSION").strip()
        lang_top = top_language_entry(read(root, "CHANGELOG.md"))
    except (OSError, ValueError, KeyError) as exc:
        print(f"ERROR: {exc}")
        return 2

    have_history = history_available(root)

    if args.commits:
        for rel in sorted(entries_by_guide):
            for e in entries_by_guide[rel]:
                c = introducing_commit(root, rel, e["stamp"]) if have_history else None
                print(f"{rel}  {e['stamp']}  {c[:7] if c else '(not committed / no history)'}")
        return 0

    failures, notes = [], []

    def fail(msg):
        failures.append(msg)

    guides = list(GUIDES) + sorted(g for g in entries_by_guide if g not in GUIDES)
    for rel in guides:
        name = os.path.basename(rel)
        entries = entries_by_guide.get(rel)
        if not entries:
            fail(f"{rel}: no entries in {MANIFEST}")
            continue
        try:
            text = read(root, rel)
        except OSError as exc:
            fail(f"{rel}: cannot read: {exc}")
            continue
        stamp, digest = body_sha256(text)
        if stamp is None:
            fail(f"{rel}: needs exactly one '*{stamp_label(rel)} version: YYYY-MM-DD-N*' line")
            continue

        # 3. stamps strictly increase; consecutive bodies differ.
        for prev, cur in zip(entries, entries[1:]):
            if not (stamp_key(prev["stamp"]) and stamp_key(cur["stamp"])
                    and stamp_key(prev["stamp"]) < stamp_key(cur["stamp"])):
                fail(f"{rel}: manifest stamps out of order: {prev['stamp']} then {cur['stamp']}")
            if prev["body_sha256"] == cur["body_sha256"]:
                fail(f"{rel}: {cur['stamp']} has the same text as {prev['stamp']}. Harness-only "
                     "stamp bumps are retired (D41): the benchmark MANIFEST's harness and "
                     "prompt-template sha256s identify the harness; drop the entry and the bump")

        # 1. the text is the text its stamp names.
        by_stamp = {e["stamp"]: e for e in entries}
        newest = entries[-1]["stamp"]
        if stamp not in by_stamp:
            fail(f"{rel}: stamp {stamp} has no entry in {MANIFEST}; stamp guides with "
                 f"`python3 tools/bump_guide_version.py {rel}`")
        else:
            if stamp != newest:
                fail(f"{rel}: stamp {stamp} is older than the manifest's newest entry {newest}")
            if by_stamp[stamp]["body_sha256"] != digest:
                fail(f"{rel}: text changed under stamp {stamp} (body sha256 {digest[:12]}, "
                     f"manifest {by_stamp[stamp]['body_sha256'][:12]}). Run "
                     f"`python3 tools/bump_guide_version.py {rel}` and add the changelog row "
                     "it prints, in the same commit")

        # 2. every manifest stamp has a changelog row; the current one is newest.
        rows = changelog.get(name)
        if rows is None:
            fail(f"{rel}: {GUIDE_CHANGELOG} has no section naming `{name}`")
            continue
        cells = {}
        for s, cell in rows:
            cells.setdefault(s, cell)
        for e in entries:
            s = e["stamp"]
            if s not in cells:
                fail(f"{rel}: stamp {s} has no row in {GUIDE_CHANGELOG}; add the row "
                     "bump_guide_version.py printed")
                continue
            cell = cells[s]
            if cell == THIS_COMMIT:
                continue
            if not have_history:
                notes.append(f"{rel} {s}: commit column not derived (no git history here)")
                continue
            derived = introducing_commit(root, rel, s)
            hashes = re.findall(r"\b[0-9a-f]{7,40}\b", cell)
            if derived is None:
                fail(f"{rel}: row {s} names {cell!r} but the stamp is not committed yet; "
                     f"write {THIS_COMMIT!r}")
            elif not any(derived.startswith(h) for h in hashes):
                fail(f"{rel}: row {s} names {cell!r}, but the stamp was introduced by "
                     f"{derived[:7]}; write {THIS_COMMIT!r} (the column is derived, not typed)")
        if rows and rows[0][0] != stamp:
            fail(f"{rel}: the newest {GUIDE_CHANGELOG} row is {rows[0][0]}, not the current "
                 f"stamp {stamp} (rows are newest first)")

    # 4. the language version and its changelog agree.
    if not STAMP_VALUE.match(version):
        fail(f"VERSION: {version!r} is not YYYY-MM-DD-N")
    elif lang_top != version:
        fail(f"VERSION is {version}, but the top CHANGELOG.md entry is {lang_top}; every "
             f"VERSION bump adds its `## {version}` entry in the same commit")

    for rel in guides:
        entries = entries_by_guide.get(rel) or []
        if entries:
            print(f"  {rel}: {entries[-1]['stamp']} ({len(entries)} manifest "
                  f"entr{'y' if len(entries) == 1 else 'ies'})")
    print(f"  VERSION {version}; top CHANGELOG.md entry {lang_top}")
    for n in notes:
        print(f"note: {n}")
    for f in failures:
        print(f"FAIL: {f}")
    if failures:
        return 1
    print("guide stamps consistent")
    return 0


if __name__ == "__main__":
    sys.exit(main())
