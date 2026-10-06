#!/usr/bin/env python3
"""Measure the three LLM-facing guides in tokens and gate the medium ceiling.

Run by CTest as `aether_guide_tokens`. The whole document is measured, front
matter and code fences included, because that is what a harness sends.

The gate is staged (owner decision D6, 2026-10-06):

  * pre-G1 (now): FAIL if the medium guide is over 15,000 o200k_base tokens.
    Every other number -- small, full, and every non-o200k tokenizer -- is
    reported, never gated.
  * g1 (from the first guide-pass commit): the medium ceiling is counted in
    the largest of the cached cohort tokenizers (max(o200k, Qwen3.5, ...)
    <= 15,000). When only o200k is cached the fallback is o200k <= 14,100,
    since o200k undercounts the Qwen cohort by ~6%. Flip STAGE below in that
    commit; `--stage g1` previews it.

The core/library split (Files, HTTP, Sockets, Tasks/AI and the appendices
count as library) is reported only; it is never gated.

Tokenizers are not vendored. o200k_base comes from tiktoken's cache
(TIKTOKEN_CACHE_DIR, else $TOKENIZER_DIR/tiktoken, else tiktoken's default
temp cache); Qwen3.5 is read from $TOKENIZER_DIR/qwen3.5/tokenizer.json.
TOKENIZER_DIR defaults to ~/.cache/aether-tokenizers, which is where
tools/fetch_tokenizers.sh puts both, sha256-checked. The Qwen count uses the
`tokenizers` package when it is installed, and otherwise the same vocabulary
and split regex loaded into tiktoken's BPE (which reproduces the reference
counts exactly on these guides); the method is printed with the count.

Nothing is downloaded unless --allow-download is given: a missing o200k cache
is a skip, not a network fetch.

Exit status: 0 pass, 1 medium over its ceiling, 2 usage or I/O error,
77 (CTest SKIP_RETURN_CODE) when tiktoken or the o200k_base file is missing.

Usage:
    python3 tools/check_guide_tokens.py [--docs DIR] [--sizes-out FILE]
                                        [--stage pre-g1|g1] [--allow-download]
"""
import argparse
import datetime
import hashlib
import json
import os
import re
import sys
import unicodedata

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Flip to "g1" in the first guide-pass commit (D6).
STAGE = "pre-g1"

CEILING = 15000          # medium, whole document
G1_O200K_FALLBACK = 14100  # medium under g1 when o200k is the only tokenizer cached

GUIDES = (
    ("small", "aether_for_llms_with_small_contexts.md"),
    ("medium", "aether_for_llms_medium_contexts.md"),
    ("full", "aether_for_llms_and_others.md"),
)

O200K_URL = "https://openaipublic.blob.core.windows.net/encodings/o200k_base.tiktoken"
O200K_SHA256 = "446a9538cb6c348e3516120d7c08b09f57c36495e2acfffe59a5bf8b0cfb1a2d"

# A `## ` section whose heading starts with one of these is library reference
# rather than core language. Everything else is core.
LIBRARY_HEADING = re.compile(r"^##\s+(Files\b|HTTP\b|Sockets\b|Tasks\b|Appendix\b)")
STAMP = re.compile(r"^\*Guide version:\s*(\d{4}-\d{2}-\d{2}-\d+)\*[ \t]*$", re.M)

SKIP = 77


def skip(msg):
    print(f"SKIP: {msg}")
    sys.exit(SKIP)


def tokenizer_dir():
    return os.environ.get("TOKENIZER_DIR") or os.path.join(
        os.path.expanduser("~"), ".cache", "aether-tokenizers")


