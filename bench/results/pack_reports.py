#!/usr/bin/env python3
"""Make harness reports committable: strip host and path detail, then gzip.

    python3 bench/results/pack_reports.py RESULTS_DIR [RESULTS_DIR ...] [--check]
    python3 bench/results/pack_reports.py --rescrub FILE_OR_DIR [...] [--check]

This repo is public. A raw report carries the bench host's home path, temp
paths, and the base_url of the destination it called (a tailnet or LAN address
for the local boards, a device address for the phone). For every unit report
`*.json` under a results directory (board directories, their infra_pending/, and
any other directory except logs/, rejected/, aborted_*/, smoke/ and
superseded_*/, which stay out of git) this writes `<name>.json.gz` with those
replaced by placeholders and removes the `.json`. The replacement is textual and
the result is re-parsed, so a report that no longer loads is an error. Nothing
that is scored is touched: prompts, model output, compiler output and verdicts
contain none of these strings unless a model echoed one.

The fleet rules (FLEET_RULES) also neutralise which machines ran a board: host
names become stable aliases (host-a, host-b, ...) and machine-specific product
names give way to neutral wording. The rules themselves name those machines, so
they live outside the repo (see _private_fleet_rules). scrub_fleet() is the same
replacement for any text: notes, scripts, docs. File names go through it too.

--rescrub applies the rules again to files already packed or committed (a
`.json.gz` is rewritten in place, deterministically: mtime 0; any other text
file likewise), for a rule added after they were packed.

--check changes nothing and lists what would be replaced, and anything that still
looks private afterwards.
"""
from __future__ import annotations

import argparse
import collections
import gzip
import json
import pathlib
import re
import sys
from typing import Callable

SKIP_DIRS = {"logs", "rejected", "smoke"}
SKIP_PREFIXES = ("aborted", "superseded")
KEEP_PLAIN = {"summary.json", "MANIFEST.json", "panel_weights.json"}

RULES: list[tuple[str, re.Pattern[str], str]] = [
    ("home-path", re.compile(r"/Users/[A-Za-z0-9_.-]+/"), "~/"),
    ("temp-path", re.compile(r"(?:/private)?/var/folders/[A-Za-z0-9_]+/[A-Za-z0-9_]+/T/"), "<tmp>/"),
    ("tailnet-name", re.compile(r"[A-Za-z0-9-]+\.tail[0-9a-z]+\.ts\.net"), "<tailnet-host>"),
    ("tailnet-ip", re.compile(r"\b100\.(?:6[4-9]|[7-9][0-9]|1[01][0-9]|12[0-7])\.\d{1,3}\.\d{1,3}\b"), "<tailnet-ip>"),
    ("lan-ip", re.compile(r"\b(?:192\.168|169\.254)\.\d{1,3}\.\d{1,3}\b"), "<lan-ip>"),
    ("device-host", re.compile(r"\b[A-Za-z0-9-]+\.coredevice\.local\b"), "<device-bridge>"),
    ("device-host", re.compile(r"(?<=http://)[A-Za-z0-9-]+(?=:8799\b)"), "<device-bridge>"),
    ("local-host", re.compile(r"\b[A-Za-z0-9-]+\.local\b(?=[:/\"])"), "<host>.local"),
]

Repl = str | Callable[[re.Match[str]], str]


