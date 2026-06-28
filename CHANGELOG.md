# Aether language changelog

Aether language versions use the date-stamp scheme `YYYY-MM-DD-N` (the same scheme
as the guides): the date of the last language-affecting change, with `N`
distinguishing multiple such changes on a single day. The value lives in
[`VERSION`](VERSION), is reported by `aether --version`, and is stamped into every
benchmark result for traceability.

Bump it with `tools/bump_version.py` **only** when a change alters what programs
compile or how they run — a syntax, semantic, or builtin change — and never on a
plain rebuild. Because the stamp is checked in, every node that builds a given
commit reports the same version, so a real mismatch between nodes means one is
genuinely behind. Each bump should add an entry below.

## 2026-06-27-2

**AST frontend is now the default (P7 cutover).** `parseAether()` parses straight
to the shared AST by default; the legacy text rewriter is retained as a runtime-
reversible fallback via `AETHER_PARSER=rewriter` (or `=legacy`), and
`AETHER_PARSER=ast` remains an explicit synonym for the default. Gate met:
RW-vs-AST parity 1819/1851 on the model corpus with **zero AST-worse** (the 32
divergences are AST-better or out-differs, where the AST path is the more correct
one -- correct error line numbers, no compound-line mis-parses). Three negative-case
diagnostics were aligned to the rewriter's wording so the curated suite stays green
under the new default: the declaration `cannot infer the type of '<x>'` error
(mixed string/number initializers like `tag + 1` now bail instead of mis-inferring
Int), the `type fields must end with ';', not ','.` type error, and the
`non-Void functions have a fallthrough path with no return value.` FLOW-001 check.

## 2026-06-27-1

**Digit separators in numeric literals.** Underscores may now appear between
digits in integer, hexadecimal, and floating-point literals (`1_000_000`,
`0xFF_FF`, `3.141_592`); they are cosmetic and stripped before the value is
parsed. A `_` is only part of the number when it sits between two digits, so a
leading/trailing `_` still begins/ends a separate token. Both frontends (the
legacy rewriter and the AST parser) accept them, via the shared rea lexer.

## 2026-06-26-1

**Compound-line parsing** (frontend rewriter): multiple constructs sharing a
single physical line now parse correctly. Previously a `type` field sharing a
line with the next method (`budgeted: Real;fn ingest(...)`) silently dropped the
method, orphaning its body, and a function body inlined on its signature line
(`fn main() { let x: T = ... }`) was left untranslated — both surfaced as
misleading `SYN-001` errors (`Unexpected token ELSE` / `COLON`) at the wrong
line. The Aether-to-Rea rewriter now normalizes run-on `type` members and inline
`fn`/`type` bodies onto their own lines before lowering. Value braces
(`new T {}`), if-expressions (`if c { a } else { b }`), and control one-liners
are left untouched, so already-canonical code is unaffected.

## 2026-06-23-1

First versioned release; baseline of the current language. Notable recent
language-affecting fixes already folded into this baseline:

- **Method-to-method receiver fix** (pscal-core): a method that calls another
  method on `self` while passing an argument no longer passes the receiver onto
  the first parameter. Restored quicksort and other method-heavy programs.
- **2D chained array indexing** (`dp[i][j]`) now returns the element rather than
  the inner row.
- **Integer-recursion argument coercion** (`n % 2`, `n / 2`): integer arguments
  to recursive calls no longer coerce through Real, fixing collatz-style programs.
