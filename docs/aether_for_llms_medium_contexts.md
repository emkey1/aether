# Aether for LLMs — Working Guide (for medium contexts)

*Guide version: 2026-10-18-1*

Everything needed to write correct Aether in one shot.

## Highest-Value Rules

1. **FX-001.** Every effectful builtin must sit inside `fx { ... }`: output
   (`print`, `println`), task helpers, `ai_chat`, and all host interaction —
   files (`read_file`, `write_file`, `file_exists`), stdin (`read_line`), env
   (`getenv`), CLI (`args`), `random`/`randomize`, clock (`clock_ms`), `http*`,
   `socket*`, `sqlite*`. Pure math, Text, array and conversion helpers do not need `fx`.
   ✗ `println("hi");` at function scope   ✓ `fx { println("hi"); }`
   ✗ `let doc: ToonDoc = toon_parse_file(p);` — reading a file is an effect
   ✓ `let doc: ToonDoc; fx { doc = toon_parse_file(p); }`
2. **SYN-001.** Aether keywords only: `fn`, `let`, `const`, `ret`, `if`, `loop`,
   `type`, `mod`, `use`, `new`, `par`, `fx`, `break`, `continue`.
   ✗ `return`, `class`, `var`, `def`, `func`, `=>`, Python-style colons.
   `while cond { }` and `for x in a..b { }` do compile, but `loop` is canonical
   and covers every form they do.
3. **BUILT-001.** Every builtin listed in this document exists with the
   signature shown. Prefer them, and do not guess a name beyond them.
   ✗ `substring(s, 0, 3)`, `to_upper(s)`, `s.upper()` — write `s[0..3]`, `upper(s)`
4. **BUILT-002.** Right name, wrong argument count is its own compile-time error
   for the fixed-arity conversion and math helpers.
   ✗ `formatfloat(r, 0, 2)` — it takes `(value, precision)`; the width slot
   belongs only to the `println`-only `r:0:prec` form.
5. **TYPE-001.** An array literal needs a declared type. Annotate any binding
   whose type is not obvious.
   ✗ `let xs = [1, 2, 3];` (cannot infer)  ✓ `let xs: Int[] = [1, 2, 3];`
6. **TOON-001.** `ToonDoc` and `ToonNode` are opaque handles: no arithmetic, and
   never assign one where the other is expected.
   ✗ `toon_get_text(doc, "name")` — getters take a `ToonNode`, so go through
   `toon_root(doc)` first.
7. **ROOT-001.** If the JSON starts with `{`, extract the named array with
   `toon_key(root, "...")` before iterating. Only iterate `root` directly when
   the JSON starts with `[`.
8. **ANN-001.** `@pre`, `@post`, `@pure`, `@cost` go directly above the function
   — never inside the body, never bare with no expression.
9. **MUT-001.** A plain `let` is already mutable. ✗ `let mut x: Int = 0;`
10. **LEN-001.** `toon_len(node)` for TOON arrays; `length(xs)` for dynamic
    arrays and `Text`. Crossing them compiles, then fails at run time with no
    code.
11. **FUNC-001.** Functions are not values. No anonymous `fn`, no lambdas,
    never pass a function as an argument, and there is no
    `map`/`filter`/`reduce` — write a `loop`. To run your own code concurrently
    use `par { f(); g(); }`. A `fn` inside a free function's body may read and
    assign that function's parameters and earlier locals (the enclosing
    variable changes) and call its sibling `fn`s; `@pure`/`@pre`/`@post` work
    above it.
12. **PAR-001.** Each `par` branch must own the record it writes. The same record
    passed to two branches races and is rejected, as is a local that a nested
    `fn` assigns. Give each branch its own and combine after the block.
13. **SCOPE-001.** A name must be declared before use and still be in scope at
    the use site. No guessed globals, no expired loop locals, no helper-local
    names borrowed from another function.
14. **METH-001.** Methods do not capture outer locals. If a method needs a loop
    index or a caller's label, pass it as a parameter.
15. **FIELD-001.** Inside a method a local may reuse a field name. Bare `valid`
    is the local; `self.valid` is the field.
16. **FIELD-002.** Field names must exist exactly as declared. Do not invent
    fields.
17. **FIELD-003.** A field default must be a **literal** (`0`, `true`, `""`,
    `[]`) — not an expression, a call, another field, or a named `const`
    (`limit: Int = MAX_SCORE;` is rejected). Set anything else at construction:
    `new T { limit: MAX_SCORE + 1 }`.
18. **ARR-002.** A one-dimensional array indexed twice is a compile-time error.
    Aether has real nested arrays — declare the rank you actually want.
    ✗ `let dp: Int[] = []; dp[i][j] = 1;`
    ✓ `let dp: Int[][] = [];` with rows that are themselves `Int[]`
19. **FLOW-001.** Every non-`Void` function must return a value on every
    reachable top-level path. **FLOW-002.** A bare `ret;` in a non-`Void`
    function is an error — give it a value or declare `-> Void`.
20. **NAME-001.** Names ignore case. Do not redeclare a local or parameter,
    name a loop variable after a variable in scope, or name a local or
    parameter after a `const` (`limit` beside `LIMIT`).
    ✗ `loop i in 0..n { }` when `i` is already a variable
21. **TUP-001.** Tuples are narrow. `let (a, b) = pair();` works on a direct
    top-level helper call. To read one field, bind first: `let t = pair(); t.0;`
    — never `pair().0` chained onto the call. A tuple fn may `ret g(x);` when
    `g` returns the same tuple type. If the producer is a method or an
    expression, return a record and read its fields instead.
22. **IMP-001 / MOD-001 / MOD-002.** Canonical import is `use "module_name";`.
    Never invent an import. Imported names match exported names *exactly* — `use`
    does not rename, so an export called `labelScore` is called
    `labelScore`, never a guessed `label`.
23. **MS-001.** `MStream` is the memory-stream handle type. `mstream_new()`
    and `mstream_from_text(text)` return `MStream`, never `Int` or `Text`; read
    contents with `mstream_text(ms) -> Text`.
