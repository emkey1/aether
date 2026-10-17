#!/usr/bin/env python3
"""Rewrite Pascal-era Aether spellings to the modern surface (D55, 2026-10-17).

The mechanical cases are rewritten in place; the rest are reported, one line
per site, for a hand migration (they change a program's shape: the File
sequence becomes read_file/write_file, val's output parameters become a tuple).

Rewritten:
    itoa(n), inttostr(n), IntToStr(n)   -> int_to_text(n)
    copy(s, a, n)                       -> s[a..a + n]   (s[..n] when a is 0,
                                           s[a..] when n is length(s) - a)
    pos(needle, s)                      -> index_of(s, needle)
    stringofchar(c, n)                  -> repeat(c, n)
    upcase(x), toupper(x)               -> upper(x)      (whole-string now, D21)
    paramcount()                        -> length(args())
    paramstr(i)                         -> args()[i - 1]  (paramstr(0) -> "")
    mstreamcreate / mstreamfromstring / mstreambuffer / mstreamappendbyte /
    mstreamsavetofile / mstreamfree     -> mstream_new / mstream_from_text /
                                           mstream_text / mstream_append_byte /
                                           mstream_save / mstream_free
Reported (hand migration): val, valreal, the File sequence (assign, reset,
rewrite, append, close, eof, erase, rename, `: File`), readln/read,
gettime/getdate, setlength, mstreamloadfromfile.

A name the source declares itself (`fn copy(...)`) is left alone, as is a
method call (`x.copy(...)`) and anything inside a string or comment.

Usage:
    python3 tools/modernize_surface.py [--write] [--json-field FIELD ...] PATH...

PATH may be an .aether file (or an extensionless example), a .md file (only
```aether fenced blocks are touched), or a .json file (string fields named by
--json-field, default reference_solution/solution/fixed_source/broken_source,
holding Aether source). Without --write it prints what it would change.
Exit status: 0, or 1 when a hand-migration site remains.
"""
import argparse
import json
import os
import re
import sys

IDENT = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")

RENAMES = {
    "itoa": "int_to_text",
    "inttostr": "int_to_text",
    "stringofchar": "repeat",
    "upcase": "upper",
    "toupper": "upper",
    "mstreamcreate": "mstream_new",
    "mstreamfromstring": "mstream_from_text",
    "mstreambuffer": "mstream_text",
    "mstreamappendbyte": "mstream_append_byte",
    "mstreamsavetofile": "mstream_save",
    "mstreamfree": "mstream_free",
}
HAND = {
    "val": "let (n, ok) = try_parse_int(t);",
    "valreal": "let (x, ok) = try_parse_real(t);",
    "assign": "read_file / read_lines / write_file / append_file",
    "assignfile": "read_file / read_lines / write_file / append_file",
    "reset": "read_file / read_lines",
    "rewrite": "write_file",
    "append": "append_file(path, text) or xs = xs + [v]",
    "close": "(no handle to close)",
    "closefile": "(no handle to close)",
    "eof": "read_all_stdin / read_lines",
    "readln": "read_line() / read_all_stdin()",
    "read": "read_line()",
    "erase": "delete_file(path)",
    "rename": "rename_file(a, b)",
    "gettime": "clock_ms() / now_text()",
    "getdate": "now_text()",
    "setlength": "xs[..n] / xs + [v]",
    "mstreamloadfromfile": "ms = mstream_load(path)",
}
DEFAULT_FIELDS = ("reference_solution", "solution", "fixed_source", "broken_source")


def code_mask(src):
    """A same-length string with string literals and comments blanked."""
    out = list(src)
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if src.startswith("//", i):
            j = src.find("\n", i)
            j = n if j < 0 else j
            for k in range(i, j):
                out[k] = " "
            i = j
        elif src.startswith("/*", i):
            j = src.find("*/", i + 2)
            j = n if j < 0 else j + 2
            for k in range(i, j):
                if out[k] != "\n":
                    out[k] = " "
            i = j
        elif c == '"' or c == "'":
            q = c
            j = i + 1
            while j < n and src[j] != q and src[j] != "\n":
                if src[j] == "\\":
                    j += 1
                j += 1
            for k in range(i + 1, min(j, n)):
                out[k] = "~"
            i = j + 1
        else:
            i += 1
    return "".join(out)


