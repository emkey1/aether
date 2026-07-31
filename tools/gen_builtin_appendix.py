#!/usr/bin/env python3
"""Regenerate the builtin-inventory appendix in the full LLM guide.

The appendix exists so BUILT-001's "if it is not listed here, it does not exist"
is something a reader can actually check. Before it, the guide documented a
curated subset and told models to discover the rest with `builtin_info(...)` --
a *runtime* call that a one-shot generator cannot make. Three separate models in
one week guessed at names (`socket_create`, `tcp_socket`, `toon_parse_string`)
that a complete list would have settled.

Three tiers, because they carry different information:

  * documented    -- a real signature; safe to call exactly as shown.
  * documented    -- no registry signature, but the guide's own prose documents
    above            it (the math, conversion, and arity tables). Listed by name
                     so the "absent means nonexistent" check still works, and
                     pointed back at the body rather than described as unknown.
  * name only     -- the name exists, but its signature is not documented.
                     Useful as *negative* information: it confirms a name is
                     real without licensing a guess at its arguments, and it
                     confirms that anything absent from all three tiers does not
                     exist at all.

The middle tier exists because the first cut of this generator bucketed purely
on whether the *registry* entry carried a signature, which put 68 names --
`abs`, `min`, `max`, `clamp`, `sqrt`, `round`, `parse_int`, `split`, the whole
socket surface -- under a heading reading "their signatures are not documented
here ... restructure to use a documented helper above", while the body above
documented every one of them. Models followed the appendix and hand-rolled
`clamp`, `abs`, and their own substring loops. The appendix must never contradict
the prose it ships with.

Regenerate after adding or renaming a builtin:

    python3 tools/gen_builtin_appendix.py

It rewrites the region between the markers in docs/aether_for_llms_and_others.md
and leaves the rest of the file untouched.
"""
import json
import os
import re
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
AETHER = os.environ.get("AETHER_BIN", os.path.join(REPO, "build", "aether"))
GUIDE = os.path.join(REPO, "docs", "aether_for_llms_and_others.md")

BEGIN = "<!-- BEGIN GENERATED BUILTIN INVENTORY -->"
END = "<!-- END GENERATED BUILTIN INVENTORY -->"

# Host-registered demo/graphics surfaces. Real in some builds, but not part of
# the language a model should be writing against -- listing them would suggest
# `bouncingballs3dstepultraadvanced` is Aether.
SKIP_CATEGORIES = {"3d", "user"}

# Friendlier headings than the raw category strings.
CATEGORY_TITLES = {
    "toon": "TOON (structured data)",
    "sqlite": "SQLite",
    "network": "Network and HTTP",
    "task": "Tasks and concurrency",
    "filesystem": "Filesystem",
    "system": "System and process",
    "capability": "Capability probes",
    "strings": "Strings",
    "io": "Input / output",
    "conversion": "Conversion",
    "time": "Clock",
    "text": "Text",
    "collection": "Collections",
    "ai": "AI",
    "core": "Core",
}


def dump_builtins():
    src = 'fn main() -> Void {\n    fx {\n        println(builtins_json(true));\n    }\n    ret;\n}\n'
    with tempfile.TemporaryDirectory() as td:
        path = os.path.join(td, "dump.aether")
        with open(path, "w") as fh:
            fh.write(src)
        out = subprocess.run([AETHER, "--no-cache", path],
                             capture_output=True, text=True, timeout=120)
        if out.returncode != 0:
            sys.exit(f"aether failed to dump builtins:\n{out.stderr}")
        return json.loads(out.stdout)


# A name the prose documents looks like `foo(` inside backticks -- that is how
# every signature in the math, conversion, arity, and helper tables is written.
# Only the hand-written body counts, so scan up to the generated block.
PROSE_CALL = re.compile(r"`([A-Za-z_][A-Za-z0-9_]*)\s*\(")


def prose_documented(guide_text):
    head = guide_text.split(BEGIN)[0]
    return set(PROSE_CALL.findall(head))


