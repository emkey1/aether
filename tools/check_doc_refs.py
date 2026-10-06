#!/usr/bin/env python3
"""Check that every commit a doc cites resolves, and that no placeholder ships.

Run by CTest as `aether_doc_refs`. Hashes typed from memory ship wrong: a pin
`c56c942bb17c` that never existed, a placeholder hash, and seven references
broken by the 2026-10-06 rewrite of the umbrella's history, all found by hand.

What counts as a commit reference: a 7-40 character lowercase hex token that is
backticked on its own (`abc1234`), or that sits near commit vocabulary on its
line (commit, pin, sha, gitlink, "fixed in", "->", "→", an `a..b` range). A
token right after sha256 / sha-256 / md5 / blob / hash of is a content hash and
is ignored, as is anything in the allowlist (tools/check_doc_refs.allowlist).
Each reference must name a commit (`git cat-file --batch-check`, `<h>^{commit}`)
in aether, external/rea or external/pscal-core, or in the umbrella when
PSCAL_UMBRELLA points at a checkout of it. Without PSCAL_UMBRELLA, an
umbrella-style reference (its line mentions the umbrella, `pscal` or `Tests/`,
or it is an 8-10 character abbreviation on a line naming none of aether, rea
and pscal-core -- git abbreviates the umbrella's larger history to that length,
the others to 7) is counted as skipped, not failed.

Placeholders fail outside fenced code: `<sha>`, `<hash>`, `<commit>`, `<rev>`,
`XXXXXXX` (7+ X), and `TBD` on a line about commits or pins.

A shallow aether checkout cannot answer the question, so the run is skipped
(exit 77) there. When rea or pscal-core is missing or shallow, a reference that
resolves nowhere is counted as unverifiable rather than failed. CI should check
out with full history (and the submodules) for this test to mean anything.

Usage:
    python3 tools/check_doc_refs.py [--root DIR] [--glob PATTERN ...]
                                    [--repo NAME=DIR ...]
Environment: PSCAL_UMBRELLA, AETHER_REA_DIR, AETHER_PSCAL_CORE_DIR.
Exit status: 0 clean, 1 unresolved references or placeholders, 2 usage
error, 77 no usable git history.
"""
import argparse
import glob
import os
import re
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_GLOBS = ("README.md", "CHANGELOG.md", "docs/**/*.md")
ALLOWLIST = os.path.join(os.path.dirname(os.path.abspath(__file__)), "check_doc_refs.allowlist")

HEX = re.compile(r"[0-9a-f]{7,40}")
VOCAB = re.compile(r"\b(?:commits?|committed|pin|pins|pinned|pinning|sha|shas|SHA|SHAs|"
                   r"gitlink|bisect(?:ed)?|cherry-pick(?:ed)?|revert(?:ed|s)?|fixed in|"
                   r"landed in|introduced in)\b|->|→", re.I)
CONTENT_HASH = re.compile(r"(?:sha-?256|sha-?512|sha1 of|md5|blob|hash of|digest)\W{0,6}$", re.I)
UMBRELLA_CONTEXT = re.compile(r"\bumbrella\b|\bpscal\b(?!-core)|\bTests/|\bcomponents/|\bPBuild\b", re.I)
LOCAL_CONTEXT = re.compile(r"\baether\b|\brea\b|\bpscal-core\b", re.I)
PLACEHOLDER = re.compile(r"<(?:sha1?|hash|commit|rev)>|\b[Xx]{7,}\b", re.I)
TBD = re.compile(r"\bTBD\b")
FENCE = re.compile(r"^\s*```")


def git_ok(path):
    try:
        r = subprocess.run(["git", "-C", path, "rev-parse", "--git-dir"],
                           capture_output=True, text=True, timeout=30)
    except (OSError, subprocess.TimeoutExpired):
        return False
    return r.returncode == 0


def own_checkout(path):
    """True if path is the top of its own git checkout (not a dir inside another)."""
    if not os.path.isdir(path):
        return False
    try:
        r = subprocess.run(["git", "-C", path, "rev-parse", "--show-toplevel"],
                           capture_output=True, text=True, timeout=30)
    except (OSError, subprocess.TimeoutExpired):
        return False
    return r.returncode == 0 and os.path.realpath(r.stdout.strip()) == os.path.realpath(path)


