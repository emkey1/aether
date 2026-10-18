#!/usr/bin/env python3
"""Lower an attempt whose nested fn assigns enclosing locals (FUNC-002) into
an equivalent program the current compiler accepts, to grade what the
attempt would do if such assignments were legal.

The lowering hoists each assigned local to a top-level `let` (a global, which
any function may assign) and turns its declaration in the host into a plain
assignment. For a host that is neither recursive nor re-entered while a
nested fn runs (true of every attempt in the census), a global written by
the nested fn and re-initialised at the declaration point is exactly a local
the nested fn assigns. The local's names come from the compiler: lower, run
`--no-run`, read the FUNC-002 name, repeat until the program compiles or
fails with another code.

  python3 results/decisions/nested_assign_lower.py --aether-bin build/aether SRC... --out DIR

writes DIR/<sha12>.aether (the sha256 prefix of the ORIGINAL source, the key
nested_assign_census.py --lowered-dir looks up) and prints what it hoisted.
"""

from __future__ import annotations

import argparse
import hashlib
import pathlib
import re
import subprocess

ZERO = {"Int": "0", "Real": "0.0", "Bool": "false", "Text": '""'}


def zero_of(t: str) -> str | None:
    t = t.strip()
    if t.endswith("[]"):
        return "[]"
    return ZERO.get(t)


def first_func002(aether: str, src: str, tmp: pathlib.Path) -> tuple[str | None, str]:
    tmp.write_text(src, encoding="utf-8")
    p = subprocess.run([aether, "--no-cache", "--no-run", str(tmp)], capture_output=True, text=True,
                       errors="replace")
    m = re.search(r"\[FUNC-002\][^\n]*cannot change '(\w+)'", p.stderr)
    first = re.search(r"\[[A-Z]+-\d+\][^\n]*", p.stderr)
    return (m.group(1) if m else None), (first.group(0) if first else ("ok" if p.returncode == 0 else p.stderr[:120]))


def hoist(src: str, name: str) -> str | None:
    """`let NAME: T = init;` (first one, indented) -> `NAME = init;`, plus a
    top-level `let NAME: T = <zero>;` ahead of the first `fn`."""
    m = re.search(r"(?m)^([ \t]+)let\s+" + re.escape(name) + r"\s*:\s*([^=;]+?)\s*=\s*", src)
    if not m:
        return None
    t = m.group(2)
    z = zero_of(t)
    if z is None:
        return None
    src = src[:m.start()] + m.group(1) + name + " = " + src[m.end():]
    first_fn = re.search(r"(?m)^(@\w|fn\s)", src)
    at = first_fn.start() if first_fn else 0
    return src[:at] + f"let {name}: {t.strip()} = {z};\n" + src[at:]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--aether-bin", default="build/aether")
    ap.add_argument("--out", type=pathlib.Path, required=True)
    ap.add_argument("sources", nargs="+", type=pathlib.Path)
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    tmp = args.out / ".probe.aether"
    for path in args.sources:
        orig = path.read_text(encoding="utf-8", errors="replace")
        sha12 = hashlib.sha256(orig.encode("utf-8")).hexdigest()[:12]
        src, hoisted, status = orig, [], ""
        for _ in range(12):
            name, status = first_func002(args.aether_bin, src, tmp)
            if not name:
                break
            new = hoist(src, name)
            if new is None:
                status = f"cannot hoist '{name}' (no typed `let`)"
                break
            src, hoisted = new, hoisted + [name]
        (args.out / f"{sha12}.aether").write_text(src, encoding="utf-8")
        print(f"{sha12} hoisted={','.join(hoisted) or '-'} then: {status[:110]}")
    tmp.unlink(missing_ok=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
