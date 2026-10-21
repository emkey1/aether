# `\u` and `\U` Text escapes decode to UTF-8 (D78 evidence)

Language 2026-10-21-6 (1.0-beta4dev). The census was taken on the tree just
before the change (language 2026-10-21-5, with the human manual), and the
paired replay on the new binary.

## The bug

The spec (§1.3) and the full guide's Lexical basics list `\uXXXX` as a Text
escape. The front end's unescaper (`aetherUnescapeString` in
`src/aether/ast_parser.c`, copied from Rea's) decoded `\n`, `\t`, `\r`, `\"`,
`\\`, `\'`, `\xNN` and octal, and kept every other escape as written. So
`println("caf\u00e9")` printed `caf\u00e9` and `length("caf\u00e9")` was 9, with
no diagnostic: a program written to the spec was silently wrong. No test covered
the escape. The human manual's author found it while checking every escape
against `./build/aether`.

## Census

`python3 results/decisions/d78_unicode_escape_census.py`, run from the root of
the tree before the change, scans 2,610 files: `.aether`, `.md` and `.out`
directly, and `.json`, `.jsonl` and `.gz` with the backslash doubled as JSON
writes it. It covers tests, examples, docs, training pairs and datasets,
`bench/` and every archived benchmark report under `bench/results` and
`results/`.

| Escape | Programs and docs holding one |
|---|---|
| `\u` + 4 hex or `\U` + 8 hex (meaning changes) | **0** |
| `\u{...}` (Rust / JavaScript spelling, unchanged) | 0 |

So no committed program, golden, seed pair or archived attempt changes output.
After the change the only hits are D78's own spec examples, fixtures and
manual lines.

## Paired replay

`python3 tools/replay_bench.py --aether-bin build/aether` over the 36 tracked
reports (317 attempts, 257 distinct programs): **0 unwaived** regressions
(44 gated, all waived under earlier declared breaks). None of the replayed
programs holds the escape, so none of their results can come from this change.

## Shipped rule

- `\uXXXX` (exactly 4 hex digits) and `\UXXXXXXXX` (exactly 8) write the code
  point as UTF-8. Text indexing and `length` count code points, so `"\u00e9"` is
  one character.
- A high surrogate followed by a `\u` low surrogate (`\uD83D\uDE00`, the JSON
  and Java spelling) joins into one character.
- With fewer hex digits the escape is unknown and stays as written, as before.
  `"C:\users"` and `"\u12"` are unchanged.
- An escape with all its digits that names no character is **SYN-001**: a lone
  surrogate (`\uD800`) or a value past U+10FFFF (`\U00110000`). Before, it was
  kept as written. The hint names the fixes: the whole pair, the 8-digit code
  point, or the character itself.

Fixtures: `tests/fx/basic/text_unicode_escape_pass` and
`tests/fx/diag/text_unicode_escape_fail`. Spec examples: `L.lit.10` and
`L.lit.11`.
