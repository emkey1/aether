#!/usr/bin/env python3
"""Check tests/surface/registry.json, Aether's surface registry.

Usage:
    python3 tools/check_surface_registry.py            # $AETHER_BIN, default build/aether
    python3 tools/check_surface_registry.py --strict-pending

The registry lists one entry per spelling with its synonym class (rationale
section 5.7, decision D7):

  1  accepted with exact meaning     fixture prints what its canonical twin prints
  2  coded rejection naming the form fixture fails to compile with exactly `code`,
                                     and the diagnostic text contains `hint`
  3  never accepted                  fixture fails to compile with exactly `code`
  4  tolerated, not taught           fixture prints what its canonical twin prints

"Prints what its twin prints" means the same stdout and the same exit status,
both compiled and run uncached. An entry whose status is "pending" has been
decided but not shipped: its `today` field pins what the fixture does now --
"accepted" (compiles and exits 0), "differs" (runs, but not as its twin),
"trap" (a run-time error), "uncoded" (rejected with an uncoded record) or the
exact code set it is rejected with. A pending entry whose outcome changes is
reported as FLIPPED and does not fail the run (unless --strict-pending): update
the entry to "holds" in the commit that ships it. Every entry's `decided` must
name a row in docs/aether_decisions.md.
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
AETHER = os.environ.get("AETHER_BIN", os.path.join(REPO, "build", "aether"))
SURFACE = os.path.join(REPO, "tests", "surface")
GUIDE_MODULES = os.path.join(REPO, "tests", "guide_modules")
DECISIONS = os.path.join(REPO, "docs", "aether_decisions.md")
FIELDS = {"id", "class", "spelling", "canonical_form", "fixture", "twin", "code", "hint",
          "decided", "status", "today", "owner", "note"}


def compile_records(workdir, path):
    r = subprocess.run([AETHER, "--no-cache", "--no-run", "--diagnostics-json", path],
                       capture_output=True, text=True, timeout=60, cwd=workdir)
    if r.returncode == 0:
        return 0, [], ""
    err = r.stderr
    start, end = err.find("[\n"), err.rfind("]")
    try:
        recs = json.loads(err[start:end + 1])
    except ValueError:
        recs = [{"code": None, "message": err.strip()}]
    return r.returncode, recs, err


def run(workdir, path):
    r = subprocess.run([AETHER, "--no-cache", path], capture_output=True, text=True,
                       timeout=60, cwd=workdir)
    return r.returncode, r.stdout, r.stderr


def outcome(workdir, path):
    """('reject', codes, uncoded, text) or ('run', rc, stdout, stderr)."""
    rc, recs, err = compile_records(workdir, path)
    if rc != 0:
        codes = {x.get("code") for x in recs if x.get("code")}
        uncoded = [x for x in recs if not x.get("code")]
        text = " ".join((x.get("message") or "") + " " + (x.get("hint") or "") for x in recs)
        return ("reject", codes, uncoded, text)
    return ("run",) + run(workdir, path)


def is_trap(o):
    return o[0] == "run" and o[1] != 0 and bool(
        re.search(r"Runtime Error|VM Error|\[[A-Z]+-\d{3}\]", o[3]))


def today_of(o, twin):
    """The `today` vocabulary for an observed outcome."""
    if o[0] == "reject":
        return ",".join(sorted(o[1])) if o[1] and not o[2] else "uncoded"
    if is_trap(o):
        return "trap"
    if twin is not None and o[1:3] == twin[1:3]:
        return "holds"
    return "accepted" if twin is None else "differs"


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--strict-pending", action="store_true",
                    help="fail when a pending entry's outcome no longer matches `today`")
    args = ap.parse_args()

    with open(os.path.join(SURFACE, "registry.json"), encoding="utf-8") as fh:
        reg = json.load(fh)
    with open(DECISIONS, encoding="utf-8") as fh:
        rows = set(re.findall(r"^\| (D\d+[a-z]?|S\d+) \|", fh.read(), re.M))

    problems, flipped, seen = [], [], set()
    counts = {c: 0 for c in (1, 2, 3, 4)}
    pending = 0
    with tempfile.TemporaryDirectory(prefix="aether_surface_") as workdir:
        if os.path.isdir(GUIDE_MODULES):
            for mod in sorted(os.listdir(GUIDE_MODULES)):
                shutil.copy(os.path.join(GUIDE_MODULES, mod), workdir)
        for e in reg["entries"]:
            eid, cls, status = e.get("id"), e.get("class"), e.get("status")
            where = f"registry entry {eid}"
            bad = set(e) - FIELDS
            if bad:
                problems.append(f"{where}: unknown fields {sorted(bad)}")
            if not eid or eid in seen:
                problems.append(f"{where}: missing or duplicate id")
            seen.add(eid)
            if cls not in counts:
                problems.append(f"{where}: class must be 1-4, not {cls!r}")
                continue
            counts[cls] += 1
            if e.get("decided") not in rows:
                problems.append(f"{where}: decided={e.get('decided')!r} is not a row of "
                                f"docs/aether_decisions.md")
            if status not in ("holds", "pending"):
                problems.append(f"{where}: status must be holds or pending")
                continue
            if status == "pending" and "today" not in e:
                problems.append(f"{where}: a pending entry needs `today`")
            if status == "holds" and cls in (2, 3) and not e.get("code"):
                problems.append(f"{where}: a class {cls} entry that holds needs `code`")
            if cls in (1, 4) and "twin" not in e:
                problems.append(f"{where}: a class {cls} entry needs a canonical `twin`")
                continue

            src = os.path.join(SURFACE, e["fixture"])
            if not os.path.isfile(src):
                problems.append(f"{where}: fixture {e['fixture']} is missing")
                continue
            shutil.copy(src, os.path.join(workdir, f"{eid}.aether"))
            got = outcome(workdir, os.path.join(workdir, f"{eid}.aether"))
            twin = None
            if cls in (1, 4):
                tsrc = os.path.join(SURFACE, e["twin"])
                shutil.copy(tsrc, os.path.join(workdir, f"{eid}.canon.aether"))
                twin = outcome(workdir, os.path.join(workdir, f"{eid}.canon.aether"))
                if twin[0] != "run" or is_trap(twin):
                    problems.append(f"{where}: the canonical twin itself does not run: {twin!r}"[:400])
                    continue

            if status == "pending":
                pending += 1
                now = today_of(got, twin)
                if now != e["today"]:
                    flipped.append(f"{where} (class {cls}, owner {e.get('owner', '?')}): "
                                   f"pinned today={e['today']}, now {now}")
                continue

            if cls in (1, 4):
                if got[0] != "run" or got[1:3] != twin[1:3]:
                    problems.append(f"{where}: class {cls} fixture must print what its twin "
                                    f"prints ({twin[1:3]!r}); got {got[:3]!r}"[:400])
            else:
                want = {e["code"]}
                if got[0] != "reject" or got[1] != want or got[2]:
                    problems.append(f"{where}: class {cls} fixture must be rejected with exactly "
                                    f"{e['code']}; got {today_of(got, None)}")
                elif e.get("hint") and e["hint"] not in got[3]:
                    problems.append(f"{where}: the diagnostic must name the Aether form "
                                    f"{e['hint']!r}; it says {got[3][:200]!r}")

    total = sum(counts.values())
    print(f"surface registry: {total} entries (class 1: {counts[1]}, 2: {counts[2]}, "
          f"3: {counts[3]}, 4: {counts[4]}), pending {pending}, "
          f"UNEXPECTED {len(problems)}, FLIPPED {len(flipped)}")
    for f in flipped:
        print(f"  FLIPPED {f}")
    for p in problems:
        print(f"--- UNEXPECTED {p}")
    return 1 if problems or (args.strict_pending and flipped) else 0


if __name__ == "__main__":
    sys.exit(main())