def tiktoken_cache_dir():
    """Mirror tiktoken's own lookup so the cache can be checked before use."""
    if "TIKTOKEN_CACHE_DIR" not in os.environ and "DATA_GYM_CACHE_DIR" not in os.environ:
        fetched = os.path.join(tokenizer_dir(), "tiktoken")
        if os.path.isdir(fetched):
            os.environ["TIKTOKEN_CACHE_DIR"] = fetched
    if "TIKTOKEN_CACHE_DIR" in os.environ:
        return os.environ["TIKTOKEN_CACHE_DIR"]
    if "DATA_GYM_CACHE_DIR" in os.environ:
        return os.environ["DATA_GYM_CACHE_DIR"]
    import tempfile
    return os.path.join(tempfile.gettempdir(), "data-gym-cache")


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def load_o200k(allow_download):
    try:
        import tiktoken
    except ImportError:
        skip("tiktoken is not installed (pip install -r tools/requirements-docs.txt)")
    cache = tiktoken_cache_dir()
    cached = os.path.join(cache, hashlib.sha1(O200K_URL.encode()).hexdigest())
    if not allow_download:
        if not os.path.exists(cached):
            skip(f"o200k_base is not cached under {cache}; run tools/fetch_tokenizers.sh "
                 "or pass --allow-download")
        if sha256_file(cached) != O200K_SHA256:
            skip(f"cached o200k_base at {cached} does not match its pinned sha256")
    try:
        return tiktoken.get_encoding("o200k_base")
    except Exception as exc:  # network or hash failure inside tiktoken
        skip(f"o200k_base could not be loaded: {exc}")


def load_qwen():
    """Return (count_fn, label) for Qwen3.5, or (None, reason)."""
    path = os.path.join(tokenizer_dir(), "qwen3.5", "tokenizer.json")
    if not os.path.exists(path):
        return None, f"no Qwen3.5 tokenizer at {path}"
    digest = sha256_file(path)[:12]
    try:
        from tokenizers import Tokenizer
        tok = Tokenizer.from_file(path)
        return (lambda s: len(tok.encode(s, add_special_tokens=False).ids),
                f"tokenizers, sha256 {digest}")
    except ImportError:
        pass
    except Exception as exc:
        return None, f"tokenizers could not load {path}: {exc}"
    # Fallback: the tokenizer.json's byte-level BPE loaded into tiktoken.
    # Vocabulary ids double as merge ranks for this tokenizer, so the encoding
    # is the same; NFC is the tokenizer's own normalizer.
    try:
        import tiktoken
        with open(path, encoding="utf-8") as fh:
            spec = json.load(fh)
        printable = (list(range(ord("!"), ord("~") + 1)) + list(range(ord("¡"), ord("¬") + 1))
                     + list(range(ord("®"), ord("ÿ") + 1)))
        byte_of, extra = {}, 0
        for b in range(256):
            if b in printable:
                byte_of[chr(b)] = b
            else:
                byte_of[chr(256 + extra)] = b
                extra += 1
        ranks = {}
        for piece, rank in spec["model"]["vocab"].items():
            try:
                ranks[bytes(byte_of[ch] for ch in piece)] = rank
            except KeyError:
                pass
        pattern = spec["pre_tokenizer"]["pretokenizers"][0]["pattern"]["Regex"]
        enc = tiktoken.Encoding(name="qwen3.5", pat_str=pattern, mergeable_ranks=ranks,
                                special_tokens={})
        return (lambda s: len(enc.encode(unicodedata.normalize("NFC", s), disallowed_special=())),
                f"tiktoken port of tokenizer.json, sha256 {digest}")
    except Exception as exc:
        return None, f"could not load {path} without the tokenizers package: {exc}"


def split_core_library(text):
    core, library, current = [], [], None
    for line in text.split("\n"):
        if line.startswith("## "):
            current = library if LIBRARY_HEADING.match(line) else core
        (current if current is not None else core).append(line)
    return "\n".join(core), "\n".join(library)