24. **FMT-001.** If the prompt specifies exact output, match it exactly —
    spacing, casing, line order, decimal precision. Print `avg0=0`, not
    `avg0 = 0`.
25. **OUT-001.** Return raw Aether source only. No Markdown fences.

Default stance: single-file programs; variadic `println("label = ", v)` rather
than `+` concatenation; explicit types on TOON values and non-trivial results;
real logic, never hard-coded expected output.

## Never Generate These

- `return` → `ret`; `class` → `type`; drop `var`, `func`, `def`, `=>`
- a field or method named after an Aether keyword (`new`, `for`, `if`, `loop`,
  `ret`) or an operator word (`div`, `mod`, `xor`, `and`, `or`, `not`). Rename
  it: `loopCount`. Other languages' words (`word`, `text`, `match`, `class`) are fine
- `fn new()` / `fn __init__` as a constructor. Aether has none — use
  `new T { field: value }`, or `new T()` then assign, or a top-level factory `fn`
- `print` / `println` / `toon_parse_file` / task helpers / `ai_chat` outside
  `fx` (FX-001)
- `use module_name;` without quotes, or `export { ... }`
- invented helpers not listed here (BUILT-001)
- `toon_parse_string(text)` — the text parser is `toon_parse(text)`
- `copy`, `pos`, `val`, `itoa`, `File` / `assign` / `readln` / `eof`,
  `paramstr`, `gettime`, `setlength`, unprefixed `mstream*`: not Aether
  (SCOPE-001 names the replacement)
- an implicit `Real` → `Int` narrowing: `let n: Int = 3.7;`, `n = random();`,
  `let n: Int = sqrt(2.0);`. It is a `[NARROW-001]` warning, so it compiles —
  write `int(...)` when you mean to truncate
- foreign object/JSON APIs: `JsonDoc`, `JsonNode`, `json.parseFile(...)`,
  `root.get(...)`, `Int.MIN`, `value.toString()`
- Markdown fences around the program

## Canonical vs accepted forms

| Topic | Canonical | Accepted | Avoid |
|---|---|---|---|
| Mutable binding | `let x: Int = 0;` | `let mut x: Int = 0;` | treating `let` as immutable |
| Loop keyword | `loop` (condition / range / foreach) | `while cond { }`, `for x in a..b { }` | inventing other loop syntax |
| Return | `ret value;` (`ret;` for Void) | — | `return value;` |
| Output | variadic `println(a, b)` in `fx` | `+` concatenation | — |
| Text equality | `a == b` | `string_eq(a, b)` | inventing `.equals(...)` |
| Text length | `string_len(t)` | `t.len` | inventing `strlen(...)` |
| Text → Int | `parse_int(t)` | — | inventing `Int(t)` / `t.toInt()` |
| Text → Real | `parse_float(t)` | — | inventing `Real(t)` |
| Text → Bool | `parse_bool(t)` | — | comparing to `"true"` by hand |
| Int → Text | `int_to_text(n)` | — | `n.toString()`, `itoa(n)` |
| Real → Text | `formatfloat(r, prec)` — 1 or 2 args | `realtostr(r)` (always 6 dp) | `formatfloat(r, 0, prec)`; `r:0:prec` as a value |
| Char → Int code | `ord(ch)` | — | `int(ch)` (`TYPE-001` for a `Text`) |
| Array / TOON length | `length(xs)` / `toon_len(node)` | `len(xs)`, `xs.len` | crossing the two |
| Method call | `counter.bump()` | — | `bump(counter)`; class syntax |
| Record init | `new Point { x: 3, y: 4 }` | `Point(x: 3, y: 4)` | `fn new()` constructor |
| TOON nested lookup | bind and validate intermediates | nested calls when shape guaranteed | a guessed flattened key |

## Conversions, math, and exact arities

These are the most-guessed names in the language. Read this table before writing
any numeric or string handling.

```aether
let parts: Text[] = split("12,7,5", ",");  // Text  -> Text[]  ["12","7","5"]
let n: Int = parse_int(parts[0]);          // Text  -> Int
let r: Real = parse_float("3.5");          // Text  -> Real
let ok: Bool = parse_bool("true");         // Text  -> Bool
let s: Text = int_to_text(99);             // Int   -> Text
let f: Text = formatfloat(3.14159, 2);     // Real  -> Text    "3.14"
let g: Text = realtostr(3.14159);          // Real  -> Text    always 6 dp
let code: Int = ord("A");                  // 1-char Text -> Int  65
let ch: Text = chr(65);                    // Int -> 1-char Text  "A"
let t: Int = int(3.7);                     // Real/Bool -> Int, truncating
let (v, valid) = try_parse_int(" 42");     // checked: (42, true); "12x" -> (0, false)
let cents: Real = round_to(2.675, 2);      // 2.68: half away from zero
```

`int(x)` casts `Real`/`Bool` only (`Int(x)`, `real(n)`, `bool(n)` are accepted
spellings). Handed a `Text` it is a `TYPE-001` error — use `parse_int` for
numeric strings and `ord` for character codes.

**The `parse_*` family cannot fail, and that is the trap.** Every input yields a
value: `parse_int("abc")` is `0`, `parse_int("12x")` is `12` (leading digits
only, the rest discarded), `parse_float("zz")` is `0.0`, and `parse_bool` is
`false` for anything but `true`/`1`/`yes`/`t`. So `parse_int(raw) == 0` is true
for `"0"`, `"abc"`, and `""` alike — never treat the result as its own validity
check.

**`try_parse_int(t)` / `try_parse_real(t)` are the checked parses.** They return
`(value, ok)`: `ok` is `true` only when the whole text (surrounding spaces
allowed) is a number, and `value` is `0` otherwise. Both are pure. Use them for
any field from a file, an argument, stdin, or a model.

`formatfloat` rounds the stored binary value (`formatfloat(2.675, 2)` is `"2.67"`);
when a spec says round half up, use `round_to(x, digits)` first.