def split_args(src, mask, open_idx):
    """Arguments of the call whose '(' is at open_idx: (list of (start, end)),
    index of the closing ')'. None when unbalanced."""
    depth = 0
    args = []
    start = open_idx + 1
    i = open_idx
    while i < len(src):
        c = mask[i]
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
            if depth == 0:
                args.append((start, i))
                return args, i
        elif c == "," and depth == 1:
            args.append((start, i))
            start = i + 1
        i += 1
    return None, None


def is_simple(expr):
    """An expression that can take a slice suffix without parentheses."""
    e = expr.strip()
    return re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*(\.[A-Za-z_][A-Za-z0-9_]*|\[[^\[\]]*\]|\([^()]*\))*", e) is not None


def needs_parens(expr):
    """True when expr has a top-level operator that binds looser than `+`."""
    e = expr.strip()
    depth = 0
    i = 0
    while i < len(e):
        c = e[i]
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
        elif depth == 0:
            if e.startswith(("==", "!=", "<=", ">=", "&&", "||", ".."), i) or c in "<>?:":
                return True
            if re.match(r"\b(and|or|not|xor)\b", e[i:]) and (i == 0 or not e[i - 1].isalnum()):
                return True
        i += 1
    return False


def is_int(s):
    return re.fullmatch(r"-?\d+", s.strip()) is not None


def copy_to_slice(base, a, n):
    # Only a name, field, index or call takes a slice suffix (`"lit"[a..b]`
    # and `(e)[a..b]` do not parse), so modernize() leaves other bases to a
    # hand edit: bind the value with `let` first.
    base, a, n = base.strip(), a.strip(), n.strip()
    ap = "(" + a + ")" if needs_parens(a) else a
    npar = "(" + n + ")" if needs_parens(n) else n
    if a == "0":
        return f"{base}[..{n}]"
    m = re.fullmatch(r"(?:length|len)\((.*)\)\s*-\s*(.+)", n)
    if m and m.group(1).strip() == base.strip("()").strip() and m.group(2).strip() == a:
        return f"{base}[{a}..]"
    if is_int(a) and is_int(n):
        return f"{base}[{a}..{int(a) + int(n)}]"
    if is_int(n) and int(n) == 0:
        return f'""'
    # `n` may start with a unary minus: a + -1 is still a + n.
    if npar.startswith("-"):
        npar = "(" + npar + ")"
    return f"{base}[{ap}..{ap} + {npar}]"


def declared_fns(mask):
    return {m.group(1).lower() for m in re.finditer(r"\bfn\s+([A-Za-z_][A-Za-z0-9_]*)", mask)}


