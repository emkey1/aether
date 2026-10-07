#!/usr/bin/env python3
"""Check the Aether type oracle (src/aether/types.c, W7-24a) against the types
programs declare.

Runs every tests/*_pass.aether (or the files given) with AETHER_DUMP_TYPES=1,
which prints one row per sink -- a typed `let`, an assignment, a `ret`, a call
argument, a record field initializer, an array element -- with the declared
type, the oracle's type for the value, and their agreement:

  exact / widen (Int into Real) / narrow (Real into Int, the D1 sink coercion)
  compat (Char into Text, nil into a record, a handle into Int, `[]` into T[])
  unknown (the oracle has no answer) / MISMATCH

Exit 1 when any row is MISMATCH: the oracle then contradicts a type the program
declares and the compiler accepts. `unknown` is coverage, reported per
expression kind, not failure.

--builtins also checks the oracle's builtin return table against the binary's
own builtins_json: every name both know must agree on the return type.

    python3 tools/check_type_oracle.py            # tests/*_pass.aether
    python3 tools/check_type_oracle.py --builtins
    AETHER_BIN=build/aether python3 tools/check_type_oracle.py prog.aether
"""
import collections
import glob
import json
import os
import re
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
AETHER = os.environ.get("AETHER_BIN", os.path.join(REPO, "build", "aether"))


def dump(path):
    env = dict(os.environ, AETHER_DUMP_TYPES="1")
    env.pop("AETHER_EXPERIMENT", None)
    p = subprocess.run([AETHER, "--no-cache", path], capture_output=True, text=True,
                       env=env, cwd=os.path.dirname(path), timeout=60, stdin=subprocess.DEVNULL)
    rows = []
    for line in p.stdout.splitlines():
        if line.startswith("#"):
            continue
        parts = line.split("\t")
        if len(parts) == 6:
            rows.append(parts)
    return p.returncode, rows, p.stderr


def check_builtins():
    src = open(os.path.join(REPO, "src", "aether", "types.c")).read()
    table = {}
    for name, ret, handle in re.findall(r'\{"([a-z0-9_]+)", (AETHER_T_\w+|RET_\w+), (NULL|"\w+")\}', src):
        table[name] = (ret, handle.strip('"') if handle != "NULL" else None)
    with tempfile.TemporaryDirectory() as d:
        prog = os.path.join(d, "bj.aether")
        with open(prog, "w") as f:
            f.write("fn main() -> Void { fx { println(builtins_json(true)); } }\n")
        out = subprocess.run([AETHER, "--no-cache", prog], capture_output=True, text=True,
                             cwd=d, timeout=60).stdout
    items = json.loads(out)
    expect = {"Int": "AETHER_T_INT", "Real": "AETHER_T_REAL", "Text": "AETHER_T_TEXT",
              "Bool": "AETHER_T_BOOL", "Void": "AETHER_T_VOID", "ToonDoc": "AETHER_T_HANDLE",
              "ToonNode": "AETHER_T_HANDLE", "MStream": "AETHER_T_HANDLE"}
    bad = 0
    checked = 0
    for it in items:
        rt = it.get("return_type")
        if not rt:
            continue
        for key in (it.get("backend_name"), it.get("name")):
            if not key or key.lower() not in table:
                continue
            ours, handle = table[key.lower()]
            want = expect.get(rt)
            checked += 1
            if ours == "RET_RANDOM" and rt in ("Real", "Int"):
                continue
            if want is None or ours != want or (want == "AETHER_T_HANDLE" and handle != rt):
                print("builtin %s: builtins_json says %s, the oracle says %s%s" %
                      (key, rt, ours, "(%s)" % handle if handle else ""))
                bad += 1
    print("builtin return rows checked against builtins_json: %d, disagreements: %d" % (checked, bad))
    return 1 if bad else 0


def main(argv):
    if "--builtins" in argv:
        return check_builtins()
    files = [a for a in argv if not a.startswith("-")] or sorted(
        glob.glob(os.path.join(REPO, "tests", "*_pass.aether")))
    by_kind = collections.defaultdict(collections.Counter)
    mismatches = []
    for path in files:
        rc, rows, err = dump(os.path.abspath(path))
        for line, sink, kind, want, have, agree in rows:
            by_kind[kind][agree] += 1
            if agree == "MISMATCH":
                mismatches.append("%s:%s %s %s declared %s, oracle %s" %
                                  (os.path.basename(path), line, sink, kind, want, have))
    agreements = ["exact", "widen", "narrow", "compat", "unknown", "MISMATCH"]
    print("%-18s %s" % ("expression", " ".join("%8s" % a for a in agreements)))
    total = collections.Counter()
    for kind in sorted(by_kind):
        c = by_kind[kind]
        total.update(c)
        print("%-18s %s" % (kind, " ".join("%8d" % c[a] for a in agreements)))
    print("%-18s %s" % ("all", " ".join("%8d" % total[a] for a in agreements)))
    print("files: %d, sinks: %d" % (len(files), sum(total.values())))
    for m in mismatches:
        print("MISMATCH " + m)
    return 1 if mismatches else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
