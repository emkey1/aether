#!/usr/bin/env python3
"""Count archived model-written programs that use a form D90 to D96 change.

    python3 results/decisions/silent_wrong_census.py .

Reads every attempt's source_code in the reports under bench/results (packed
or plain) and every program in tests/replay, once per distinct program.
"""
import gzip, json, glob, hashlib, re, os, sys, collections
root = sys.argv[1]
pats = {
 "toon/mstream binding in a let that does not start its line": re.compile(r'[;{][ \t]*let[ \t]+\w+[ \t]*(?::[ \t]*[\w\[\]]+)?[ \t]*=[ \t]*(?:toon_(?:parse|parse_file|root|key|at|get)\b|mstream_(?:new|from_text)\b)'),
 "  ... of which the type is inferred (no annotation)": re.compile(r'[;{][ \t]*let[ \t]+\w+[ \t]*=[ \t]*(?:toon_(?:parse|parse_file|root|key|at|get)\b|mstream_(?:new|from_text)\b)'),
 "any toon/mstream let (for scale)": re.compile(r'let[ \t]+\w+[ \t]*(?::[ \t]*[\w\[\]]+)?[ \t]*=[ \t]*(?:toon_(?:parse|parse_file|root|key|at|get)\b|mstream_(?:new|from_text)\b)'),
 "\\xNN escape at or above 0x80": re.compile(r'\\x[89a-fA-F][0-9a-fA-F]'),
 "negative literal slice bound": re.compile(r'\[\s*-\s*\d+\s*\.\.|\.\.\s*-\s*\d+\s*\]'),
 "round(x, n) two-argument": re.compile(r'\bround\(\s*[^(),]+,\s*[^()]+\)'),
 "Int let from a Real literal >= 1e19": re.compile(r':\s*Int\s*=\s*\d(?:\.\d+)?e(?:19|[2-9]\d)\b'),
}
seen = set(); hits = collections.Counter(); passing = collections.Counter(); ex = {}
files = glob.glob(os.path.join(root, "bench/results/**/*.json.gz"), recursive=True) + glob.glob(os.path.join(root, "bench/results/**/*.json"), recursive=True)
n_att = 0
def walk(o):
    global n_att
    if isinstance(o, dict):
        src = o.get("source_code")
        if isinstance(src, str) and src.strip():
            h = hashlib.sha1(src.encode()).hexdigest()
            if h not in seen:
                seen.add(h); n_att += 1
                ok = bool((o.get("run") or {}).get("exact_stdout_match"))
                for k, p in pats.items():
                    m = p.search(src)
                    if m:
                        hits[k] += 1; passing[k] += ok
                        ex.setdefault(k, src[max(0, m.start()-60):m.end()+40].replace("\n", " "))
        for v in o.values(): walk(v)
    elif isinstance(o, list):
        for v in o: walk(v)
for f in files:
    try:
        d = json.loads((gzip.open(f, "rt", encoding="utf-8") if f.endswith(".gz") else open(f, encoding="utf-8")).read())
    except Exception as e:
        continue
    walk(d)
# replay programs and the test corpus
extra = glob.glob(os.path.join(root, "tests/replay/**/*.aether"), recursive=True)
for f in extra:
    src = open(f, encoding="utf-8", errors="replace").read()
    for k, p in pats.items():
        if p.search(src): hits[k + " [tests/replay]"] += 1
print("reports", len(files), "distinct programs", n_att, "replay programs", len(extra))
for k in pats:
    print(f"{hits[k]:5d} programs ({passing[k]} of them scored exact)  {k}")
    if k in ex: print("        e.g.", ex[k][:200])
for k in hits:
    if k.endswith("[tests/replay]"): print(hits[k], k)