def fmt(n):
    return "n/a" if n is None else f"{n:,}"


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--docs", default=os.path.join(REPO, "docs"),
                    help="directory holding the three guides (default: the repo's docs/)")
    ap.add_argument("--sizes-out", default=os.path.join(REPO, "build", "guide_sizes.md"),
                    help="where to write the measured table (default: build/guide_sizes.md)")
    ap.add_argument("--stage", choices=("pre-g1", "g1"), default=STAGE,
                    help=f"ceiling rule to apply (default: {STAGE})")
    ap.add_argument("--allow-download", action="store_true",
                    help="let tiktoken fetch o200k_base if it is not cached")
    args = ap.parse_args()

    texts = {}
    for role, name in GUIDES:
        path = os.path.join(args.docs, name)
        try:
            with open(path, encoding="utf-8") as fh:
                texts[role] = fh.read()
        except OSError as exc:
            print(f"ERROR: cannot read {path}: {exc}")
            return 2

    o200k = load_o200k(args.allow_download)
    qwen, qwen_label = load_qwen()

    rows = {}
    for role, name in GUIDES:
        text = texts[role]
        core, library = split_core_library(text)
        stamp = STAMP.search(text)
        rows[role] = {
            "name": name,
            "stamp": stamp.group(1) if stamp else "(none)",
            "o200k": len(o200k.encode(text, disallowed_special=())),
            "qwen": qwen(text) if qwen else None,
            "core": len(o200k.encode(core, disallowed_special=())),
            "library": len(o200k.encode(library, disallowed_special=())) if library else 0,
        }

    med = rows["medium"]
    if args.stage == "pre-g1":
        gated_value, rule = med["o200k"], f"o200k <= {CEILING:,}"
        limit = CEILING
    else:
        cohort = [v for v in (med["o200k"], med["qwen"]) if v is not None]
        if med["qwen"] is None:
            gated_value, rule, limit = med["o200k"], f"o200k <= {G1_O200K_FALLBACK:,} (only o200k cached)", G1_O200K_FALLBACK
        else:
            gated_value, rule, limit = max(cohort), f"max(o200k, qwen3.5) <= {CEILING:,}", CEILING
    over = gated_value > limit

    print(f"guide token counts (whole document; stage {args.stage}, D6)")
    print(f"  {'guide':<7} {'stamp':<13} {'o200k':>8} {'qwen3.5':>8}  ceiling")
    for role, _ in GUIDES:
        r = rows[role]
        if role == "medium":
            verdict = "FAIL" if over else "PASS"
            note = f"{rule}: {verdict} ({limit - gated_value:+,} headroom)"
        else:
            note = "reported only"
        print(f"  {role:<7} {r['stamp']:<13} {fmt(r['o200k']):>8} {fmt(r['qwen']):>8}  {note}")
    print(f"  qwen3.5: {qwen_label}")
    print("  core/library split, o200k (reported only):")
    for role, _ in GUIDES:
        r = rows[role]
        print(f"    {role:<7} core {fmt(r['core']):>7}  library {fmt(r['library']):>7}")

    warnings = []
    if med["qwen"] is not None and med["qwen"] > CEILING and args.stage == "pre-g1":
        warnings.append(f"medium is {med['qwen']:,} qwen3.5 tokens, over {CEILING:,}; "
                        "reported only until G1 (D6), when max(cohort) becomes the gate")
    for w in warnings:
        print(f"WARN: {w}")

    try:
        out_dir = os.path.dirname(os.path.abspath(args.sizes_out))
        os.makedirs(out_dir, exist_ok=True)
        with open(args.sizes_out, "w", encoding="utf-8") as fh:
            fh.write("<!-- Generated by tools/check_guide_tokens.py; do not edit. -->\n\n")
            fh.write("| Guide | Stamp | o200k | Qwen3.5 | Core | Library | Ceiling |\n")
            fh.write("|---|---|---:|---:|---:|---:|---|\n")
            for role, _ in GUIDES:
                r = rows[role]
                ceil = rule if role == "medium" else "none (reported)"
                fh.write(f"| {role} | `{r['stamp']}` | {fmt(r['o200k'])} | {fmt(r['qwen'])} | "
                         f"{fmt(r['core'])} | {fmt(r['library'])} | {ceil} |\n")
            fh.write(f"\nMeasured {datetime.date.today().isoformat()}, whole documents, "
                     f"o200k_base (sha256 {O200K_SHA256[:12]}); Qwen3.5: {qwen_label}.\n")
        print(f"  wrote {args.sizes_out}")
    except OSError as exc:
        print(f"WARN: could not write {args.sizes_out}: {exc}")

    if over:
        print(f"FAIL: medium is {gated_value:,} tokens under '{rule}'. Cut, do not creep: "
              "pair every addition with a named cut (docs/aether_doc_maintenance.md).")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
