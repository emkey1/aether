# `\u{1F600}` Text escapes decode to UTF-8 (D79 evidence)

Language 2026-10-21-7 (1.0-beta4dev). The census was taken on the tree just
before the change (language 2026-10-21-6, D78 at 61f6ba1), and the paired replay
on the new binary and on D78's.

## The behaviour

D78 made `\uXXXX` and `\UXXXXXXXX` decode and left the braced spelling alone:
`\u` followed by `{` is not 4 hex digits, so the escape was unknown and kept as
written. `println("\u{1F600}")` printed the 9 characters `\u{1F600}`, and
`length("\u{1F600}")` was 9, with no diagnostic. Rust (1 to 6 hex digits), Swift
(1 to 8) and JavaScript (any count, value at most 10FFFF) all write a Unicode
escape this way and all mean one code point by it, so a model that writes it
means the character.

## Options

| Option | For | Against |
|---|---|---|
| **Decode** (chosen, owner) | the one meaning all three languages share; additive under D75; a class-1 synonym (rationale §5.7) costs a model nothing | a second spelling of `\U0001F600` |
| SYN-001 with a hint naming `\U0001F600` | one canonical spelling | a repair round on code with one obvious meaning; class 2 is for spellings Aether cannot give an exact meaning |
| Keep as written (D78) | no change | silently wrong |

## Census

`python3 results/decisions/d78_unicode_escape_census.py`, whose `brace` column
counts `\u{` (with the backslash doubled inside JSON), run on `git archive 61f6ba1`
of `tests examples docs training bench results`: 2,614 files.

| Escape | Hits before the change |
|---|---|
| `\u{...}` in a program, golden, seed pair, dataset or archived attempt | **0** |
| `\u{...}` in prose (D78's decision row and its evidence file) | 2 |

So no committed program changes output. After the change the hits are D79's
own fixtures, spec examples, manual lines, registry entry and decision prose
(79 in 2,620 files).

## Paired replay

`python3 tools/replay_bench.py --aether-bin build/aether` over the 36 tracked
reports (317 attempts, 257 distinct programs): **0 unwaived** regressions (44
gated, all waived under earlier declared breaks). The same replay on D78's
binary writes a `--report-json` identical in every field but the binary's hash
and the run time (tallies, flips, moves, waived and unwaived lists, task
classes), so no replayed result comes from this change.

## Shipped rule

- `\u{` + one or more hex digits + `}` writes that code point as UTF-8, with any
  digit count: `\u{e9}`, `\u{1F600}`, `\u{0001F600}`. The value only stops
  growing past U+10FFFF, so a long run cannot overflow.
- Braces hold a whole code point. A surrogate (`\u{D800}`) or a value past
  U+10FFFF (`\u{110000}`) is **SYN-001**, with a hint naming `\u{1F600}` or the
  character itself; `\u{D83D}\u{DE00}` does not join (Rust and Swift reject it
  as well). This half is D75's single exception: those spellings were silently
  kept as written before.
- Braces with no hex digits or no closing `}` stay an unknown escape kept as
  written: `\u{}`, `\u{name}`, `\u{41 `. A literal split by adjacency
  (`"\u{1F" "600}"`) is unescaped piece by piece, so it is kept as written too.
- `\U{...}` is not a spelling anywhere and stays unknown.

Fixtures: `tests/fx/basic/text_unicode_brace_escape_pass` and
`tests/fx/diag/text_unicode_brace_escape_fail`; surface registry entry
`unicode_brace_escape` (class 1, twin `\U0001F600`). Spec examples: `L.lit.12`
and `L.lit.13`.
