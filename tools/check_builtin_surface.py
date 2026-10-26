#!/usr/bin/env python3
"""Check the whole builtin surface against an aether binary, generated from tables.

Nothing used to check that a documented builtin reaches an engine function: of
the 60 names D72 renamed, 45 were called by no test or example, so a wrong alias
would have shipped silently. This tool generates its programs from the tables
themselves, so a builtin added to either table is covered with no test to write:

  documented  every signature in the builtin listing (`builtins_json(true)`, the
              table behind the guide's appendix; each overload, with and without
              its defaulted parameters) is called with its documented arity and
              argument types, its result bound to its documented return type,
              and the program must compile (`--no-run`);
  listed      every other listed name must resolve: a call to it may draw an
              arity or type diagnostic but never SCOPE-001 for that name;
  removed     every removed spelling (the front end's removed-builtin table,
              D55 and D72, plus the surface registry's D61/D66 class-2 entries)
              must fail with SCOPE-001 and a hint naming its replacement.

  python3 tools/check_builtin_surface.py [--aether-bin BIN] [--jobs N] [-v]

Exit 0 when every check passes, 1 otherwise. A signature this tool cannot turn
into a call (a parameter with no type, a record-field argument) is reported as
skipped with its reason, never silently dropped.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import json
import os
import pathlib
import re
import subprocess
import sys
import tempfile

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT / "tools"))
import gen_builtin_appendix as appendix  # noqa: E402  (the appendix's own sections)

KNOWN = REPO_ROOT / "tests" / "builtin_surface.known"
REMOVED_TABLE = REPO_ROOT / "src" / "aether" / "ast_stdlib.c"
REGISTRY = REPO_ROOT / "tests" / "surface" / "registry.json"

DUMP_SRC = 'fn main() -> Void {\n    fx {\n        println(builtins_json(true));\n    }\n    ret;\n}\n'

# A value of each documented parameter type, and the declarations it needs.
SETUP = {
    "ToonDoc": 'let doc: ToonDoc = toon_parse("{}");',
    "ToonNode": 'let node: ToonNode = toon_root(doc);',
    "MStream": 'let ms: MStream = mstream_new();',
    "Map": 'let m: Map[Text, Int] = {"a": 1};',
}
SETUP_NEEDS = {"ToonNode": ["ToonDoc"]}


def value_for(ptype: str) -> tuple[str, list[str]] | None:
    """An argument expression of type `ptype` and the setups it needs."""
    t = ptype.strip()
    t = re.sub(r"\bT\b|\bR\b", "Int", t)          # generics: instantiate at Int
    simple = {"Int": "1", "Real": "1.5", "Text": '"a"', "Bool": "true",
              "Int[]": "[1]", "Real[]": "[1.5]", "Text[]": '["a"]', "Bool[]": "[true]",
              "Int[][]": "[[1]]"}
    if t in simple:
        return simple[t], []
    if t == "ToonDoc":
        return "doc", ["ToonDoc"]
    if t == "ToonNode":
        return "node", ["ToonDoc", "ToonNode"]
    if t == "MStream":
        return "ms", ["MStream"]
    if t.startswith("Map[Text"):
        return "m", ["Map"]
    if t in ("arrayOrText",):
        return "[1]", []
    return None


def return_binding(ret: str, call: str) -> tuple[str, str | None]:
    """The statement that binds `call` to its documented return type."""
    r = ret.strip()
    r = re.split(r"\s+in\s+[\[(]", r)[0].strip()  # "Int in [0,n)" -> "Int"
    r = re.sub(r"\s+\([^()]*\)$", "", r)           # "T (the target's result)" -> "T"
    r = re.sub(r"\bT\b|\bR\b", "Int", r)
    if r == "Void":
        return f"{call};", None
    if r.startswith("(") and r.endswith(")"):
        n = len(split_top(r[1:-1]))
        names = ", ".join(f"r{i}" for i in range(n))
        return f"let ({names}) = {call};", None
    if r in ("Array", "untyped array"):
        # An engine array of records with no Aether type (task_stats, whose
        # signature said "Array" until 2026-10-26): the documented use is
        # counting it.
        return f"let r: Int = length({call});", None
    if r.startswith("Map[Text"):
        return f"let r: Map[Text, Int] = {call};", None
    return f"let r: {r} = {call};", None


def split_top(s: str) -> list[str]:
    """Split on commas outside brackets and parentheses."""
    out, depth, cur = [], 0, ""
    for ch in s:
        if ch in "([":
            depth += 1
        elif ch in ")]":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur)
            cur = ""
        else:
            cur += ch
    if cur.strip():
        out.append(cur)
    return [p.strip() for p in out if p.strip()]


OVERLOAD_START = re.compile(r";\s+(?=[a-z_][a-z0-9_]*\()")


def overloads(signature: str) -> list[tuple[str, list[str], str]]:
    """(name, params, return type) for each overload in a listing signature."""
    out = []
    for piece in OVERLOAD_START.split(signature):
        m = re.match(r"^([a-z_][a-z0-9_]*)\((.*?)\)\s*->\s*(.+)$", piece.strip())
        if m:
            out.append((m.group(1), split_top(m.group(2)), m.group(3).strip()))
    return out


# A builtin whose generic parameter must be a record: (top-level declarations,
# setup lines, argument expressions).
OVERRIDES = {
    "same": ("type SurfaceRec { x: Int = 0; }", ["let a: SurfaceRec = new SurfaceRec();"], ["a", "a"]),
    # A record array and one of its fields, named by a string literal.
    "sorted_by": ("type SurfaceRec { x: Int = 0; }", ["let rs: SurfaceRec[] = [new SurfaceRec()];"],
                  ["rs", '"x"']),
    "sorted_by_desc": ("type SurfaceRec { x: Int = 0; }", ["let rs: SurfaceRec[] = [new SurfaceRec()];"],
                       ["rs", '"x"']),
    # The worker builtin is named as Text, its argument passed through (the guide's example).
    "task_spawn": ("", [], ['"delay"', '"w"', "5"]),
    "task_queue": ("", [], ['"delay"', '"w"', "5"]),
    "len": ("", [], ["[1]"]),
}


def programs_for(entry: dict) -> list[tuple[str, str | None]]:
    """(label, program or None) per call shape; None carries the skip reason in label."""
    shapes = []
    if entry["name"] in OVERRIDES:
        top, setups, argv = OVERRIDES[entry["name"]]
        for name, params, ret in overloads(entry["signature"])[:1]:
            call = f"{name}({', '.join(argv)})"
            stmt, _ = return_binding(ret.replace("R[]", "SurfaceRec[]") if "SurfaceRec" in top else ret, call)
            body = "\n".join("        " + s for s in setups + [stmt])
            shapes.append((f"{call} -> {ret}", f"{top}\n\nfn main() -> Void {{\n    fx {{\n{body}\n    }}\n    ret;\n}}\n"))
        return shapes
    for name, params, ret in overloads(entry["signature"]):
        required, optional = [], []
        for p in params:
            if p.endswith("..."):
                required.append(("variadic", "Text"))
                continue
            if ":" not in p:
                shapes.append((f"{name}: parameter '{p}' has no type", None))
                break
            pname, ptype = (x.strip() for x in p.split(":", 1))
            if "=" in ptype:
                optional.append((pname, ptype.split("=", 1)[0].strip()))
            else:
                required.append((pname, ptype))
        else:
            variants = [required] + ([required + optional] if optional else [])
            for params_used in variants:
                args, needs = [], []
                for pname, ptype in params_used:
                    v = value_for(ptype)
                    if v is None:
                        args = None
                        shapes.append((f"{name}: no value for type '{ptype}'", None))
                        break
                    args.append(v[0])
                    needs += v[1]
                if args is None:
                    continue
                call = f"{name}({', '.join(args)})"
                stmt, _ = return_binding(ret, call)
                setups, seen = [], set()
                for n in needs:
                    for dep in SETUP_NEEDS.get(n, []) + [n]:
                        if dep not in seen:
                            seen.add(dep)
                            setups.append(SETUP[dep])
                body = "\n".join("        " + s for s in setups + [stmt])
                prog = f"fn main() -> Void {{\n    fx {{\n{body}\n    }}\n    ret;\n}}\n"
                shapes.append((f"{call} -> {ret}", prog))
    return shapes


def removed_names() -> list[tuple[str, str]]:
    """(removed spelling, replacement the hint must name)."""
    out = []
    src = REMOVED_TABLE.read_text()
    table = src.split("kAetherRemovedBuiltins[] = {", 1)[1].split("\n};", 1)[0]
    for m in re.finditer(r'\{\s*"([a-z_0-9]+)",\s*((?:"(?:[^"\\]|\\.)*"\s*|[A-Z_]+\s*)+)\}', table):
        hint = m.group(2)
        code = re.search(r"`([a-z_][a-z0-9_]*)\(", "".join(re.findall(r'"((?:[^"\\]|\\.)*)"', hint)))
        out.append((m.group(1), code.group(1) if code else ""))
    reg = json.loads(REGISTRY.read_text())
    for e in reg["entries"]:
        if e.get("class") == 2 and e.get("decided") in ("D61", "D66") and e["id"].startswith("removed_"):
            old = re.match(r"([a-z_0-9]+)\(", e["spelling"])
            new = re.match(r"([a-z_0-9]+)\(", e["canonical_form"])
            if old and new:
                out.append((old.group(1), new.group(1)))
    return out


def compile_src(aether: str, src: str, workdir: str, idx: int) -> subprocess.CompletedProcess:
    path = os.path.join(workdir, f"p{idx}.aether")
    with open(path, "w") as fh:
        fh.write(src)
    return subprocess.run([aether, "--no-cache", "--no-run", path], capture_output=True,
                          text=True, timeout=60, cwd=workdir)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--aether-bin", default=os.environ.get("AETHER_BIN", str(REPO_ROOT / "build" / "aether")))
    ap.add_argument("--jobs", type=int, default=8)
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()
    aether = os.path.abspath(args.aether_bin)

    with tempfile.TemporaryDirectory() as td:
        dump = os.path.join(td, "dump.aether")
        with open(dump, "w") as fh:
            fh.write(DUMP_SRC)
        out = subprocess.run([aether, "--no-cache", dump], capture_output=True, text=True, timeout=120)
        if out.returncode != 0:
            sys.exit(f"aether failed to dump builtins:\n{out.stderr}")
        listing = json.loads(out.stdout)

        # The surface the guide's appendix states: documented signatures, names
        # documented in the guide's own tables, and names listed without one.
        guide_text = pathlib.Path(appendix.GUIDE).read_text()
        appendix.AETHER = aether
        documented, above, name_only = appendix.build_sections(appendix.callable_entries(listing),
                                                               appendix.prose_documented(guide_text))
        backends = {e["name"]: e.get("backend_name") for e in listing}
        jobs, skipped = [], []
        for e in (x for cat in documented.values() for x in cat):
            for label, prog in programs_for(e):
                if prog is None:
                    skipped.append(label)
                else:
                    jobs.append(("documented", e["name"], label, prog))
        for e in (x for sec in (above, name_only) for cat in sec.values() for x in cat):
            prog = f"fn main() -> Void {{\n    fx {{\n        {e['name']}();\n    }}\n    ret;\n}}\n"
            jobs.append(("listed", e["name"], f"{e['name']}()", prog))
        for old, new in removed_names():
            prog = f"fn main() -> Void {{\n    fx {{\n        {old}(1);\n    }}\n    ret;\n}}\n"
            jobs.append(("removed", old, f"{old} -> {new}", prog))

        failures = []
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
            results = list(pool.map(lambda ij: (ij[1], compile_src(aether, ij[1][3], td, ij[0])),
                                    enumerate(jobs)))
        counts = {"documented": [0, 0], "listed": [0, 0], "removed": [0, 0]}
        for (kind, name, label, _prog), run in results:
            err = run.stderr
            if kind == "documented":
                ok = run.returncode == 0
                why = (err.strip().splitlines() or ["(no stderr)"])[0]
                why = re.sub(r"^\S+\.aether:\d+: ", "", why)
                backend = backends.get(name)
                if not ok and backend and re.search(r"\[SCOPE-001\][^\n]*'%s'" % re.escape(backend), err):
                    why = f"not in this build: engine builtin '{backend}' is not registered"
            elif kind == "listed":
                bad = re.search(r"\[SCOPE-001\][^\n]*'%s'" % re.escape(name), err)
                ok = not bad
                why = bad.group(0) if bad else ""
            else:
                new = label.split(" -> ", 1)[1]
                ok = run.returncode != 0 and "[SCOPE-001]" in err and (not new or new in err)
                why = (err.strip().splitlines() or ["(compiled)"])[0] if not ok else ""
            counts[kind][0 if ok else 1] += 1
            if not ok:
                failures.append((f"{kind} {name}", f"{kind:10s} {label}: {why[:220]}"))
            elif args.verbose:
                print(f"ok  {kind:10s} {label}")

    # tests/builtin_surface.known: `<kind> <name>  # why` lines for failures
    # an open decision owns. It may only shrink: a listed entry that passes is
    # stale and fails the check until it is removed.
    known = {}
    if KNOWN.exists():
        for line in KNOWN.read_text().splitlines():
            key = line.split("#", 1)[0].strip()
            if key:
                known[" ".join(key.split())] = line
    failed_keys = {k for k, _ in failures}
    unexpected = [msg for k, msg in failures if k not in known]
    stale = sorted(k for k in known if k not in failed_keys)
    for msg in sorted(set(msg for k, msg in failures if k in known)):
        print("known", msg)
    for msg in sorted(unexpected):
        print("FAIL", msg)
    for k in stale:
        print("STALE", f"{k}: listed in {KNOWN.name} but passes; remove the line")
    for s in sorted(set(skipped)):
        print("skip", s)
    print("builtin surface: documented %d ok / %d failed (%d shapes skipped), listed %d ok / %d failed, "
          "removed %d ok / %d failed" % (counts["documented"][0], counts["documented"][1], len(set(skipped)),
                                        counts["listed"][0], counts["listed"][1],
                                        counts["removed"][0], counts["removed"][1]))
    if known:
        print(f"known failures: {len(known) - len(stale)} still failing, {len(stale)} stale")
    return 1 if unexpected or stale else 0


if __name__ == "__main__":
    sys.exit(main())