def modernize(src):
    """Return (new_src, notes) where notes are (line, name, advice) for sites
    needing a hand migration."""
    notes = []
    changed = True
    # Iterate to a fixed point: nested calls (copy(itoa(n), 0, 2)) rewrite
    # from the inside out across passes.
    passes = 0
    while changed and passes < 20:
        passes += 1
        changed = False
        mask = code_mask(src)
        own = declared_fns(mask)
        for m in IDENT.finditer(mask):
            name = m.group(0)
            low = name.lower()
            s, e = m.start(), m.end()
            if s > 0 and (mask[s - 1] == "." or mask[s - 1].isalnum() or mask[s - 1] == "_"):
                continue
            j = e
            while j < len(mask) and mask[j] in " \t":
                j += 1
            if j >= len(mask) or mask[j] != "(":
                continue
            if low in own:
                continue
            pre = mask[max(0, s - 3):s]
            if re.search(r"\bfn\s*$", mask[max(0, s - 8):s]):
                continue
            args, close = split_args(src, mask, j)
            if args is None:
                continue
            argtext = [src[a:b] for a, b in args]
            if len(argtext) == 1 and argtext[0].strip() == "":
                argtext = []
            rep = None
            if low in ("itoa", "inttostr"):
                rep = "int_to_text(" + ",".join(argtext) + ")"
            elif low == "copy" and len(argtext) == 3 and is_simple(argtext[0]):
                rep = copy_to_slice(*argtext)
            elif low == "pos" and len(argtext) == 2:
                rep = f"index_of({argtext[1].strip()}, {argtext[0].strip()})"
            elif low == "paramcount" and not argtext:
                rep = "length(args())"
            elif low == "paramstr" and len(argtext) == 1:
                i = argtext[0].strip()
                # paramstr(0) was always "" (the engine never stored the
                # program name); args() starts at the first argument.
                if is_int(i) and int(i) == 0:
                    rep = '""'
                else:
                    rep = f"args()[{int(i) - 1}]" if is_int(i) else f"args()[{i} - 1]"
            elif low in RENAMES:
                rep = RENAMES[low] + "(" + ",".join(argtext) + ")"
            if rep is not None:
                src = src[:s] + rep + src[close + 1:]
                changed = True
                break
    mask = code_mask(src)
    own = declared_fns(mask)
    for m in IDENT.finditer(mask):
        name = m.group(0)
        low = name.lower()
        s, e = m.start(), m.end()
        if s > 0 and (mask[s - 1] == "." or mask[s - 1].isalnum() or mask[s - 1] == "_"):
            continue
        j = e
        while j < len(mask) and mask[j] in " \t":
            j += 1
        line = src.count("\n", 0, s) + 1
        if j < len(mask) and mask[j] == "(" and low == "copy" and low not in own:
            notes.append((line, name, "bind the text with `let` first, then slice it"))
        elif j < len(mask) and mask[j] == "(" and low in HAND and low not in own:
            if re.search(r"\bfn\s*$", mask[max(0, s - 8):s]):
                continue
            notes.append((line, name, HAND[low]))
        elif name == "File" and re.search(r":\s*$", mask[max(0, s - 4):s]):
            notes.append((line, "File", "read_file / write_file"))
    return src, notes


FENCE = re.compile(r"(^```aether[^\n]*\n)(.*?)(^```)", re.S | re.M)


def process_text(path, text, kind, fields):
    notes = []
    if kind == "md":
        def sub(m):
            new, n = modernize(m.group(2))
            base = text.count("\n", 0, m.start(2))
            notes.extend((base + ln, nm, adv) for ln, nm, adv in n)
            return m.group(1) + new + m.group(3)
        return FENCE.sub(sub, text), notes
    if kind == "json":
        # Edit the raw text, one field value at a time, so the rest of the
        # file (key order, escapes, indentation) stays byte for byte.
        data = json.loads(text)
        out = text

        def walk(obj, where):
            nonlocal out
            if isinstance(obj, dict):
                for k, v in obj.items():
                    if k in fields and isinstance(v, str):
                        new, n = modernize(v)
                        tag = f"{where}{obj.get('id', '')}.{k}"
                        notes.extend((f"{tag}:{ln}", nm, adv) for ln, nm, adv in n)
                        if new != v:
                            for ascii_ in (False, True):
                                old_enc = json.dumps(v, ensure_ascii=ascii_)
                                if old_enc in out:
                                    out = out.replace(old_enc, json.dumps(new, ensure_ascii=ascii_), 1)
                                    break
                            else:
                                notes.append((tag, "?", "could not locate the field text to edit"))
                    elif isinstance(v, (dict, list)):
                        walk(v, where)
            elif isinstance(obj, list):
                for v in obj:
                    walk(v, where)
        walk(data, "")
        return out, notes
    new, n = modernize(text)
    return new, n


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("paths", nargs="+")
    ap.add_argument("--write", action="store_true")
    ap.add_argument("--json-field", action="append", dest="fields")
    args = ap.parse_args(argv)
    fields = set(args.fields or DEFAULT_FIELDS)
    pending = 0
    for path in args.paths:
        kind = "md" if path.endswith(".md") else "json" if path.endswith(".json") else "src"
        with open(path, encoding="utf-8") as fh:
            text = fh.read()
        new, notes = process_text(path, text, kind, fields)
        for ln, nm, adv in notes:
            print(f"{path}:{ln}: hand: {nm} -> {adv}")
        pending += len(notes)
        if new != text:
            print(f"{path}: rewritten" if args.write else f"{path}: would rewrite")
            if args.write:
                with open(path, "w", encoding="utf-8") as fh:
                    fh.write(new)
    return 1 if pending else 0


if __name__ == "__main__":
    sys.exit(main())