def is_shallow(path):
    r = subprocess.run(["git", "-C", path, "rev-parse", "--is-shallow-repository"],
                       capture_output=True, text=True, timeout=30)
    return r.stdout.strip() == "true"


def resolve(path, hashes):
    """{hash: True|False|'ambiguous'} for commit-ness in the repo at path."""
    if not hashes:
        return {}
    inp = "".join(f"{h}^{{commit}}\n" for h in hashes)
    r = subprocess.run(["git", "-C", path, "cat-file", "--batch-check"],
                       input=inp, capture_output=True, text=True, timeout=120)
    out = {}
    for h, line in zip(hashes, r.stdout.splitlines()):
        if line.endswith(" missing"):
            out[h] = False
        elif line.endswith(" ambiguous"):
            out[h] = "ambiguous"
        else:
            out[h] = " commit " in line
    return out


def load_allowlist(path):
    allowed = set()
    try:
        with open(path, encoding="utf-8") as fh:
            for line in fh:
                tok = line.split("#", 1)[0].strip()
                if tok:
                    allowed.add(tok)
    except FileNotFoundError:
        pass
    return allowed


def candidates(text):
    """Yield (line_no, token, line) for every commit-like reference."""
    for no, line in enumerate(text.split("\n"), 1):
        for m in HEX.finditer(line):
            tok, s, e = m.group(0), m.start(), m.end()
            before = line[s - 1] if s else ""
            after = line[e] if e < len(line) else ""
            if before and (before.isalnum() or before in "_-/") or (
                    before == "." and line[max(0, s - 2):s] != ".."):
                continue
            if after and (after.isalnum() or after in "_-"):
                continue
            if tok.isdigit() or not re.search(r"\d", tok) or not re.search(r"[a-f]", tok):
                continue
            if CONTENT_HASH.search(line[:s]):
                continue
            backticked = before == "`" and after == "`"
            window = line[max(0, s - 60):e + 20]
            ranged = line[max(0, s - 2):s] == ".." or line[e:e + 2] == ".."
            if backticked or ranged or VOCAB.search(window):
                yield no, tok, line