**Arity traps.** These are caught at compile time as `BUILT-002`, but write them
right the first time:

| Helper | Arity | Note |
|---|---|---|
| `formatfloat(v)` / `formatfloat(v, prec)` | 1 or 2 | never 3 — no width slot |
| `realtostr(v)` | 1 | always 6 decimals |
| `clamp(x, lo, hi)` | 3 | |
| `min(a, b)` / `max(a, b)` | 2 | |
| `ord(ch)` / `chr(n)` / `trim(s)` | 1 | |
| `replace(s, old, new)` | 3 | every occurrence |
| `split(t, sep)` | 2 | |
| `parse_int` / `parse_float` / `parse_bool` | 1 | |

**Math surface.** `arctan` not `atan`, `ln` not `log`. Trig takes radians.
Arguments and results are `Real` unless noted.

- rounding (all → `Int`): `round`, `trunc`, `floor`, `ceil`; sign: `abs`
- powers: `sqrt`, `sqr`, `pow(base, exp)` (`power` is an alias), `exp`, `ln`,
  `log10`
- trig: `sin`, `cos`, `tan`, `arcsin`, `arccos`, `arctan`, `atan2(y, x)`, `cotan`
- hyperbolic: `sinh`, `cosh`, `tanh`
- selection: `min(a, b)`, `max(a, b)`, `clamp(x, lo, hi)` — all preserve operand
  type, so all-`Int` arguments give an `Int` back. **Reach for these instead of
  hand-writing an if-chain.**
- integer: `odd(n) -> Bool`, `factorial(n) -> Int`, `fibonacci(n) -> Int`
- random: `random()` → **`Real`** in `[0, 1)`; `random(n)` → **`Int`** in
  `[0, n)`; seed with `randomize()`. Both effectful.

**`random` is two functions.** Assigning `random()` to an `Int` truncates to a
constant `0`; for an index or a roll pass the bound, inside `fx`:
`idx = random(8);` gives `0..7`.

Each thread gets its own random stream, so `par` branches draw independent
numbers. With no `randomize()` call a run is reproducible.

**Clock.** `clock_ms() -> Int` (milliseconds since 1970, for timing) and
`now_text() -> Text` (`"YYYY-MM-DD HH:MM:SS"`, local time); both effectful.

Integer quotient: `a div b` (`7 div 2` is `3`), never `/`. Decimals: a `Real`
operand, `let pct: Real = ok * 100.0 / total;`

## Safe generation algorithm

1. Decide whether the program needs TOON.
2. Top-level order is free; locals need `let` first.
3. Give every function parameter an explicit type.
4. Prefer explicit `let name: Type = ...` unless the initializer is a literal or
   a call with a declared return type.
5. Put all output, file reads (`toon_parse_file`), task helpers, and AI calls
   inside `fx`.
6. Use `ret`, never `return`.
7. Close every parsed `ToonDoc` with `toon_close(doc)`.
8. Run the Validation Checklist before emitting.

## Smallest useful program

```aether
fn main() -> Void {
    fx {
        println("Hello from Aether");
    }
    ret;
}
```

## Core syntax

- Comments: `// line comment`; `//` is never division (write `div`). Block
  comments compile, but generate `//`.
- Text literals are double-quoted; escapes: `\"`, `\\`, `\n`, `\t`.
- Types: `Int`, `Real`, `Text`, `Bool` (`true` / `false`), `Void`, plus the
  opaque handles `ToonDoc`, `ToonNode`, `MStream`. `Float` and `String`
  are accepted spellings of `Real` and `Text`. Every other foreign spelling —
  `Double`, `Integer`, `Boolean`, `Char`, `List`, `Map`, lowercase `int` — is a
  `TYPE-002` error.