def build_sections(entries, in_prose):
    # An entry whose name is some other entry's backend_name is the internal
    # spelling of an Aether-facing alias; the alias is what should be written.
    backend_names = {e["backend_name"] for e in entries if e.get("backend_name")}

    best = {}
    for e in entries:
        if (e.get("category") or "") in SKIP_CATEGORIES:
            continue
        name = e["name"]
        if name in backend_names and name not in {x.get("backend_name") for x in [e]}:
            pass
        if name in backend_names:
            continue
        prev = best.get(name)
        # Prefer the entry carrying a signature.
        if prev is None or (not prev.get("signature") and e.get("signature")):
            best[name] = e

    documented, above, name_only = {}, {}, {}
    for name, e in best.items():
        if e.get("signature"):
            bucket = documented
        elif name in in_prose:
            bucket = above
        else:
            bucket = name_only
        bucket.setdefault(e.get("category") or "core", []).append(e)
    return documented, above, name_only


def name_list(cat_map, cat):
    names = sorted(e["name"] for e in cat_map[cat])
    eff = {e["name"] for e in cat_map[cat] if e["effectful"]}
    return ", ".join(f"`{n}`{' *fx*' if n in eff else ''}" for n in names)


def render(documented, above, name_only):
    out = [BEGIN, ""]
    out.append("*Generated by `tools/gen_builtin_appendix.py`. Do not edit by hand.*")
    out.append("")
    doc_count = sum(len(v) for v in documented.values())
    above_count = sum(len(v) for v in above.values())
    raw_count = sum(len(v) for v in name_only.values())
    out.append(
        f"Every builtin this compiler exposes: **{doc_count}** with a signature "
        f"below, **{above_count}** whose signatures are in the tables earlier in "
        f"this document, and **{raw_count}** more by name only. If a helper "
        "appears in none of the three, it does not exist — inline the logic "
        "instead of reaching for it."
    )
    out.append("")
    out.append("`fx` marks a builtin as effectful: it must be called inside an "
               "`fx { ... }` block and may not be called from a `@pure` function "
               "(FX-001).")
    out.append("")

    out.append("### Documented signatures")
    out.append("")
    out.append("Safe to call exactly as written.")
    out.append("")
    for cat in sorted(documented, key=lambda c: (c != "toon", c)):
        rows = sorted(documented[cat], key=lambda e: e["name"])
        out.append(f"**{CATEGORY_TITLES.get(cat, cat)}**")
        out.append("")
        out.append("| Signature | Effect |")
        out.append("|---|---|")
        for e in rows:
            sig = e["signature"].replace("|", "\\|")
            out.append(f"| `{sig}` | {'`fx`' if e['effectful'] else 'pure'} |")
        out.append("")

    out.append("### Documented earlier in this document")
    out.append("")
    out.append(
        "Real, and their signatures **are** documented — in the conversion, "
        "arity, math, and helper tables above, not in the table below. They are "
        "listed here only so that \"absent from the appendix\" still means "
        "\"does not exist\". Scroll up for the signature; do not treat these as "
        "unknown and do not hand-roll a replacement."
    )
    out.append("")
    for cat in sorted(above):
        out.append(f"**{CATEGORY_TITLES.get(cat, cat)}** — " + name_list(above, cat))
        out.append("")

    out.append("### Name only")
    out.append("")
    out.append(
        "These names exist, but their signatures are not documented here. That "
        "is deliberate and it is still useful: it confirms a name is real "
        "**without** licensing a guess at its arguments. Calling one with an "
        "invented argument list is how `BUILT-002` and uncoded runtime errors "
        "happen. If you need one of these and cannot verify its signature by "
        "running the compiler, restructure to use a documented helper above."
    )
    out.append("")
    for cat in sorted(name_only):
        out.append(f"**{CATEGORY_TITLES.get(cat, cat)}** — " + name_list(name_only, cat))
        out.append("")

    out.append(END)
    return "\n".join(out)


def main():
    entries = dump_builtins()

    with open(GUIDE) as fh:
        text = fh.read()
    if BEGIN not in text or END not in text:
        sys.exit(f"markers not found in {GUIDE}; add {BEGIN} / {END} first")

    documented, above, name_only = build_sections(entries, prose_documented(text))
    block = render(documented, above, name_only)

    head = text.split(BEGIN)[0]
    tail = text.split(END, 1)[1]
    with open(GUIDE, "w") as fh:
        fh.write(head + block + tail)

    doc_count = sum(len(v) for v in documented.values())
    above_count = sum(len(v) for v in above.values())
    raw_count = sum(len(v) for v in name_only.values())
    print(f"builtin appendix: {doc_count} documented, {above_count} documented "
          f"above, {raw_count} name-only ({len(entries)} raw entries in)")


if __name__ == "__main__":
    main()