def main():
    ap = argparse.ArgumentParser(description="Check commit references in docs.")
    ap.add_argument("--root", default=REPO, help="repository whose docs are checked")
    ap.add_argument("--glob", action="append", dest="globs",
                    help=f"files to scan, relative to --root (default: {', '.join(DEFAULT_GLOBS)})")
    ap.add_argument("--repo", action="append", default=[], metavar="NAME=DIR",
                    help="an extra repository a reference may resolve in")
    ap.add_argument("--allowlist", default=ALLOWLIST)
    args = ap.parse_args()
    root = os.path.abspath(args.root)

    if not git_ok(root):
        print(f"SKIP: {root} is not a git checkout; commit references cannot be resolved")
        return 77
    if is_shallow(root):
        print(f"SKIP: {root} is a shallow clone; fetch full history to check references")
        return 77

    repos = [("aether", root),
             ("rea", os.environ.get("AETHER_REA_DIR") or os.path.join(root, "external", "rea")),
             ("pscal-core", os.environ.get("AETHER_PSCAL_CORE_DIR")
              or os.path.join(root, "external", "pscal-core"))]
    umbrella = os.environ.get("PSCAL_UMBRELLA")
    if umbrella:
        repos.append(("umbrella", umbrella))
    for spec in args.repo:
        name, _, path = spec.partition("=")
        if not path:
            print(f"ERROR: --repo wants NAME=DIR, got {spec!r}")
            return 2
        repos.append((name, path))
    usable, incomplete, notes = [], [], []
    for name, path in repos:
        if name == "aether" or own_checkout(path):
            usable.append((name, path))
            if is_shallow(path):
                incomplete.append(f"{name} (shallow)")
        else:
            incomplete.append(f"{name} (missing)")
            notes.append(f"{name}: no git checkout at {path}; pass AETHER_REA_DIR / "
                         "AETHER_PSCAL_CORE_DIR or check out the submodules")
    have_umbrella = any(n == "umbrella" for n, _ in usable)
    if umbrella and not have_umbrella:
        print(f"ERROR: PSCAL_UMBRELLA={umbrella} is not a git checkout")
        return 2

    allowed = load_allowlist(args.allowlist)
    files = []
    for pattern in args.globs or DEFAULT_GLOBS:
        files.extend(sorted(glob.glob(os.path.join(root, pattern), recursive=True)))
    refs, placeholders = [], []
    for path in dict.fromkeys(files):
        rel = os.path.relpath(path, root)
        with open(path, encoding="utf-8") as fh:
            text = fh.read()
        in_fence = False
        for no, line in enumerate(text.split("\n"), 1):
            if FENCE.match(line):
                in_fence = not in_fence
            if in_fence:  # a usage template such as `git show <sha>` is not a placeholder
                continue
            for m in PLACEHOLDER.finditer(line):
                placeholders.append((rel, no, m.group(0), line))
            if TBD.search(line) and VOCAB.search(line):
                placeholders.append((rel, no, "TBD", line))
        for no, tok, line in candidates(text):
            if tok not in allowed:
                refs.append((rel, no, tok, line))

    tokens = sorted({t for _, _, t, _ in refs})
    found = {t: [] for t in tokens}
    ambiguous = set()
    for name, path in usable:
        for t, ok in resolve(path, tokens).items():
            if ok is True:
                found[t].append(name)
            elif ok == "ambiguous":
                ambiguous.add(t)

    unresolved, skipped, unverifiable = [], [], []
    for rel, no, tok, line in refs:
        if found[tok] or tok in ambiguous:
            continue
        umbrella_style = bool(UMBRELLA_CONTEXT.search(line)) or (
            8 <= len(tok) <= 10 and not LOCAL_CONTEXT.search(line))
        # The repos this reference could live in, by what its line names.
        could_be = [n for n, rx in (("pscal-core", r"\bpscal-core\b"), ("rea", r"\brea\b"))
                    if re.search(rx, line, re.I)] or ["rea", "pscal-core"]
        if umbrella_style and not have_umbrella:
            skipped.append((rel, no, tok, line))
        elif any(c.startswith(n + " ") for c in incomplete for n in could_be):
            unverifiable.append((rel, no, tok, line))
        else:
            unresolved.append((rel, no, tok, line))

    resolved = sum(1 for _, _, t, _ in refs if found[t])
    print(f"doc commit references: {len(refs)} in {len(set(r[0] for r in refs))} files "
          f"({len(tokens)} distinct), searched {', '.join(n for n, _ in usable)}")
    print(f"  resolved {resolved}, unresolved {len(unresolved)}, umbrella-style skipped "
          f"{len(skipped)}{' (set PSCAL_UMBRELLA to check them)' if skipped else ''}, "
          f"unverifiable {len(unverifiable)}, placeholders {len(placeholders)}")
    for n in notes:
        print(f"note: {n}")
    if incomplete:
        print(f"note: incomplete history: {', '.join(incomplete)}; references that resolve "
              "nowhere are counted as unverifiable, not failures")
    for t in sorted(ambiguous):
        print(f"note: {t} is an ambiguous abbreviation; cite more characters")
    for rel, no, tok, line in skipped:
        print(f"  skipped     {rel}:{no}: {tok}")
    for rel, no, tok, line in unverifiable:
        print(f"  unverifiable {rel}:{no}: {tok}")
    for rel, no, tok, line in unresolved:
        print(f"UNRESOLVED  {rel}:{no}: `{tok}` is not a commit in any searched repo: "
              f"{line.strip()[:160]}")
    for rel, no, tok, line in placeholders:
        print(f"PLACEHOLDER {rel}:{no}: {tok!r}: {line.strip()[:160]}")
    return 1 if unresolved or placeholders else 0


if __name__ == "__main__":
    sys.exit(main())