def _private_fleet_rules() -> list[tuple[str, re.Pattern[str], Repl]]:
    """Machine-specific scrub rules, kept out of this public repo.

    Loaded from $AETHER_FLEET_SCRUB or ~/.config/pscal/fleet_scrub.py, a module that
    defines FLEET_RULES in the same (name, pattern, replacement) shape. Without it
    no fleet rule runs, and --check says so."""
    import importlib.util
    import os
    path = pathlib.Path(os.environ.get("AETHER_FLEET_SCRUB", "~/.config/pscal/fleet_scrub.py")).expanduser()
    if not path.is_file():
        return []
    spec = importlib.util.spec_from_file_location("aether_private_fleet_scrub", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return list(getattr(module, "FLEET_RULES", []))


FLEET_RULES: list[tuple[str, re.Pattern[str], Repl]] = _private_fleet_rules()
# Anything matching these after the rules have run is reported, never silently kept.
RESIDUAL = re.compile(r"/Users/|\.ts\.net|\b100\.(?:6[4-9]|[7-9]\d|1[01]\d|12[0-7])\.\d+\.\d+\b|\b192\.168\.|"
                      r"sk-[A-Za-z0-9_-]{8,}|Bearer [A-Za-z0-9._-]{12,}|local-agent-mode-" + "sessions")


def reports(root: pathlib.Path):
    for path in sorted(root.rglob("*.json")):
        rel = path.relative_to(root)
        if any(part in SKIP_DIRS or part.startswith(SKIP_PREFIXES) for part in rel.parts[:-1]):
            continue
        # Unit reports are named <destination>__<suite>...json; anything else (frozen task
        # files, summaries, manifests) is not a report and is left alone.
        if path.name in KEEP_PLAIN or "__" not in path.name:
            continue
        yield path


def scrub_fleet(text: str) -> tuple[str, collections.Counter]:
    seen: collections.Counter = collections.Counter()
    for name, pattern, repl in FLEET_RULES:
        text, n = pattern.subn(repl, text)
        if n:
            seen[name] += n
    return text, seen


def scrub(text: str) -> tuple[str, collections.Counter]:
    seen: collections.Counter = collections.Counter()
    for name, pattern, repl in RULES:
        text, n = pattern.subn(repl, text)
        if n:
            seen[name] += n
    text, fleet = scrub_fleet(text)
    seen.update(fleet)
    return text, seen


def write_gz(path: pathlib.Path, text: str) -> None:
    with gzip.GzipFile(filename="", fileobj=open(path, "wb"), mode="wb", mtime=0) as out:
        out.write(text.encode("utf-8"))


def rescrub(paths: list[str], check: bool) -> int:
    """Re-apply every rule to files already packed (`.json.gz`) or committed as text."""
    total: collections.Counter = collections.Counter()
    changed = bad = 0
    files = []
    for p in paths:
        root = pathlib.Path(p)
        files += sorted(f for f in root.rglob("*") if f.is_file()) if root.is_dir() else [root]
    for path in files:
        raw = path.read_bytes()
        packed = path.suffix == ".gz"
        if packed:
            raw = gzip.decompress(raw)
        if b"\0" in raw[:4096]:
            continue
        try:
            before = raw.decode("utf-8")
        except UnicodeDecodeError:
            continue
        text, seen = scrub(before)
        if text == before:
            continue
        if path.name.endswith((".json", ".json.gz")):
            try:
                json.loads(text)
            except ValueError as exc:
                print(f"NOT VALID JSON after scrub: {path}: {exc}")
                bad += 1
                continue
        total.update(seen)
        changed += 1
        if not check:
            if packed:
                write_gz(path, text)
            else:
                path.write_text(text, encoding="utf-8")
    print(f"{'would rescrub' if check else 'rescrubbed'} {changed} file(s); replacements: {dict(total)}; "
          f"problems: {bad}")
    return 1 if bad else 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dirs", nargs="+")
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--rescrub", action="store_true", help="re-apply the rules to packed and committed files")
    args = ap.parse_args()
    if args.rescrub:
        return rescrub(args.dirs, args.check)
    total: collections.Counter = collections.Counter()
    bad = packed = 0
    for d in args.dirs:
        root = pathlib.Path(d)
        for path in reports(root):
            raw = path.read_text(encoding="utf-8")
            text, seen = scrub(raw)
            total.update(seen)
            try:
                json.loads(text)
            except ValueError as exc:
                print(f"NOT VALID JSON after scrub: {path}: {exc}")
                bad += 1
                continue
            left = sorted(set(RESIDUAL.findall(text)))
            if left:
                print(f"residual in {path}: {left[:6]}")
                bad += 1
                continue
            packed += 1
            if not args.check:
                write_gz(pathlib.Path(str(path) + ".gz"), text)
                path.unlink()
    print(f"{'would pack' if args.check else 'packed'} {packed} report(s); replacements: {dict(total)}; problems: {bad}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
