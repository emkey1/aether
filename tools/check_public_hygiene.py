#!/usr/bin/env python3
"""Fail if a tracked text file in a public repo carries host, fleet, credential or
session data.

emkey1/aether and emkey1/pscal are public. On 2026-10-06 a corpus golden was found
holding a full PATH with Claude session ids, and the umbrella's CLAUDE.md held
tailnet addresses, the ssh user's sudo rights and credential locations. This is the
net that keeps that class out: it scans every tracked text file (`git ls-files`)
under --root for the patterns below.

Known, accepted hits live in an allowlist (default: tools/public_hygiene_allowlist.txt
under --root), one entry per line:

    glob <pattern>            # reason       every check off for a path glob (vendored trees)
    glob <pattern> <check>    # reason       one check off for a path glob, the rest still apply
    <path> <line-sha1-12>     # reason       one line, keyed by its content

A line entry survives the line moving but not its text changing. `--baseline`
prints entries for every current hit, for seeding an allowlist on first adoption;
review them before committing.

Exit status: 0 clean, 1 unallowlisted hits, 77 not a git checkout (skipped).
"""

from __future__ import annotations

import argparse
import fnmatch
import hashlib
import pathlib
import re
import subprocess
import sys

PATTERNS: list[tuple[str, re.Pattern[str]]] = [
    ("home-path", re.compile(r"/Users/[A-Za-z0-9_.-]+/|/home/claw\b")),
    ("claude-session-dir", re.compile(r"local-agent-mode-sessions|claude-[0-9]+/-Users-")),
    ("session-uuid", re.compile(
        r"session.{0,40}[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}"
        r"|[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}.{0,40}session",
        re.IGNORECASE)),
    ("path-dump", re.compile(r"\bPATH\s*[=:]\s*/[^\s:\"']*(?::/[^\s:\"']*){2,}")),
    ("tailnet-name", re.compile(r"\.ts\.net\b")),
    ("tailnet-ip", re.compile(r"\b100\.(?:6[4-9]|[7-9][0-9]|1[01][0-9]|12[0-7])\.\d{1,3}\.\d{1,3}\b")),
    ("sudo-rights", re.compile(r"NOPASSWD|passwordless sudo", re.IGNORECASE)),
]
MAX_BYTES = 4 * 1024 * 1024


def line_key(text: str) -> str:
    return hashlib.sha1(text.strip().encode("utf-8", "replace")).hexdigest()[:12]


def load_allowlist(path: pathlib.Path) -> tuple[list[tuple[str, str | None]], set[tuple[str, str]]]:
    globs: list[tuple[str, str | None]] = []
    lines: set[tuple[str, str]] = set()
    names = {name for name, _ in PATTERNS}
    if not path.exists():
        return globs, lines
    for raw in path.read_text(encoding="utf-8").splitlines():
        entry = raw.split("#", 1)[0].strip()
        if not entry:
            continue
        parts = entry.split()
        if parts[0] == "glob" and len(parts) in (2, 3):
            if len(parts) == 3 and parts[2] not in names:
                raise SystemExit(f"unknown check {parts[2]!r} in {path}: {raw!r}")
            globs.append((parts[1], parts[2] if len(parts) == 3 else None))
        elif len(parts) == 2:
            lines.add((parts[0], parts[1]))
        else:
            raise SystemExit(f"bad allowlist entry in {path}: {raw!r}")
    return globs, lines


def tracked_files(root: pathlib.Path) -> list[str]:
    out = subprocess.run(["git", "-C", str(root), "ls-files", "-z"],
                         capture_output=True, check=True).stdout
    return [p for p in out.decode("utf-8", "replace").split("\0") if p]


def scan(root: pathlib.Path, globs: list[tuple[str, str | None]], allowed: set[tuple[str, str]]):
    hits = []
    for rel in tracked_files(root):
        off = {check for g, check in globs if fnmatch.fnmatch(rel, g)}
        if None in off:
            continue
        path = root / rel
        if not path.is_file() or path.stat().st_size > MAX_BYTES:
            continue
        data = path.read_bytes()
        if b"\0" in data[:8192]:
            continue  # binary
        for lineno, text in enumerate(data.decode("utf-8", "replace").splitlines(), 1):
            for name, pattern in PATTERNS:
                if name not in off and pattern.search(text):
                    if (rel, line_key(text)) not in allowed:
                        hits.append((rel, lineno, name, text))
                    break
    return hits


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    ap.add_argument("--root", type=pathlib.Path, default=pathlib.Path("."),
                    help="repository to scan (default: current directory)")
    ap.add_argument("--allowlist", type=pathlib.Path,
                    help="allowlist file (default: <root>/tools/public_hygiene_allowlist.txt)")
    ap.add_argument("--baseline", action="store_true",
                    help="print allowlist entries for every current hit instead of failing")
    args = ap.parse_args()
    root = args.root.resolve()
    allowlist = args.allowlist or root / "tools" / "public_hygiene_allowlist.txt"
    globs, allowed = load_allowlist(allowlist)
    try:
        hits = scan(root, globs, allowed)
    except (subprocess.CalledProcessError, FileNotFoundError):
        print(f"public hygiene: {root} is not a git checkout; skipped")
        return 77  # CTest SKIP_RETURN_CODE
    if args.baseline:
        for rel, lineno, name, text in hits:
            print(f"{rel} {line_key(text)}  # {name}, line {lineno} at adoption")
        return 0
    if not hits:
        print(f"public hygiene: clean ({root.name})")
        return 0
    print(f"public hygiene: {len(hits)} line(s) carry host, fleet, credential or "
          f"session data ({root.name}):", file=sys.stderr)
    for rel, lineno, name, text in hits:
        print(f"  {rel}:{lineno}: [{name}] {text.strip()[:160]}", file=sys.stderr)
    print(f"Remove the data, or (if it is genuinely public) allowlist the line in "
          f"{allowlist} as `<path> {{sha1-12}}  # reason`; --baseline prints entries.",
          file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
