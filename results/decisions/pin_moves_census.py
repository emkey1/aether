#!/usr/bin/env python3
"""Sort the external/ pin moves since a tag that carry no pin_gate summary into
"Aether-visible" and "other front end" (roadmap step 3(b), E1's step 0).

Three steps, each writing JSON for the next (see pin_moves_since_1.0_2026-11-28.md):

  list      every commit in BASE..HEAD that moved an external/ gitlink and whose
            message does not mention pin_gate, the engine range each move spans
            and every engine commit's files (git only).
  firstpass marks each engine commit "compiled" when it touches a file the
            aether target reads (a deps list from a build's depfiles), else
            "outside".
  rollup    takes pin_moves_preproc_diff.py's verdicts for the compiled ones and
            the reading verdicts for those whose preprocessed code changed, and
            classes every engine commit and every pin move.

  python3 pin_moves_census.py list <aether repo> <base> <head> list.json
  python3 pin_moves_census.py firstpass list.json <deps.txt> classed.json
  python3 pin_moves_census.py rollup classed.json <preproc.json> <reading.json> out.json
"""
import collections
import json
import re
import subprocess
import sys

SDL = re.compile(r"(^|/)(sdl|gl|audio|graphics_3d|sdl3d|pscal_sdl)[^/]*$|src/runtime/(shaders|terrain)/")


def git(*args, cwd):
    return subprocess.run(["git", *args], cwd=cwd, text=True, capture_output=True, check=True).stdout


def gitlinks(repo, commit):
    links = {}
    for line in git("ls-tree", commit, "external/", cwd=repo).splitlines():
        meta, path = line.split("\t")
        _, kind, sha = meta.split()
        if kind == "commit":
            links[path] = sha
    return links


def engine_commits(sub, old, new):
    out = []
    for e in git("rev-list", "--reverse", f"{old}..{new}", cwd=sub).split():
        files = [f for f in git("show", "--format=", "--name-only", e, cwd=sub).splitlines() if f]
        out.append({"sha": e, "subject": git("log", "-1", "--format=%s", e, cwd=sub).strip(), "files": files})
    return out


def cmd_list(repo, base, head, out):
    moves = git("rev-list", "--reverse", f"{base}..{head}", "--", "external", cwd=repo).split()
    gated = set(git("rev-list", f"{base}..{head}", "--grep=pin_gate", "--", "external", cwd=repo).split())
    rows, pending = [], {}
    for c in moves:
        if c in gated:
            continue
        parent = git("rev-list", "--parents", "-n", "1", c, cwd=repo).split()[1]
        before, after = gitlinks(repo, parent), gitlinks(repo, c)
        row = {"commit": c, "subject": git("log", "-1", "--format=%s", c, cwd=repo).strip(),
               "date": git("log", "-1", "--format=%cs", c, cwd=repo).strip(), "moves": []}
        for path in sorted(set(before) | set(after)):
            old, new = before.get(path), after.get(path)
            if old == new:
                continue
            sub = f"{repo}/{path}"
            move = {"path": path, "old": old, "new": new}
            # A gitlink to a commit the engine never had (c521ffb) is repaired by a later
            # move; the range is counted once, under the move that set the bad link.
            if subprocess.run(["git", "cat-file", "-e", f"{new}^{{commit}}"], cwd=sub,
                              capture_output=True).returncode != 0:
                pending[path] = (row, move)
                move["note"] = "gitlink names no engine commit; repaired by a later move"
                move["engine_commits"] = []
            elif path in pending and pending[path][1]["new"] == old:
                first_row, first_move = pending.pop(path)
                first_move["engine_commits"] = engine_commits(sub, first_move["old"], new)
                first_move["repaired_to"] = new
                move["note"] = f"repairs {first_row['commit'][:7]}'s gitlink; its range is counted there"
                move["engine_commits"] = []
            else:
                move["engine_commits"] = engine_commits(sub, old, new)
            row["moves"].append(move)
        rows.append(row)
    json.dump({"base": base, "head": head, "all": len(moves), "gated": len(gated), "rows": rows},
              open(out, "w"), indent=1)
    n = sum(len(m["engine_commits"]) for r in rows for m in r["moves"])
    print(f"{len(rows)} of {len(moves)} pin moves carry no pin_gate summary; {n} engine commits in their ranges")


def cmd_firstpass(list_json, deps_txt, out):
    data = json.load(open(list_json))
    deps = set(open(deps_txt).read().split())
    counts = collections.Counter()
    for row in data["rows"]:
        for m in row["moves"]:
            for e in m["engine_commits"]:
                full = [f"{m['path']}/{f}" for f in e["files"]]
                e["in_build"] = [f for f in full if f in deps]
                e["first_pass"] = "compiled" if e["in_build"] else "outside"
                counts[e["first_pass"]] += 1
    json.dump(data, open(out, "w"), indent=1)
    print(dict(counts))


def cmd_rollup(classed_json, preproc_json, reading_json, out):
    data = json.load(open(classed_json))
    pre = {r["sha"]: r for r in json.load(open(preproc_json))}
    read = {r["sha"]: r for r in json.load(open(reading_json))}
    engine_rows, move_rows = [], []
    for row in data["rows"]:
        classes = []
        for m in row["moves"]:
            for e in m["engine_commits"]:
                s10 = e["sha"][:10]
                if e["first_pass"] == "outside":
                    if all(SDL.search(f) for f in e["files"]):
                        c, how, why = "sdl-build-only", "files", "touches only SDL backend files; the default build has SDL off"
                    else:
                        c, how, why = "other-front-end", "files", "touches no file the default aether build compiles or includes"
                elif pre[s10]["verdict"] == "binary-identical":
                    c, how, why = ("other-front-end", "preprocess",
                                   f"all {pre[s10]['tus']} aether translation units that read it preprocess identically")
                else:
                    c, how, why = read[s10]["class"], "reading", read[s10]["reason"]
                engine_rows.append({"pin_move": row["commit"][:7], "engine": m["path"].split("/")[1], "sha": s10,
                                    "subject": e["subject"], "class": c, "decided_by": how, "reason": why})
                classes.append(c)
        for wanted in ("aether-visible", "unsure", "sdl-build-only", "other-front-end"):
            if wanted in classes:
                mc = wanted
                break
        else:
            mc = "no-engine-change"
        move_rows.append({"pin_move": row["commit"][:7], "date": row["date"], "subject": row["subject"],
                          "engine_commits": len(classes), "class": mc,
                          "visible": [r["sha"] for r in engine_rows
                                      if r["pin_move"] == row["commit"][:7] and r["class"] == "aether-visible"]})
    json.dump({"base": data["base"], "head": data["head"], "engine_commits": engine_rows, "pin_moves": move_rows},
              open(out, "w"), indent=1)
    print("engine commits:", dict(collections.Counter(r["class"] for r in engine_rows)))
    print("by method:", dict(collections.Counter(f"{r['decided_by']}:{r['class']}" for r in engine_rows)))
    print("pin moves:", dict(collections.Counter(r["class"] for r in move_rows)))


if __name__ == "__main__":
    {"list": cmd_list, "firstpass": cmd_firstpass, "rollup": cmd_rollup}[sys.argv[1]](*sys.argv[2:])
