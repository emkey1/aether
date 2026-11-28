#!/usr/bin/env python3
"""Step 3(b), second pass: does an engine commit change what the aether build compiles?

For every engine commit the first pass found touching a file the aether target
reads, preprocess each translation unit that reads a touched file (the file
itself for a .c, every TU whose depfile lists it for a header) with that TU's
own compile command from compile_commands.json, once at the commit's parent and
once at the commit, in one scratch worktree of the engine repo, and compare.
-E -P drops comments and line markers; __LINE__, __FILE__, __DATE__ and
__TIME__ are pinned so moved lines do not count; blank lines and runs of spaces
are normalised. Identical output for every TU means the commit cannot change
the aether binary (comments, other platforms, #if-ed-out code).

Usage (after `pin_moves_census.py firstpass`; the build configured with
-DCMAKE_EXPORT_COMPILE_COMMANDS=ON and built, so its depfiles exist):
  python3 pin_moves_preproc_diff.py <aether src root> <build dir> <classed.json> <out.json>
"""
import concurrent.futures
import hashlib
import json
import os
import re
import shlex
import subprocess
import sys

src_root, build_dir, classed_path, out_path = sys.argv[1:5]
src_root = os.path.realpath(src_root)
build_dir = os.path.realpath(build_dir)
SCRATCH = os.path.join(os.path.dirname(os.path.realpath(out_path)), "engine_wt")
ENGINES = {"external/pscal-core": "pscal-core", "external/rea": "rea"}


def run(cmd, cwd=None, check=True):
    return subprocess.run(cmd, cwd=cwd, text=True, capture_output=True, check=check)


# -- TU commands and which TUs read which file -----------------------------------
commands = {}
for entry in json.load(open(os.path.join(build_dir, "compile_commands.json"))):
    path = os.path.realpath(os.path.join(entry["directory"], entry["file"]))
    args = entry.get("arguments") or shlex.split(entry["command"])
    commands[path] = (entry["directory"], args)

readers = {}  # repo-relative path (external/...) -> set of TU source paths
for dirpath, _, names in os.walk(build_dir):
    for name in names:
        if not name.endswith(".o.d"):
            continue
        text = open(os.path.join(dirpath, name), errors="replace").read().replace("\\\n", " ")
        if ":" not in text:
            continue
        target, deps = text.split(":", 1)
        files = [os.path.realpath(d) for d in deps.split()]
        tu = next((f for f in files if f in commands), None)
        if tu is None:
            continue
        for f in files:
            rel = os.path.relpath(f, src_root)
            if rel.startswith("external/"):
                readers.setdefault(rel, set()).add(tu)


def pp_args(directory, args, engine_prefix, worktree):
    """The TU's command, retargeted at the worktree, as a preprocess-only run."""
    out, skip = [], False
    for a in args:
        if skip:
            skip = False
            continue
        if a in ("-o", "-MF", "-MT", "-MQ"):
            skip = True
            continue
        if a in ("-c", "-MD", "-MMD") or a.startswith("-MF") or a.startswith("-o"):
            continue
        out.append(a.replace(engine_prefix, worktree))
    out[1:1] = ["-E", "-P", "-w", "-Wno-builtin-macro-redefined", "-D__LINE__=0",
                '-D__FILE__="f"', '-D__DATE__="d"', '-D__TIME__="t"']
    return out


def normalise(text):
    lines = [" ".join(line.split()) for line in text.splitlines()]
    return "\n".join(line for line in lines if line)


def preprocess_all(tus, engine_prefix, worktree):
    def one(tu):
        directory, args = commands[tu]
        src = tu.replace(engine_prefix, worktree)
        if tu.startswith(engine_prefix) and not os.path.exists(src):
            return tu, "absent"
        proc = run(pp_args(directory, args, engine_prefix, worktree), cwd=directory, check=False)
        if proc.returncode != 0:
            return tu, "error:" + hashlib.sha1(proc.stderr.encode()).hexdigest()[:8] + ":" + proc.stderr.strip().splitlines()[-1][:160] if proc.stderr.strip() else "error"
        return tu, hashlib.sha256(normalise(proc.stdout).encode()).hexdigest()
    with concurrent.futures.ThreadPoolExecutor(max_workers=12) as pool:
        return dict(pool.map(one, sorted(tus)))


data = json.load(open(classed_path))
results = []
os.makedirs(SCRATCH, exist_ok=True)
worktrees = {}
for rel_engine, short in ENGINES.items():
    repo = os.path.join(src_root, rel_engine)
    wt = os.path.join(SCRATCH, short)
    if not os.path.isdir(wt):
        run(["git", "-C", repo, "worktree", "add", "--detach", wt, "HEAD"])
    worktrees[rel_engine] = (repo, wt)

for row in data["rows"]:
    for move in row["moves"]:
        for e in move.get("engine_commits", []):
            if e.get("first_pass") != "compiled":
                continue
            rel_engine = move["path"]
            repo, wt = worktrees[rel_engine]
            engine_prefix = os.path.join(src_root, rel_engine)
            tus = set()
            for f in e["in_build"]:
                tus |= readers.get(f, set())
                if f.endswith((".c", ".m")):
                    tus.add(os.path.realpath(os.path.join(src_root, f)))
            tus = {t for t in tus if t in commands}
            run(["git", "-C", wt, "checkout", "-q", "--detach", e["sha"] + "^"])
            before = preprocess_all(tus, engine_prefix, wt)
            run(["git", "-C", wt, "checkout", "-q", "--detach", e["sha"]])
            after = preprocess_all(tus, engine_prefix, wt)
            changed = sorted(os.path.relpath(t, src_root) for t in tus if before[t] != after[t])
            errors = sorted({v for v in list(before.values()) + list(after.values()) if not re.fullmatch(r"[0-9a-f]{64}", v)})
            verdict = "binary-identical" if not changed and not errors else ("changes" if changed else "unresolved")
            results.append({"aether_commit": row["commit"][:7], "engine": rel_engine, "sha": e["sha"][:10],
                            "subject": e["subject"], "tus": len(tus), "verdict": verdict,
                            "changed_tus": changed, "errors": errors})
            print(f"{e['sha'][:10]} {verdict:17} tus={len(tus):3} changed={len(changed):3} {e['subject'][:70]}", flush=True)

json.dump(results, open(out_path, "w"), indent=1)
counts = {}
for r in results:
    counts[r["verdict"]] = counts.get(r["verdict"], 0) + 1
print("DONE", counts)