- `println(boolValue)` prints `true` or `false`.
- Operators: `+ - * / %`, `== != < <= > >=`, `!`/`not`, `&&`/`and`, `||`/`or`
  (short-circuit). Arrays compare element by element with `==` / `!=`.
  `-7 % 2` is `-1` (the dividend's sign): test odd with `n % 2 != 0`.
- Bitwise / shift, `Int` only: `&`, `|`, `^` (`xor` is the same operator as `^`),
  `<<`, `>>`. `6 & 3` is `2`; `6 << 1` is `12`. These are not logical operators —
  use `&&` / `||` for `Bool`. **They bind looser than the comparisons**, so
  `flags & mask != 0` groups as `flags & (mask != 0)` and produces an `Int`
  rather than a `Bool`; always write `(flags & mask) != 0`. That mistake is a
  `PREC-001` warning, not an error — it compiles and prints a number.
- Numeric literals accept `_` separators between digits: `1_000_000`, `0xFF_FF`.

Every `fn` declares a return type. Procedures use `-> Void` and a bare `ret;`.
`fn helper(x: Int) { ... }` is invalid.

```aether
const MAX_SCORE: Int = 100;

fn add(a: Int, b: Int) -> Int {
    ret a + b;
}
```

**Conditionals.** Parentheses are optional. The canonical multi-branch shape is
sequential `if` + `ret`:

```aether
fn classify(score: Int) -> Text {
    if score >= 90 {
        ret "ready";
    }
    if score >= 70 {
        ret "review";
    }
    ret "blocked";
}
```

Statement-level `else` and chained `else if` are supported. Inline
`if ... else ...` also works anywhere a value is expected — declarations,
assignments, `ret`, and call arguments including `println`:

```aether
let label: Text = if ready { "ready" } else { "blocked" };
let grade: Text = if score > 90 { "A" } else if score > 80 { "B" } else { "C" };
```

**Loops.** `break` exits, `continue` skips, and a range is half-open: 1
through `n` is `1..n + 1`, and -1, 0, 1 is `-1..2`. Prefer the range and
foreach forms — both **declare their own loop variable**, so they need no
`let`:

```aether
loop i in 0..count { total = total + i; }   // range: 0 up to count - 1
loop i in 0..count step 2 { total = total + i; }  // stepped; `step -1` counts down
loop item in items { total = total + item; } // foreach over an array, Text or ToonNode array
loop index < total { index = index + 1; }   // condition: declare `index` first
loop { break; }                             // infinite + break
```

That variable is scoped to the loop; `item` takes the element type (`Int` from
`Int[]`, one-character `Text` from a `Text`, `ToonNode` from a TOON array).

**Quantifiers.** `all x in xs: x > 0` and `any i in 0..n: xs[i] == t` are `Bool`
expressions over the same headers. They stop at the first deciding element
(empty: `all` true, `any` false) and work in contracts. Parenthesize one inside
`&&`: `(all x in xs: x > 0) && ok`.

**Operator precedence** follows C: `* / %` above `+ -`, then `<< >>`, then the
comparisons, then `== !=`, then `& ^ |`, then `&&`, then `||`; the word forms
sit with their symbols. When in doubt, parenthesize — it is always accepted.

## Functions and tuple returns

Every parameter is typed and every function declares a return type.

```aether
@pure
fn area(w: Int, h: Int) -> Int {
    ret w * h;
}

fn main() -> Void {
    fx { println(area(3, 4)); }
    ret;
}
```

A function may return a **tuple**, declared as `-> (T, U)`.

```aether
fn pairSquares(n: Int) -> (Int, Int) {
    ret (n, n * n);
}

fn main() -> Void {
    let (base, square) = pairSquares(6);     // destructure a DIRECT call
    let t = pairSquares(7);                  // or bind, then read positionally
    fx {
        println(base, " ", square);
        println(t.0, " ", t.1);              // zero-based
    }
    ret;
}
```

The limits, all reported as `TUP-001`:

- Destructuring works only on a **direct call to a top-level tuple-returning
  function**, not on a method, an undefined helper, or a nested expression.
- An index at or past the arity (`t.2` on a 2-tuple) is a **compile-time** error,
  not a crash.
- Methods cannot return tuples.

When any of those bite, return a record instead and read its fields — records
have none of these restrictions:

```aether
type Pair {
    first: Int = 0;
    second: Int = 0;
}

fn splitPair(s: Text) -> Pair {
    ret new Pair { first: string_len(s), second: 7 };
}
```

A `@post` on a tuple return uses positional slots:

```aether
@post result.0 <= result.1
fn ordered(a: Int, b: Int) -> (Int, Int) {
    if a <= b {
        ret (a, b);
    }
    ret (b, a);
}
```

Recursion through a tuple-returning function and calls to it from concurrent
`par` branches are both fine.

## Records: `type`

```aether
type Counter {
    value: Int = 0;                    // fields end with ';'

    @pre self.value >= 0
    fn bump() -> Int {
        self.value = self.value + 1;   // lowercase self, never Self
        ret self.value;
    }
}
```

- Methods live **inside** the `type` with an **implicit `self`**. Never give a
  method a `self` parameter. Call a sibling method as `self.other()`.
- A method's `@pre` / `@post` may reference `self.field`.
- Fields may declare **literal** defaults (FIELD-003). `new Counter()` gives
  each field its declared default, otherwise the type zero: `Int` `0`, `Real`
  `0.0`, `Bool` `false`, `Text` empty.
- Records are **pointer-backed**: a mutation made through a callee is visible to
  the caller. (Arrays are not — see **Dynamic arrays**.)
- A top-level `fn bump(self: Counter) -> Int` is callable as `counter.bump()`,
  but a contract on it cannot name `self`. A field may hold an array of records
  (`items: Item[] = [];`).

### Constructing records and typing bindings

`new T()` is the only constructor. There is no `fn new()`, `fn __init__`, or
`T.new()`.

```aether
type Point {
    x: Int = 0;
    y: Int = 0;
}

fn main() -> Void {
    let p: Point = new Point { x: 3, y: 4 };   // set fields at construction
    let q: Point = new Point();                // defaults / type zeroes
    q.x = 3;                                   // or assign after allocation
    let ps: Point[] = [new Point { x: 1, y: 2 }, new Point { x: 3, y: 4 }];
    fx { println(p.x, " ", q.x, " ", length(ps)); }
    ret;
}
```

A partial `new T { ... }` keeps every unset field's default. Bare
`Point { x: 3, y: 4 }` and `Point(x: 3, y: 4)` are also accepted.

**Always annotate an array literal:** `let xs = [1, 2, 3];` fails with `cannot
infer the type of 'xs'`. `let c = new C();` infers `C`. Omit a type only for a
literal or a call with a declared return type.

## Effects: `fx`

Every effectful builtin call sits inside `fx { ... }` (the smallest program
above is the shape). Only the *builtin calls* are gated, not the surrounding structure — `if`, `loop`,
and blocks nest inside `fx` freely, so an entire loop can live in one block:

```aether
fx {
    loop i in 0..5 {
        println(i);
    }
}
```

A `@pure` function may call only pure builtins and other `@pure` functions —
mark every helper it calls `@pure` too.

## Printing and formatting

```aether
fx {
    println("Drop ", j, " -> ID: ", id);   // variadic; never '+' guessing
    println(pct:0:2);                      // width:precision => 95.50
    print("no newline");
}
```

`println(realValue)` defaults to six decimals. Use `value:width:precision` when
exact output matters; width `0` means "precision only". That spec works **only
inside `print`/`println`** — to build a `Text`, use `formatfloat`.

`println(a, b)` puts **nothing** between its arguments — write
`println(a, " ", b)` — and each requested output line is its own `println`.

## Text

```aether
fn main() -> Void {
    let name: Text = "Aether";
    if name == "Aether" {                   // canonical; string_eq(a, b) accepted
        let n: Int = string_len(name);      // canonical; name.len accepted
        fx { println(n); }
    }
    ret;
}
```

The Text helpers, all pure (positions are 0-based, `-1` when absent):

- search: `contains(s, x)`, `starts_with(s, p)`, `ends_with(s, p)`,
  `index_of(s, x)`, `count(s, x)`
- build: `upper(s)`, `lower(s)`, `trim(s)`, `replace(s, old, new)` (every
  occurrence), `repeat(s, n)`, `reversed(s)`, `join(parts, sep)`,
  `split(t, sep)`

**`Text` indexes exactly like an array: 0-based, half-open slices.** `s[0]` is
the first character, `s[string_len(s) - 1]` the last, `s[a..b]` slices like an
array, and `s[i] = "x"` replaces one character. Visit each character with
`loop ch in s { ... }`, or by index with `loop i in 0..string_len(s) { ... s[i] ... }`.

## Dynamic arrays

```aether
fn main() -> Void {
    let xs: Int[] = [];
    xs = xs + [7];                 // append (literal of any length)
    let ys: Int[] = [1, 2, 3];
    xs = xs + ys;                  // concatenate two array-valued expressions
    let n: Int = length(xs);       // len(xs) and xs.len also accepted
    let first: Int = xs[0];        // indexed read
    xs[0] = 9;                     // indexed write
    let mid: Int[] = ys[0..2];     // slice: elements 0 and 1, a copy not a view
    fx { println(n, " ", first, " ", length(mid)); }
    ret;
}
```

`xs[a..b]` is half-open, matching `loop i in a..b`; `xs[a..]` runs to the end
(`queue = queue[1..];`) and `xs[..b]` starts at 0, for `Text` too. There is no
first-class Range value — `a..b` is meaningful only inside `[...]` or a `loop`
header.
`[v; n]` is n copies of v (`[0; 26]`, a table `[[0; cols]; rows]` with
independent rows); shrink with a slice (`xs = xs[..n];`). `xs == ys` compares
element by element, nested arrays included.

**Nested arrays are real.** A row is just an `Int[]`, so a table is `Int[][]`:

```aether
fn main() -> Void {
    let table: Int[][] = [];
    loop r in 0..3 {
        let row: Int[] = [];
        loop c in 0..4 {
            row = row + [r * 4 + c];
        }
        table = table + [row];
    }
    table[1][2] = 99;
    fx {
        println(table[1][2], " rows=", length(table), " cols=", length(table[0]));
    }
    ret;
}
```

Bound the inner loop with `length(table[r])`, not the first row's width — rows
are independent arrays, so a table may be jagged.

**Arrays are value-copied at the call boundary; records are not.** Passing `xs`
into a function gives that function its own copy, so mutating it there never
reaches the caller. Return the new array and reassign:

```aether
fn doubleAll(arr: Int[]) -> Int[] {
    loop i in 0..length(arr) {
        arr[i] = arr[i] * 2;
    }
    ret arr;                       // required -- in-place writes are invisible otherwise
}
```

A `Void` function that writes into an array parameter and never returns it draws
an `[ARR-001]` warning; the same pattern on a *record* parameter works.

`println` cannot print a whole array (`TYPE-001`). Print the elements:
`fx { loop x in xs { print(x, " "); } println(""); }`.

## Array helpers

For `Int[]`, `Real[]` and `Text[]`, all pure; each returns a new value and
leaves `xs` unchanged:

| Helper | Result |
|---|---|
| `sum(xs)` | `Int` or `Real`; `0` when empty |
| `min_of(xs)` / `max_of(xs)` | an element; a run-time error when empty |
| `sorted(xs)` / `sorted_desc(xs)` | a new array, stable |
| `reversed(xs)` | a new array |
| `contains(xs, v)` / `index_of(xs, v)` / `count(xs, v)` | `Bool` / first index or `-1` / `Int` |
| `sorted_by(rs, "field")` / `sorted_by_desc(rs, "field")` | records by an `Int`, `Real` or `Text` field, named by a string literal (FIELD-002 otherwise) |

```aether
type Player { name: Text; score: Int; }

fn main() -> Void {
    let xs: Int[] = [3, 1, 2];
    let ys: Int[] = sorted(xs);                      // [1, 2, 3]; xs unchanged
    let mean: Real = sum(xs) * 1.0 / length(xs);
    let ps: Player[] = [new Player { name: "a", score: 5 }, new Player { name: "b", score: 9 }];
    let ranked: Player[] = sorted_by_desc(ps, "score");
    fx { println(ys[0], " ", xs[0], " ", mean, " ", ranked[0].name); }
    ret;
}
```

**Filter and map.** No `filter`, no `map` — build a new array in a loop:

```aether
@pure
fn evensDoubled(xs: Int[]) -> Int[] {
    let out: Int[] = [];
    loop i in 0..length(xs) {
        if xs[i] % 2 == 0 {
            out = out + [xs[i] * 2];
        }
    }
    ret out;
}
```

**Lookup tables.** There is no map or dictionary type. Use two parallel arrays
and a linear scan, or a `type` with an array field:

```aether
@pure
fn lookup(keys: Text[], values: Int[], key: Text) -> Int {
    loop i in 0..length(keys) {
        if keys[i] == key {
            ret values[i];
        }
    }
    ret -1;
}
```

## Structured data: TOON

Complete helper surface:

- lifecycle: `has_toon()`, `toon_parse(text)`, `toon_parse_file(path)`,
  `toon_root(doc)`, `toon_close(doc)`, `toon_free(node)`
- navigation: `toon_key(node, key)`, `toon_key_or(node, key, fallback)`,
  `toon_at(node, i)`, `toon_len(node)`, `toon_null()` (a null node, the usual
  `toon_key_or` fallback)
- field getters: `toon_get_text(node, key)`, `toon_get_int(node, key)`,
  `toon_get_real(node, key)`, `toon_get_bool(node, key)`
- defaulted getters: `toon_get_text_or(node, key, fallback)`, and the matching
  `toon_get_int_or`, `toon_get_real_or`, `toon_get_bool_or`: the fallback when
  the key is missing, null or of another type
- node values: `toon_text_value(node)`, `toon_int_value(node)`,
  `toon_real_value(node)`, `toon_bool_value(node)`, `toon_null_value(node)` (Bool)
- presence and shape: `toon_type(node)`, `toon_has_key(node, key)`,
  `toon_has_at(node, i)`, `toon_is_text`, `toon_is_int`, `toon_is_real`,
  `toon_is_bool`, `toon_is_null`, `toon_is_arr`, `toon_is_obj`

Node-value accessors keep the `_value` suffix (`toon_int_value`); field getters
do not (`toon_get_int`). There is no bare `toon_int` or `toon_text`. Keys are
`Text`, indexes are `Int`.

**Ownership.** `toon_parse_file(path)` is effectful (file I/O — call it inside
`fx`); `toon_parse(text)` and every node operation are pure. The `ToonDoc` owns
every `ToonNode` derived from it, and `toon_close(doc)` releases the document and
all remaining handles. For short programs, one `toon_close` at the end is enough;
inside a large loop, `toon_free(node)` on temporaries avoids handle buildup.

Getters take a `ToonNode` (TOON-001):

```aether
let doc: ToonDoc = toon_parse("{\"name\":\"Aether\"}");
let root: ToonNode = toon_root(doc);
let name: Text = toon_get_text(root, "name");
```

**Root shape (ROOT-001).**

```aether
// JSON starts with '[': root IS the array
loop i in 0..toon_len(root) {
    let row: ToonNode = toon_at(root, i);
}

// JSON starts with '{': extract the named array first
let jobs: ToonNode = toon_key(root, "jobs");
loop i in 0..toon_len(jobs) {
    let job: ToonNode = toon_at(jobs, i);
}
```

**Key fidelity (KEY-001).** Copy JSON keys exactly; never flatten a nested object
into a guessed key (`"appName"`). A dotted key walks nested *objects* (never an
array index) in `toon_key`, `toon_has_key` and every getter:

```aether
let port: Int = toon_get_int_or(root, "server.port", 0);
```

**Nested lookup safety (NEST-001).** A missing intermediate never crashes: an
absent node degrades in every accessor (`_or` fallback, `false`, zero value,
`toon_len` 0), so a dotted key or `toon_key_or(node, key, toon_null())` makes a
path of any depth total with no `if`:

```aether
let gb: Int = toon_get_int_or(row, "spec.mem.gb", 0);
let spec: ToonNode = toon_key_or(row, "spec", toon_null());
```

Keep an `if toon_has_key(...)` guard only when the output must say *which*
level was missing. (`toon_has_at` raises on a null node; test `i < toon_len(n)`.)

## Concurrency: `par`

`par { ... }` runs your own functions concurrently and joins before continuing.
The body holds **direct call statements only** — no assignments, no loops, no
inline `fx`. Wrap any work in a `fn` and call that.

Results come back through records passed as arguments, one per branch (PAR-001).

```aether
type Tally {
    count: Int = 0;
}

fn bump(t: Tally, upTo: Int) -> Void {
    loop i in 0..upTo {
        t.count = t.count + 1;
    }
    ret;
}

fn main() -> Void {
    let a: Tally = new Tally();
    let b: Tally = new Tally();
    par {
        bump(a, 100);
        bump(b, 200);
    }
    fx { println("a=", a.count, " b=", b.count); }
    ret;
}
```

Scalars and handles (an `Int`, a `ToonNode`) may be shared across branches;
never the same record or array.

## Contracts

```aether
@pure
@pre score >= 0
@post result >= 0
fn normalize(score: Int) -> Int {
    ret clamp(score, 0, 100);
}
```

- `@post` may reference `result`.
- `@cost 5ms` accepts units `ns us ms s op ops step steps`. It is syntax-checked
  but non-binding.
- On a collection return, a contract must compare a **property**, not the
  collection: `@post length(result) > 0`, never `@post result > 0`. Same for a
  `@pre` over an array parameter: `@pre length(xs) > 0`; every element:
  `@pre all x in xs: x >= 0`.

## Modules

Most generated Aether should be a single file. When a task supplies modules:

- import with `use "module_name";` — the string resolves to a sibling file of
  that exact name
- imported names match exported names exactly (MOD-001)
- `@pure` goes above `export fn`
- assume modules export `const` and `fn`

```aether
use "score_utils";

fn main() -> Void {
    let score: Int = clampScore(141);
    fx { println(score, " ", labelScore(score)); }
    ret;
}
```

## Copyable templates

**Pure helper plus effectful main.**

```aether
@pure
fn transform(value: Int) -> Int {
    ret value + 1;
}

fn main() -> Void {
    let answer: Int = transform(41);
    fx {
        println("answer = ", answer);
    }
    ret;
}
```

**TOON array classification, from a file.** `scores.json` holds
`[{"name":"A","score":95},{"name":"B","score":72}]`.

```aether
@pure
fn classify(score: Int) -> Text {
    if score >= 90 {
        ret "ready";
    }
    if score >= 70 {
        ret "review";
    }
    ret "blocked";
}

fn main() -> Void {
    if !has_toon() {
        fx { println("structured data support unavailable"); }
        ret;
    }

    let doc: ToonDoc;
    fx { doc = toon_parse_file("scores.json"); }   // reading a file is an effect
    let root: ToonNode = toon_root(doc);

    loop i in 0..toon_len(root) {
        let row: ToonNode = toon_at(root, i);
        let name: Text = toon_get_text_or(row, "name", "?");
        let score: Int = toon_get_int_or(row, "score", 0);
        fx {
            println(name, " -> ", classify(score));
        }
    }

    toon_close(doc);
    ret;
}
```

**Safe nested extraction with a running total.**

```aether
fn main() -> Void {
    if !has_toon() {
        fx { println("structured data support unavailable"); }
        ret;
    }

    let doc: ToonDoc = toon_parse("{\"rows\":[{\"meta\":{}},{\"meta\":{\"code\":\"K7\"}},{\"note\":\"x\"}]}");
    let root: ToonNode = toon_root(doc);
    let rows: ToonNode = toon_key(root, "rows");
    let missing: Int = 0;

    loop i in 0..toon_len(rows) {
        let row: ToonNode = toon_at(rows, i);
        let code: Text = "EMPTY";
        if toon_has_key(row, "meta") {
            let meta: ToonNode = toon_key(row, "meta");
            code = toon_get_text_or(meta, "code", "EMPTY");
        }
        if code == "EMPTY" {
            missing = missing + 1;
        }
        fx {
            println("row ", i, " = ", code);
        }
    }

    fx {
        println("missing = ", missing);
    }

    toon_close(doc);
    ret;
}
```

**Compact object with a method.**

```aether
type Basket {
    items: Int = 0;
    total: Real = 0.0;

    fn add(price: Real) -> Void {
        self.items = self.items + 1;
        self.total = self.total + price;
        ret;
    }

    fn average() -> Real {
        if self.items == 0 {
            ret 0.0;
        }
        ret self.total / self.items;
    }
}

fn main() -> Void {
    let b: Basket = new Basket();
    b.add(4.5);
    b.add(9.25);
    fx {
        println("items=", b.items, " avg=", formatfloat(b.average(), 2));
    }
    ret;
}
```

**Method that needs caller context.** A method cannot see the caller's loop
index (METH-001) — pass it in:

```aether
type Row {
    label: Text = "";

    fn render(index: Int, total: Int) -> Text {
        ret int_to_text(index + 1) + "/" + int_to_text(total) + " " + self.label;
    }
}

fn main() -> Void {
    let rows: Row[] = [new Row { label: "alpha" }, new Row { label: "beta" }];
    loop i in 0..length(rows) {
        let line: Text = rows[i].render(i, length(rows));
        fx { println(line); }
    }
    ret;
}
```

**Long report programs.** Shape them as: parse (a file: inside `fx`) →
`toon_root` → `toon_key` for the
item array → one pure normalize helper → one pure classify helper → one mutable
totals `type` → one loop that extracts, classifies, updates totals, and prints →
one final totals block → `toon_close(doc)`.

## Repair rules

The compiler prints a stable code in brackets. Read the code, then apply the fix.

- **[FX-001]** an output, file-read (`toon_parse_file`), task, or `ai_chat` call
  outside an effect block → wrap it in `fx { ... }` (rule 1 shows the shapes).
- **[SYN-001]** non-Aether syntax → `ret` not `return`, `type` not `class`; drop
  `var`, `def`, `=>` (the message names the Aether form). Also a field or method
  named after an Aether keyword or operator word → rename the member.
- **[SCOPE-001]** the catch-all. It is one of:
  - a helper not listed in this document → it does not exist; the hint names
    the one to use (`pos` → `index_of`, `val` → `try_parse_int`)
  - an export called by a guessed name → use the exact exported name
  - a method reaching an outer local → pass it in as a parameter
  - `method '<m>' is not defined on type '<T>'` → define it inside the `type`
  - a genuinely undeclared name → declare it earlier or pass it in
- **[BUILT-002]** right builtin, wrong argument count → check the arity table.
- **[NAME-001]** a name declared twice (names ignore case; a loop variable,
  local or parameter may not reuse a visible variable or `const`) → pick a
  fresh name, or assign without `let`.
- **[IMP-001]** a malformed `use` → write `use "module_name";` and call the
  exports directly.
- **[TYPE-001]** a type cannot be inferred → annotate it, including untyped array
  literals. Also `println` of a whole array, `int()` of a `Text`, and a `Real`
  where an `Int` is needed (`%`, `div`).
- **[TYPE-002]** a type name that does not exist, in any position (declaration
  order does not matter) → use `Int`, `Real`, `Text`, `Bool`, `Void`, `T[]`, or
  a `type` you declared. The message names the Aether
  spelling when there is one (`Double` → `Real`, `Integer` → `Int`, `Char` →
  `Text`, `int` → `Int`).
- **[TOON-001]** handle misuse → go through `toon_root(doc)` (rule 6).
- **[MS-001]** stream misuse → declare stream bindings `MStream` (rule 23).
- **[FIELD-002]** `Unknown field` → use the exact declared name.
- **[FIELD-003]** a non-literal field default → use a literal, or set the value
  at construction with `new T { field: value }`. A *type*-mismatched default is
  `[TYPE-001]` instead.
- **[FLOW-001]** a fallthrough path with no return value → add a final `ret`.
- **[FLOW-002]** a bare `ret;` in a non-`Void` function → return a value, or
  declare `-> Void`.
- **[ANN-001]** a misplaced annotation, or a `@pure` function calling an effect →
  move the annotation above the function; keep effects out of pure code and
  mark its helpers `@pure` too.
- **[TUP-001]** tuple misuse → destructure a direct call, or bind and read one
  field; never chain onto the call.
- **[ARR-001]** (warning) a `Void` function mutating an array parameter → return
  the array and reassign at the call site.
- **[NARROW-001]** (warning) a Real value stored in an `Int` target → the
  fraction is discarded. Use a `Real` target, or `int(...)` to say you meant it.
- **[ARR-002]** a one-dimensional array indexed twice → declare it `T[][]` and
  build rows as arrays, or index once with a computed offset
  (`grid[r * width + c]`).
- **`Length expects a string or array argument`** / **`... expects a single
  value handle`** (runtime, no code) → `toon_len` and `length` are crossed
  (LEN-001).
- **[ARR-003]** (runtime) index outside the array's range; the message gives the
  index, the dimension and the valid range, and the `[Error Location]` line gives
  the source line. `valid indices are 0..0` means one element exists, so the row
  is usually **not appended yet**: while building a table row by row, the current
  row is the local `row`, not `table[i]` — `table[i]` only exists after
  `table = table + [row];`. Earlier rows read fine as `table[i - 1][j]`.
- **[CON-001]** (runtime) `Aether @pre failed in f` (or `@post`), exit 1 → a
  call broke `f`'s contract: `@pre`, a bad argument; `@post`, a wrong `result`.
- **[PREC-001]** (warning) `flags & mask != 0` → parenthesize:
  `(flags & mask) != 0` (Core syntax). As an error:
  `0 <= i < n` → `0 <= i && i < n`.
- **[FMT-001]** a placeholder (`"{}"`, `%d`, `f"..."`) in `println` → pass
  values as arguments: `println("n = ", n)`.
- **[DIV-002]** `x // 2` → `//` starts a comment; integer division is `x div 2`.
- **[ENTRY-001]** top-level statements beside `fn main`, or a `main` with
  parameters → put the statements in `fn main() -> Void`.
- **[PAR-001]** the same record in more than one `par` branch → give each branch
  its own and combine afterwards.
- **[PAR-002]** a non-call statement inside `par` → wrap the work in a `fn`.

If the program *compiles* but the output is wrong, no code is printed. Those are
authoring rules nothing can check: extra headings, wrong spacing or precision (an
integer where decimals were wanted → a `Real` target and a `Real` operand),
guessed JSON keys, a fallback that hid a missing level, or iterating an object root.
Re-read the prompt and match it exactly.

## Validation checklist

- all output, `toon_parse_file`, task, and `ai_chat` calls inside `fx { ... }`
  (FX-001)
- every called helper is listed in this document, not guessed (BUILT-001), with
  the right argument count (BUILT-002)
- imports verified; exported names used exactly (IMP-001, MOD-001)
- all parameters typed; array literals annotated (TYPE-001)
- `ret` not `return`; `type` not `class` (SYN-001)
- no field or method named after an Aether keyword; no constructor method
- no arithmetic on or cross-assignment of TOON handles; every doc closed
- stream bindings declared `MStream`; bodies read via `mstream_text` (MS-001)
- object roots: named array extracted (ROOT-001); keys copied exactly (KEY-001);
  a missing intermediate handled deliberately (NEST-001)
- `toon_len` vs `length` used correctly (LEN-001)
- `div` for whole-number quotients; a `Real` operand wherever decimals matter;
  no accidental `Real` → `Int` truncation
- each `par` branch writes its own record (PAR-001)
- functions return on every reachable path (FLOW-001)

---

## Appendix: host capabilities

Reach for these only when the task requires them.

### Files and environment

All effectful — inside `fx`, and banned in `@pure`.

- `read_file(path) -> Text`, `read_lines(path) -> Text[]` (no line breaks)
- `write_file(path, text)` (create or truncate), `append_file(path, text)`;
  end each line with `"\n"` yourself
- `file_exists(path) -> Bool`, `delete_file(path) -> Bool`,
  `rename_file(from, to) -> Bool`, `getcurrentdir() -> Text`, `mkdir(path)`
- `read_line() -> Text` (next stdin line, `""` at the end),
  `read_all_stdin() -> Text`
- `args() -> Text[]` (the arguments after the program), `getenv(name) -> Text`

```aether
fn main() -> Void {
    fx {
        write_file("notes.txt", "first line\n");
        append_file("notes.txt", "second line\n");
        loop line in read_lines("notes.txt") { println(line); }
    }
    ret;
}
```

### HTTP requests

Not present in every build. If the task must degrade cleanly, guard with
`has_builtin("network", "HttpRequest")`. All effectful. The response body lands
in an `MStream` out-buffer.

- `httpsession() -> Int`, `httpclose(session)`,
  `httpsetheader(session, name, value)`
- `httprequest(session, method, url, body, out) -> Int` — returns the HTTP
  status; `body` is the request payload (`""` for GET); `out` is an initialized
  `MStream`
- `mstream_new() -> MStream`, `mstream_from_text(text) -> MStream`,
  `mstream_text(ms) -> Text`, `mstream_append_text(ms, t)`,
  `mstream_length(ms) -> Int`, `mstream_free(ms)` — all pure;
  `mstream_load(path) -> MStream` and `mstream_save(ms, path)` touch files.

```aether
fn fetch(url: Text) -> Void {
    fx {
        let session: Int = httpsession();
        let out: MStream = mstream_new();
        let status: Int = httprequest(session, "GET", url, "", out);
        if status == 200 {
            println(mstream_text(out));
        }
        mstream_free(out);
        httpclose(session);
    }
    ret;
}
```

`mstream_text(out)` reads the body. A JSON response pairs directly with TOON:
`toon_parse(mstream_text(out))`.

### Sockets

A BSD-style API. The names are `socket*` and `dnslookup` — there is no
`tcpsocket*`, `udpsocket*`, or `resolve`. All effectful. `socketreceive` returns
an `MStream`, never `Text`.

- `socketcreate(type[, family]) -> Int` — `type` `0`=TCP, `1`=UDP; `family`
  `4`=IPv4 (default), `6`=IPv6
- `socketconnect(socket, host, port)`, `socketbind(socket, port)`,
  `socketbindaddr(socket, host, port)`, `socketlisten(socket, backlog)` — all
  return `0` or `-1`; `socketclose(socket)` returns nothing
- `socketaccept(socket) -> Int` and `socketreceive(socket, maxlen) -> MStream`
  both **block**. A zero-length receive means the peer closed, not an error.
- `socketsend(socket, data) -> Int` — `data` is `Text` or `MStream`
- `socketpeeraddr(socket) -> Text`, `socketlasterror() -> Int`
- `socketsetblocking(socket, blocking: Bool)`,
  `socketpoll(socket, timeoutMs, flags) -> Int` — `flags` `1`=readable,
  `2`=writable, bitwise-or; `0` on timeout

`socketaccept` and `socketreceive` hang forever with no peer. Either run a
client and server in one process via `par` — creating, binding, and listening
*before* the `par` block so the client cannot race a server that is not yet
listening — or set the socket non-blocking and poll.

### Tasks and AI

All effectful except the `has_*` probes.

- `sleep(ms: Int) -> Void` — a blocking millisecond pause. Prefer this over a
  busy-wait loop.
- `ai_chat(model, messages, system = "", apiKey = "", endpoint = "") -> Text`
- probes: `has_ai() -> Bool`, `has_toon() -> Bool`,
  `has_builtin(category: Text, function: Text) -> Bool` — true for extended
  *and* core builtins; a core builtin matches on the function name whatever
  category you pass

To run your own code concurrently, use `par`.
