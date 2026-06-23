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
