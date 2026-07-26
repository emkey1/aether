"""Compile-check every ```aether block in an LLM-facing guide.

Usage: python3 tools/verify_guide_snippets.py docs/<guide>.md
See docs/aether_doc_maintenance.md for why this exists.
"""
import re, subprocess, sys, os, tempfile

GUIDE = sys.argv[1]
AETHER = os.environ.get("AETHER_BIN",
                       os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "build", "aether"))
OUT = tempfile.mkdtemp(prefix="aether_guide_snip_")

# Blocks that MUST fail: negative examples and module imports with no module on disk.
# Keyed by a distinctive substring; value is why.
EXPECT_FAIL = {
    'toon_get_text(doc, "name");     // TOON-001': "negative example: getter on a ToonDoc",
    'use "bench_support";': "imports a module that is not on disk here",
}

CONTEXT = """
type Ctx { label: Text = ""; }

fn ctxDoc() -> ToonDoc { ret toon_parse("{\\"rows\\":[{\\"meta\\":{}}],\\"server\\":{}}"); }

fn __frag(ready: Bool, score: Int, index: Int, total: Int, count: Int,
          j: Int, id: Text, pct: Real, s: Text, url: Text, n: Int,
          xs: Int[], root: ToonNode, row: ToonNode, doc: ToonDoc) -> Void {
"""

text = open(GUIDE).read()
lines = text.split("\n")
blocks, cur, start = [], None, 0
for i, l in enumerate(lines, 1):
    if l.strip() == "```aether" and cur is None:
        cur, start = [], i
    elif l.strip() == "```" and cur is not None:
        blocks.append((start, "\n".join(cur)))
        cur = None
    elif cur is not None:
        cur.append(l)

def compile_src(src, tag):
    p = os.path.join(OUT, f"{tag}.aether")
    open(p, "w").write(src)
    r = subprocess.run([AETHER, "--no-cache", "--no-run", p],
                       capture_output=True, text=True, timeout=60)
    return r.returncode, (r.stdout + r.stderr).strip()

results = []
for k, (ln, src) in enumerate(blocks):
    why_fail = next((v for s, v in EXPECT_FAIL.items() if s in src), None)
    if re.search(r"\bfn\s+main\s*\(", src):
        kind, full = "whole", src
    elif re.search(r"^\s*(fn|type|mod|use|const|@)", src, re.M):
        kind, full = "decl", src + "\n\nfn main() -> Void {\n    ret;\n}\n"
    else:
        body = "\n".join("    " + x for x in src.split("\n"))
        kind = "frag"
        full = CONTEXT + body + "\n    ret;\n}\n\nfn main() -> Void {\n    ret;\n}\n"
    rc, out = compile_src(full, f"b{k:02d}_L{ln}")
    results.append((ln, kind, rc, out, src, why_fail))

unexpected = [r for r in results if (r[2] != 0) != (r[5] is not None)]
neg_ok = [r for r in results if r[5] and r[2] != 0]
print(f"blocks {len(results)}  compiled {sum(1 for r in results if r[2]==0)}  "
      f"expected-fail verified {len(neg_ok)}  UNEXPECTED {len(unexpected)}\n")
for ln, kind, rc, out, src, why in unexpected:
    tag = "FAILED but should compile" if rc != 0 else f"COMPILED but expected to fail ({why})"
    print(f"--- line {ln} ({kind}): {tag} ---")
    print(out[:500])
    print("    " + "\n    ".join(src.split("\n")[:8]))
    print()

sys.exit(1 if unexpected else 0)
