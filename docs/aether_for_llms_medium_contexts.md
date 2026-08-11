# Aether for LLMs — Working Guide (for medium contexts)

*Guide version: 2026-08-11-1*

Everything needed to write correct Aether in one shot. Sized for a ~32K context:
it should occupy about a third of your window, leaving room to reason, emit the
program, and repair it once.

Sections are ordered by how often you need them. If this document reaches you
truncated, what you lose first is the capability appendix (files, HTTP, sockets,
tasks) — not the conversion and math tables, which sit near the top because
guessing a conversion's name or arity is the single most common way generated
Aether fails.

## Highest-Value Rules

Each rule is followed by the shape that violates it, because recognizing the
wrong shape is what stops you writing it.

1. **FX-001.** Every effectful builtin must sit inside `fx { ... }`: output
   (`print`, `println`), task helpers, `ai_chat`, and all host interaction —
   filesystem (`mkdir`, `fileexists`), env (`getenv`), CLI (`paramstr`),
   `random`/`randomize`, clock (`gettime`), console input, `http*`, `socket*`,
   `sqlite*`. Pure math, string, and conversion helpers do not need `fx`.
   ✗ `println("hi");` at function scope   ✓ `fx { println("hi"); }`
2. **SYN-001.** Aether keywords only: `fn`, `let`, `const`, `ret`, `if`, `loop`,
   `type`, `mod`, `use`, `new`, `par`, `fx`, `break`, `continue`.
   ✗ `return`, `class`, `var`, `def`, `func`, `=>`, Python-style colons.
   `while cond { }` and `for x in a..b { }` do compile, but `loop` is canonical
   and covers every form they do.
3. **BUILT-001.** The builtins listed in this document are the surface. Do not
   invent helpers and do not guess names. More builtins exist, but the discovery
   functions (`builtin_info`, `builtins_json`) are *runtime* calls — if you are
   emitting a source file in one shot you cannot run them, so an unlisted name is
   not available to you. Restructure onto a listed helper instead.
   ✗ `substring(s, 0, 3)`, `to_upper(s)`, `replace(s, a, b)`
4. **BUILT-002.** Right name, wrong argument count is its own compile-time error
   for the fixed-arity conversion and math helpers.
   ✗ `formatfloat(r, 0, 2)` — it takes `(value, precision)`; the width slot
   belongs only to the `println`-only `r:0:prec` form.
5. **TYPE-001.** If inference is not obviously safe, write the type.
   ✗ `let c = new C();` (a later `c.inc();` fails `POINTER but got VOID`)
   ✗ `let xs = [1, 2, 3];` (cannot infer)
   ✓ `let c: C = new C();`  ✓ `let xs: Int[] = [1, 2, 3];`
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
10. **ORDER-001.** Define types, helpers, and modules before `main` uses them.
11. **LEN-001.** `toon_len(node)` for TOON arrays; `length(xs)` for dynamic
    arrays. Crossing them is a `TYPE-001`.
12. **FUNC-001.** Functions are not values. No anonymous `fn`, no lambdas, no
    closures, never pass a function as an argument, and there is no
    `map`/`filter`/`reduce` — write a `loop`. To run your own code concurrently
    use `par { f(); g(); }`.
13. **PAR-001.** Each `par` branch must own the record it writes. The same record
    passed to two branches races and is rejected. Give each branch its own and
    combine after the block.
14. **SCOPE-001.** A name must be declared before use and still be in scope at
    the use site. No guessed globals, no expired loop locals, no helper-local
    names borrowed from another function.
15. **METH-001.** Methods do not capture outer locals. If a method needs a loop
    index or a caller's label, pass it as a parameter.
16. **FIELD-001.** Inside a method a local may reuse a field name. Bare `valid`
    is the local; `self.valid` is the field.
17. **FIELD-002.** Field names must exist exactly as declared. Do not invent
    fields.
18. **FIELD-003.** A field default must be a **literal** — `count: Int = 0`,
    `on: Bool = true`, `name: Text = ""`, `xs: Int[] = []`. Not an expression,
    not another field, not `self`, not a call — and **not a named `const`
    either**: `limit: Int = MAX_SCORE;` is rejected just like
    `limit: Int = MAX_SCORE + 1;`. For anything else, set it at construction:
    `new T { limit: MAX_SCORE + 1 }`.
19. **ARR-002.** A one-dimensional array indexed twice is a compile-time error.
    Aether has real nested arrays — declare the rank you actually want.
    ✗ `let dp: Int[] = []; dp[i][j] = 1;`
    ✓ `let dp: Int[][] = [];` with rows that are themselves `Int[]`
20. **FLOW-001.** Every non-`Void` function must return a value on every
    reachable top-level path. **FLOW-002.** A bare `ret;` in a non-`Void`
    function is an error — give it a value or declare `-> Void`.
21. **NAME-001.** Do not redeclare a local in the same scope.
22. **TUP-001.** Tuples are narrow. `let (a, b) = pair();` works on a direct
    top-level helper call. To read one field, bind first: `let t = pair(); t.0;`
    — never `pair().0` chained onto the call. If the producer is a method or an
    expression, return a record and read its fields instead.
23. **IMP-001 / MOD-001 / MOD-002.** Canonical import is `use "module_name";`.
    Never invent an import. Imported names match exported names *exactly* — `use`
    does not rename, so an export called `classifySupport` is called
    `classifySupport`, never a guessed `classify`.
24. **MS-001.** `MStream` is the memory-stream handle type. `mstreamcreate()`
    and `mstreamfromstring(text)` return `MStream`, never `Int` or `Text`; read
    contents with `mstreambuffer(ms) -> Text`. To build a stream that already
    holds text, call `mstreamfromstring(text)` — there is no stream-write
    builtin, so `mstreamcreate()` followed by a guessed `mstreamwrite(...)` is
    wrong.
25. **FMT-001.** If the prompt specifies exact output, match it exactly —
    spacing, casing, line order, decimal precision. Print `avg0=0`, not
    `avg0 = 0`.
26. **OUT-001.** Return raw Aether source only. No Markdown fences.

Default stance: single-file programs; variadic `println("label = ", v)` rather
than `+` concatenation; explicit types on TOON values and non-trivial results;
real logic, never hard-coded expected output.

Not every rule above is a compiler check. Some are emitted with a stable
`[CODE]`, some are folded into a broader code (usually `SCOPE-001`), and the rest
are authoring rules nothing can catch. **Repair rules** below marks which is
which.

## Never Generate These

- `return` → `ret`; `class` → `type`; drop `var`, `func`, `def`, `=>`
- a field or method named after a reserved word — a type name (`word`, `text`,
  `int`, `byte`, `bool`), a keyword (`new`, `for`, `if`, `match`), or an operator
  word (`mul`, `div`, `mod`, `xor`). Rename it: `wordCount`, `multiply`
- `fn new()` / `fn __init__` as a constructor. Aether has none — use
  `new T { field: value }`, or `new T()` then assign, or a top-level factory `fn`
- an untyped `new` binding or array literal (TYPE-001)
- `print` / `println` / task helpers / `ai_chat` outside `fx` (FX-001)
- invented imports such as `use "helpers";` (IMP-001)
- `use module_name;` without quotes, or `export { ... }`
- invented helpers not listed here (BUILT-001)
- `toon_parse_string(text)` — the parser is `toon_parse(text)`. The `_file`
  suffix on `toon_parse_file(path)` does **not** imply a `_string` sibling;
  `toon_parse` already takes `Text`
- `formatfloat(r, 0, prec)` — one or two arguments only (BUILT-002)
- anonymous functions, lambdas, or passing a function as a value (FUNC-001)
- arithmetic on, or cross-assignment of, `ToonDoc` / `ToonNode` (TOON-001)
- `let stream: Int = mstreamcreate();` — streams are `MStream` (MS-001)
- annotations inside a function body (ANN-001)
- `pair().0` — bind the tuple first (TUP-001)
- an implicit `Real` → `Int` narrowing: `let n: Int = 3.7;`, `n = random();`,
  `let n: Int = sqrt(2.0);`. It is a `[NARROW-001]` warning, so it compiles —
  write `int(...)` when you mean to truncate
- mixed-type output that assumes `+` will stringify numbers
- foreign object/JSON APIs: `JsonDoc`, `JsonNode`, `json.parseFile(...)`,
  `root.get(...)`, `Int.MIN`, `value.toString()`
- `write(...)` / `writeln(...)` for console output; use `print` / `println`
  (`writeln` remains correct for *file* handles — see the appendix)
- Markdown fences around the program

## Canonical vs accepted forms

| Topic | Canonical | Accepted | Avoid |
|---|---|---|---|
| Mutable binding | `let x: Int = 0;` | `let mut x: Int = 0;` | treating `let` as immutable |
| Loop keyword | `loop` (condition/range/infinite) | `while cond { }`, `for x in a..b { }` | inventing other loop syntax |
| Return | `ret value;` (`ret;` for Void) | — | `return value;` |
| Output | variadic `println(a, b)` in `fx` | text-only `+` concatenation | `Text + Int` guessing |
| Text equality | `a == b` | `string_eq(a, b)` | inventing `.equals(...)` |
| Text length | `string_len(t)` | `t.len` | inventing `strlen(...)` |
| Text → Int | `parse_int(t)` | — | inventing `Int(t)` / `t.toInt()` |
| Text → Real | `parse_float(t)` | — | inventing `Real(t)` |
| Text → Bool | `parse_bool(t)` | — | comparing to `"true"` by hand |
| Int → Text | `int_to_text(n)` | `itoa(n)` | inventing `n.toString()` |
| Real → Text | `formatfloat(r, prec)` — 1 or 2 args | `realtostr(r)` (always 6 dp) | `formatfloat(r, 0, prec)`; `r:0:prec` as a value |
| Char → Int code | `ord(ch)` | — | `int(ch)` (returns `0` for `Text`) |
| Int code → Char | `chr(code)` | — | a lookup table |
| Split text | `split(t, sep)` → `Text[]` | — | manual character scanning |
| Dynamic array length | `length(xs)` | `len(xs)`, `xs.len` | `toon_len(xs)` |
| TOON array length | `toon_len(node)` | — | `length(node)` |
| Method call | `counter.bump()` | `bump(counter)` if receiver obvious | class syntax |
| Transform a list | explicit `loop` building `xs + [v]` | — | `map(...)` / `filter(...)` |
| Record init | `new Point { x: 3, y: 4 }` | `Point(x: 3, y: 4)` | `fn new()` constructor |
| TOON nested lookup | bind and validate intermediates | nested calls when shape guaranteed | `_or` on a missing intermediate |
| Imports | verified `use "module";` | self-contained code | guessed modules |
| Method context | pass loop/local context as parameters | helper computes its own label | implicit capture of outer `i` |

## Conversions, math, and exact arities

These are the most-guessed names in the language. Read this table before writing
any numeric or string handling.

```aether
let parts: Text[] = split("12,7,5", ",");  // Text  -> Text[]  ["12","7","5"]
let n: Int = parse_int(parts[0]);          // Text  -> Int
let r: Real = parse_float("3.5");          // Text  -> Real
let ok: Bool = parse_bool("true");         // Text  -> Bool
let s: Text = int_to_text(99);             // Int   -> Text    (itoa also accepted)
let f: Text = formatfloat(3.14159, 2);     // Real  -> Text    "3.14"
let g: Text = realtostr(3.14159);          // Real  -> Text    always 6 dp
let code: Int = ord("A");                  // 1-char Text -> Int  65
let ch: Text = chr(65);                    // Int -> 1-char Text  "A"
let t: Int = int(3.7);                     // Real/Bool -> Int, truncating
```

`int(x)` casts `Real`/`Bool` only. Handed a `Text` it silently returns `0` — use
`parse_int` for numeric strings and `ord` for character codes.

**The `parse_*` family cannot fail, and that is the trap.** Every input yields a
value: `parse_int("abc")` is `0`, `parse_int("12x")` is `12` (leading digits
only, the rest discarded), `parse_float("zz")` is `0.0`, and `parse_bool` is
`false` for anything but `true`/`1`/`yes`/`t`. So `parse_int(raw) == 0` is true
for `"0"`, `"abc"`, and `""` alike — never treat the result as its own validity
check.

**`val` is the checked parse.** There is no `try_parse`, but `val(t, n, code)`
reports failure: `code` is `0` only when the *whole* string parsed, otherwise the
1-based position of the first bad character. `valreal(t, r, code)` is the `Real`
form. Both are pure — no `fx` needed, legal in a `@pure` function — and both
leave the destination **unmodified** on failure, so check `code` before reading
it. Note `val("12x", n, code)` fails where `parse_int("12x")` returns `12`; that
strictness is the point. Use `val` for any field from a file, an argument, an
environment variable, or a model. Declare both destinations first:

```aether
let n: Int = 0;
let code: Int = 0;
val(raw, n, code);
if code != 0 {
    n = -1;
}
```

**Arity traps.** These are caught at compile time as `BUILT-002`, but write them
right the first time:

| Helper | Arity | Note |
|---|---|---|
| `formatfloat(v)` / `formatfloat(v, prec)` | 1 or 2 | never 3 — no width slot |
| `realtostr(v)` | 1 | always 6 decimals |
| `clamp(x, lo, hi)` | 3 | |
| `min(a, b)` / `max(a, b)` | 2 | |
| `ord(ch)` / `chr(n)` / `trim(s)` | 1 | |
| `copy(s, start, count)` | 3 | `start` is 0-based |
| `pos(needle, s)` | 2 | 0-based; `-1` when absent |
| `split(t, sep)` | 2 | |
| `parse_int` / `parse_float` / `parse_bool` | 1 | |

**Math surface.** Names follow Pascal conventions — `arctan` not `atan`, `ln`
not `log`. Trig takes radians. Arguments and results are `Real` unless noted.

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

**`random` is two functions with different return types.** Assigning the
no-argument form to an `Int` truncates `[0, 1)` to a constant `0`, so every
"random" choice becomes the same choice — silently. When you want an index or a
roll, pass the bound:

```aether
let idx: Int = 0;
fx {
    randomize();
    idx = random(8);        // 0..7 -- correct
}
```

More generally, an implicit `Real` → `Int` narrowing raises `[NARROW-001]`:
`let n: Int = 3.7;` gives `3` and warns. It is only a warning, so the program
still compiles — write `int(expr)` when you mean to truncate, which silences it
and makes the intent visible.

Each thread gets its own random stream, so `par` branches draw independent
numbers. With no `randomize()` call a run is reproducible.

**Clock.** `realtimeclock() -> Int` returns Unix epoch seconds and is an
ordinary function — reach for this one. The wall-clock component readers are
*procedures* that write into var out-parameters rather than returning anything,
a calling convention that appears nowhere else in Aether except `readln`:

```aether
fn main() -> Void {
    let hour: Int = 0;
    let minute: Int = 0;
    let second: Int = 0;
    let centis: Int = 0;
    let year: Int = 0;
    let month: Int = 0;
    let day: Int = 0;
    let weekday: Int = 0;
    fx {
        let stamp: Int = realtimeclock();          // a function -- returns a value
        gettime(hour, minute, second, centis);     // a procedure -- fills its arguments
        getdate(year, month, day, weekday);
        println("epoch ", stamp);
        println(year, "-", month, "-", day, " ", hour, ":", minute, ":", second);
    }
    ret;
}
```

`let t = gettime();` fails with *"Built-in procedure 'gettime' cannot be used as
a function in an expression."* — it has no return value at all.

`Int / Int` is integer division (`7 / 2` is `3`). Force a `Real` operand when you
want decimals: `let pct: Real = ok * 100.0 / total;`

## Safe generation algorithm

1. Decide whether the program needs TOON.
2. Define `type` blocks before any function that instantiates them.
3. Define helper functions before their callers.
4. Give every function parameter an explicit type.
5. Prefer explicit `let name: Type = ...` unless the initializer is a literal or
   a call with a declared return type.
6. Put all output, task helpers, and AI calls inside `fx`.
7. Use `ret`, never `return`.
8. Close every parsed `ToonDoc` with `toon_close(doc)`.
9. Run the Validation Checklist before emitting.

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

- Comments: `// line comment`. Block comments compile, but generate `//`.
- Text literals are double-quoted; escape an inner quote as `\"`.
- Types: `Int`, `Real`, `Text`, `Bool` (`true` / `false`), `Void`, plus the
  opaque handles `ToonDoc`, `ToonNode`, `MStream`, `File`.
- `println(boolValue)` prints `true` or `false`.
- Operators: `+ - * / %`, `== != < <= > >=`, `!`, `&&`, `||` (short-circuit).
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
const Name: Text = "Aether";

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

**Loops.** `break` exits, `continue` skips, and a range is half-open:

```aether
loop index < total { index = index + 1; }   // condition
loop i in 0..count { total = total + i; }   // range: 0 up to count - 1
loop { break; }                             // infinite + break
```

A loop variable in `loop i in 0..n` is scoped to the loop. Declaring an
unrelated `let i: Int = 0;` elsewhere in the same function is fine.

**Operator precedence**, tightest to loosest: unary `!` / `-`, then `* / %`,
then `+ -`, then `<< >>`, then `< <= > >=`, then `== !=`, then `&`, then `^`,
then `|`, then `&&`, then `||`. When in doubt, parenthesize — it is always
accepted.

## Functions and tuple returns

Every parameter is typed and every function declares a return type. A function
must be defined before the function that calls it (ORDER-001).

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

A function may return a **tuple**, declared as `-> (T, U)`. Tuples are
deliberately narrow — they exist for the "two results, no record needed" case
and nothing more.

```aether
fn pairSquares(n: Int) -> (Int, Int) {
    ret (n, n * n);
}

fn main() -> Void {
    let (base, square) = pairSquares(6);     // destructure a DIRECT call
    let t = pairSquares(7);                  // or bind, then read positionally
    fx {
        println(base, " ", square);
        println(t.0, " ", t.1);              // zero-based, no type annotation
    }
    ret;
}
```

The limits, all reported as `TUP-001`:

- Destructuring works only on a **direct call to a top-level tuple-returning
  function**. A method, an undefined helper, or a nested expression is rejected
  with "tuple destructuring target is not a known tuple-return function".
- `.N` works only on a **bound variable**. `pairSquares(6).0` chained onto the
  call is rejected with a hint to bind it first.
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

Recursion through a tuple-returning function (direct or mutual) and calls to the
same one from concurrent `par` branches are both fine — a tuple return lowers to
a record returned by value, so it is reentrant per call.

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
  method a `self` parameter.
- A method's `@pre` / `@post` may reference `self.field`. A free-standing
  `fn bump(self: Counter)` with a `self`-referencing contract fails with
  `[SCOPE-001] identifier 'self' not in scope` — put the method and its contract
  inside the `type`.
- Fields may declare **constant** defaults (FIELD-003). `new Counter()` gives
  each field its declared default, otherwise the type zero: `Int` `0`, `Real`
  `0.0`, `Bool` `false`, `Text` empty.
- Records are **pointer-backed**: a mutation made through a callee is visible to
  the caller. (Arrays are not — see **Dynamic arrays**.)
- A top-level `fn bump(self: Counter) -> Int` acts as an extension method called
  as `counter.bump()`, but it cannot carry a contract that names `self`.
- Field and method names must not be reserved words (rule list above).
- A `type` may be declared after another `type` that refers to it; ordering only
  matters relative to the *functions* that use them.

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

**Always annotate a binding that holds a `new` instance or an array literal.**
`let c = new C();` followed by `c.inc();` fails with `argument 1 to 'c.inc'
expects type POINTER but got VOID`, and `let xs = [1, 2, 3];` fails with `cannot
infer the type of 'xs'`.

### Safe inference

Omit the type only for literals (`42`, `3.5`, `"text"`, `true`) and for calls to
functions or methods with declared return types. Annotate everything else: `new`
instances, array literals, TOON extractions, branchy results, and arithmetic
whose operand types are not visible at a glance.

## Effects: `fx`

```aether
fn main() -> Void {
    fx {
        println("hello");
    }
    ret;
}
```

Only the *builtin calls* are gated, not the surrounding structure — `if`, `loop`,
and blocks nest inside `fx` freely, so an entire loop can live in one block:

```aether
fx {
    loop i in 0..5 {
        println(i);
    }
}
```

A `@pure` function may not call any effectful builtin, directly or indirectly.

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

Exact-output discipline: print exactly the labels requested, format percentages
and averages explicitly, and add no extra headings, blank lines, or commentary.

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

The complete safe surface: `string_eq`, `string_len`, `split`, `parse_int`,
`parse_float`, `parse_bool`, `itoa` / `int_to_text`, `formatfloat` /
`realtostr`, `ord` / `chr`, `copy(s, start, count)` (substring),
`pos(needle, s)`, `trim(s)`, `stringofchar(ch, n)`. There is no `replace` and no
whole-string `to_upper` — build them with a loop over `ord` / `chr` if a task
demands one.

**`Text` indexes exactly like an array: 0-based, half-open slices.** `s[0]` is
the first character, `s[string_len(s) - 1]` the last, and `s[a..b]` is the
substring from `a` up to but not including `b`. One idiom covers iteration:

```aether
fn shout(s: Text) -> Void {
    fx {
        loop i in 0..string_len(s) {
            print(s[i]);
        }
        println("");
    }
    ret;
}
```

`pos` returns `-1` when the needle is absent — `0` is a legitimate match at the
first character, so test `pos(...) >= 0`, never `pos(...) > 0`.

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

`xs[a..b]` is half-open, matching `loop i in a..b`. There is no first-class
Range value — `a..b` is meaningful only inside `[...]` or a `loop` header.

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
are independent arrays, so a table may be jagged. Declaring `Int[]` and then
writing `dp[i][j]` is `ARR-002`: the rank of the *declaration* is what is wrong,
not the indexing syntax.

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
an `[ARR-001]` warning. It compiles, but it is almost certainly a mistake — this
is the same asymmetry: the identical pattern on a *record* parameter works,
because records are pointer-backed.

`println` does not stringify arrays. `println("data: ", xs)` prints the internal
representation, not the elements, and this is not an error. Loop and print each:

```aether
fx {
    loop i in 0..length(xs) {
        print(xs[i], " ");
    }
    println("");
}
```

## Writing what the surface does not give you

`BUILT-001` forbids inventing helpers, so when a task needs one that does not
exist, write the loop. These are the canonical shapes for the helpers models most
often reach for. Copy them rather than guessing a name.

**Sum, mean, and extremes over an `Int[]`.** There is no `sum` or `mean`.

```aether
@pure
fn total(xs: Int[]) -> Int {
    let acc: Int = 0;
    loop i in 0..length(xs) {
        acc = acc + xs[i];
    }
    ret acc;
}

@pure
fn mean(xs: Int[]) -> Real {
    if length(xs) == 0 {
        ret 0.0;
    }
    ret total(xs) * 1.0 / length(xs);
}
```

For the extremes, seed from element `0` and fold with `min` / `max`, which do
exist:

```aether
@pre length(xs) > 0
@pure
fn largest(xs: Int[]) -> Int {
    let best: Int = xs[0];
    loop i in 1..length(xs) {
        best = max(best, xs[i]);
    }
    ret best;
}
```

**Sort.** There is no `sort` and no comparator, because functions are not values
(FUNC-001). Write an insertion sort over a copy:

```aether
@pure
fn sorted(xs: Int[]) -> Int[] {
    let out: Int[] = xs;               // arrays copy by value, so this is a clone
    loop i in 1..length(out) {
        let v: Int = out[i];
        let j: Int = i - 1;
        loop j >= 0 {
            if out[j] <= v {
                break;
            }
            out[j + 1] = out[j];
            j = j - 1;
        }
        out[j + 1] = v;
    }
    ret out;                           // must return it -- see Dynamic arrays
}
```

**Join.** There is no `join`; build the `Text` with a separator guard:

```aether
@pure
fn joinText(parts: Text[], sep: Text) -> Text {
    let out: Text = "";
    loop i in 0..length(parts) {
        if i > 0 {
            out = out + sep;
        }
        out = out + parts[i];
    }
    ret out;
}
```

**Case conversion.** There is no `to_upper`. Map code points with `ord` / `chr`:

```aether
@pure
fn upper(s: Text) -> Text {
    let out: Text = "";
    loop i in 0..string_len(s) {
        let c: Int = ord(s[i]);
        if c >= 97 && c <= 122 {
            out = out + chr(c - 32);
        } else {
            out = out + s[i];
        }
    }
    ret out;
}
```

**Contains, starts-with, replace.** `pos` gives you the first two directly;
remember it returns `-1` when absent, so compare against `>= 0`.

```aether
@pure
fn contains(haystack: Text, needle: Text) -> Bool {
    ret pos(needle, haystack) >= 0;
}

@pure
fn startsWith(s: Text, prefix: Text) -> Bool {
    if string_len(prefix) > string_len(s) {
        ret false;
    }
    ret s[0..string_len(prefix)] == prefix;
}

@pure
fn replaceFirst(s: Text, from: Text, to: Text) -> Text {
    let at: Int = pos(from, s);
    if at < 0 {
        ret s;
    }
    ret s[0..at] + to + s[at + string_len(from)..string_len(s)];
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

**Validating before a parse.** The `parse_*` family cannot report failure, so
when a field must be entirely numeric, check it first:

```aether
@pure
fn isAllDigits(s: Text) -> Bool {
    if string_len(s) == 0 {
        ret false;
    }
    loop i in 0..string_len(s) {
        let c: Int = ord(s[i]);
        if c < 48 || c > 57 {
            ret false;
        }
    }
    ret true;
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
  `toon_get_int_or`, `toon_get_real_or`, `toon_get_bool_or`
- node values: `toon_text_value(node)`, `toon_int_value(node)`,
  `toon_real_value(node)`, `toon_bool_value(node)`, `toon_null_value(node)`
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

**Getters take a `ToonNode`, never the `ToonDoc`.** Never this:

```aether
let doc: ToonDoc = toon_parse("{\"name\":\"Aether\"}");
let name: Text = toon_get_text(doc, "name");     // TOON-001
```

Always this:

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

**Key fidelity (KEY-001).** Copy JSON keys exactly. Never flatten a nested object
into a guessed key (`"appName"`) or a dotted key (`"server.port"`):

```aether
let server: ToonNode = toon_key(root, "server");
let port: Int = toon_get_int_or(server, "port", 0);
```

**Nested lookup safety (NEST-001).** A `_or` fallback protects only the final
lookup, not the path to it. Make the path itself total with
`toon_key_or(node, key, toon_null())`: on a missing key it yields a null node,
and a null node absorbs the rest of the walk — `toon_len` is 0 and every `_or`
getter returns its fallback — so a chain of any depth is safe with no `if`.

```aether
let spec: ToonNode = toon_key_or(row, "spec", toon_null());
let mem: ToonNode = toon_key_or(spec, "mem", toon_null());
let gb: Int = toon_get_int_or(mem, "gb", 0);   // 0 if spec or mem is absent
```

Prefer this to a stack of `if toon_has_key(...)` guards, which is correct but
costs a nesting level per segment. What is never safe is bare `toon_key` on a key
that may be absent — `toon_get_int_or(toon_key(row, "spec"), "cores", 0)` is the
unguarded case. (`toon_has_at` is the one accessor that does not absorb a null
node: it raises rather than returning false.)

## Concurrency: `par`

`par { ... }` runs your own functions concurrently and joins before continuing.
The body holds **direct call statements only** — no assignments, no loops, no
inline `fx`. Wrap any work in a `fn` and call that.

Results come back through pointer-backed records passed as arguments, **one
record per branch**. Sharing a record across branches races and is rejected
(PAR-001).

```aether
type Tally {
    count: Int = 0;
}

fn tally(t: Tally, upTo: Int) -> Void {
    loop i in 0..upTo {
        t.count = t.count + 1;
    }
    ret;
}

fn main() -> Void {
    let a: Tally = new Tally();
    let b: Tally = new Tally();
    par {
        tally(a, 100);
        tally(b, 200);
    }
    fx { println("a=", a.count, " b=", b.count); }
    ret;
}
```

Read-only handles (a `ToonNode`, an `Int`) may be shared across branches freely;
it is the written-to record that must be private.

This is the capture-free way to parallelize user code. The task helpers in the
appendix are a lower-level API over runtime builtins, not user functions.

## Contracts

```aether
@pure
@pre score >= 0
@post result >= 0
fn normalize(score: Int) -> Int {
    ret clamp(score, 0, 100);
}
```

- Annotations sit directly above the function, never inside it, never bare.
- `@post` may reference `result`.
- `@cost 5ms` accepts units `ns us ms s op ops step steps`. It is syntax-checked
  but non-binding.
- `@pure` forbids every effectful builtin, transitively.
- On a collection return, a contract must compare a **property**, not the
  collection: `@post length(result) > 0`, never `@post result > 0`. Same for a
  `@pre` over an array parameter: `@pre length(xs) > 0`.

## Modules

Most generated Aether should be a single file. When a task supplies modules:

- import with `use "module_name";` — the string resolves to a sibling file of
  that exact name
- imported names match exported names exactly (MOD-001)
- `mod ModuleConsts { export ... }` corresponds to `use "module_consts";`
- write `@pure` above `export fn` when combining them
- assume modules export `const` and `fn`

```aether
use "bench_support";

fn main() -> Void {
    let score: Int = clampSupport(41);
    fx { println(score); }
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

**TOON array classification.**

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

    let doc: ToonDoc = toon_parse("[{\"name\":\"A\",\"score\":95},{\"name\":\"B\",\"score\":72}]");
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

    let doc: ToonDoc = toon_parse("{\"rows\":[{\"meta\":{\"code\":\"A1\"}},{\"meta\":{}},{\"broken\":true}]}");
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

**Long report programs.** Shape them as: parse → `toon_root` → `toon_key` for the
item array → one pure normalize helper → one pure classify helper → one mutable
totals `type` → one loop that extracts, classifies, updates totals, and prints →
one final totals block → `toon_close(doc)`.

## Repair rules

The compiler prints a stable code in brackets, and on newer builds a
`help: see <CODE> ...` line. Read the code, then apply the fix.

- **[FX-001]** output, task, or `ai_chat` call outside an effect block → wrap it
  in `fx { ... }`.
- **[SYN-001]** non-Aether syntax → `ret` not `return`, `type` not `class`; drop
  `var`, `def`, `=>`. Also a field or method named after a reserved word →
  rename the member.
- **[SCOPE-001]** the catch-all. It is one of:
  - a helper not listed in this document → it does not exist; inline the logic
  - an export called by a guessed name → use the exact exported name
  - a type or helper used before it is defined → define it earlier
  - a method reaching an outer local → pass it in as a parameter
  - `method '<m>' is not defined on type '<T>'` → define it inside the `type`
  - an **unknown type name in an annotation** — any position (binding, parameter,
    return type, record field, tuple item) and any array depth, so `let v: Bogus[]`
    and `fn f(p: Bogus)` fail exactly as `let v: Bogus = 1` does → fix the spelling,
    or declare `type Bogus { ... }` (order does not matter)
  - a genuinely undeclared name → declare it earlier or pass it in
- **[BUILT-002]** right builtin, wrong argument count → check the arity table.
- **[NAME-001]** a local redeclared in the same scope → pick a fresh name.
- **[IMP-001]** an invented or malformed import → remove the `use`, or write
  `use "module_name";` and call the exports directly.
- **[TYPE-001]** a type cannot be inferred → annotate it, including untyped array
  literals. Also covers `toon_len` vs `length` confusion.
- **`expects type POINTER but got VOID`** on a method call (no code) → the
  receiver is an inferred `new` binding; annotate it.
- **[TOON-001]** handle misuse → go through `toon_root(doc)`; never do arithmetic
  on, or cross-assign, handles.
- **[MS-001]** stream misuse → declare stream bindings `MStream`, read with
  `mstreambuffer(ms)`.
- **[FIELD-002]** `Unknown field` → use the exact declared name.
- **[FIELD-003]** a non-constant field default → use a literal, or set the value
  at construction with `new T { field: value }`. A *type*-mismatched default is
  `[TYPE-001]` instead.
- **[FLOW-001]** a fallthrough path with no return value → add a final `ret`.
- **[FLOW-002]** a bare `ret;` in a non-`Void` function → return a value, or
  declare `-> Void`.
- **[ANN-001]** a misplaced annotation, or a `@pure` function calling an effect →
  move the annotation above the function; keep effects out of pure code.
- **[TUP-001]** tuple misuse → destructure a direct call, or bind and read one
  field; never chain onto the call.
- **[ARR-001]** (warning) a `Void` function mutating an array parameter → return
  the array and reassign at the call site.
- **[NARROW-001]** (warning) a Real value stored in an `Int` target → the
  fraction is discarded. Use a `Real` target, or `int(...)` to say you meant it.
- **[ARR-002]** a one-dimensional array indexed twice → declare it `T[][]` and
  build rows as arrays, or index once with a computed offset
  (`grid[r * width + c]`).
- **[ARR-003]** (runtime) index outside the array's range; the message gives the
  index, the dimension and the valid range, and the `[Error Location]` line gives
  the source line. `valid indices are 0..0` means one element exists, so the row
  is usually **not appended yet**: while building a table row by row, the current
  row is the local `row`, not `table[i]` — `table[i]` only exists after
  `table = table + [row];`. Earlier rows read fine as `table[i - 1][j]`.
- **[PREC-001]** (warning) `flags & mask != 0` → `&`, `|` and `^` bind looser
  than the comparisons, so this groups as `flags & (mask != 0)` and yields an
  **Int**, not a Bool. Parenthesize: `(flags & mask) != 0`.
- **[MUT-001]** `let mut` → drop `mut`.
- **[PAR-001]** the same record in more than one `par` branch → give each branch
  its own and combine afterwards.
- **[PAR-002]** a non-call statement inside `par` → wrap the work in a `fn`.

If the program *compiles* but the output is wrong, no code is printed. Those are
authoring rules nothing can check: extra headings, wrong spacing or precision (an
integer where decimals were wanted → add a `Real` operand such as `100.0`),
guessed JSON keys, an unguarded nested lookup, or iterating an object root.
Re-read the prompt and match it exactly.

## Validation checklist

- all output, task, and `ai_chat` calls inside `fx { ... }` (FX-001)
- every called helper appears in this document (BUILT-001) and with the right
  argument count (BUILT-002)
- imports verified; exported names used exactly (IMP-001, MOD-001)
- all parameters typed; `new` instances and array literals annotated (TYPE-001)
- `ret` not `return`; `type` not `class`; no `let mut` (SYN-001, MUT-001)
- no field or method named after a reserved word; no constructor method
- no arithmetic on or cross-assignment of TOON handles; every doc closed
- stream bindings declared `MStream`; bodies read via `mstreambuffer` (MS-001)
- object roots: named array extracted (ROOT-001); keys copied exactly (KEY-001);
  intermediates guarded before `_or` (NEST-001)
- `toon_len` vs `length` used correctly (LEN-001)
- a `Real` operand wherever decimals matter; no accidental `Real` → `Int` truncation
- each `par` branch writes its own record (PAR-001)
- functions return on every reachable path (FLOW-001)

---

## Appendix: host capabilities

Reach for these only when the task requires them.

### Files and environment

All effectful — inside `fx`, and banned in `@pure`.

- `fileexists(path) -> Bool`, `getcurrentdir() -> Text`
- `getenv(name) -> Text`, `getenvint(name, fallback) -> Int`
- `mkdir(path)`, `rmdir(path)`
- `paramcount() -> Int`, `paramstr(i) -> Text` (`paramstr(0)` is the program)

For file *contents*, declare a `File` variable — a handle type distinct from
`Text` — and use `assign` / `reset` / `rewrite` / `readln` / `writeln` / `eof` /
`close` / `erase` / `rename`:

```aether
fn main() -> Void {
    let f: File;
    fx {
        assign(f, "notes.txt");
        rewrite(f);
        writeln(f, "first line");
        close(f);

        reset(f);
        loop {
            if eof(f) { break; }
            let line: Text;
            readln(f, line);   // writes into `line` -- unique to this builtin
            println(line);
        }
        close(f);
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
- `mstreamcreate() -> MStream`, `mstreamfromstring(text) -> MStream`,
  `mstreambuffer(ms) -> Text`, `mstreamfree(ms)` — all pure. There is no
  stream-write builtin: to get text into a stream use `mstreamfromstring`.

```aether
fn fetch(url: Text) -> Void {
    fx {
        let session: Int = httpsession();
        let out: MStream = mstreamcreate();
        let status: Int = httprequest(session, "GET", url, "", out);
        if status == 200 {
            println(mstreambuffer(out));
        }
        mstreamfree(out);
        httpclose(session);
    }
    ret;
}
```

`mstreambuffer(out)` is the only way to read the body. A JSON response pairs
directly with TOON: `toon_parse(mstreambuffer(out))`.

### Sockets

A BSD-style API. The names are `socket*` and `dnslookup` — there is no
`tcpsocket*`, `udpsocket*`, or `resolve`. All effectful. `socketreceive` returns
an `MStream`, never `Text`.

- `socketcreate(type[, family]) -> Int` — `type` `0`=TCP, `1`=UDP; `family`
  `4`=IPv4 (default), `6`=IPv6
- `socketconnect(socket, host, port)`, `socketbind(socket, port)`,
  `socketbindaddr(socket, host, port)`, `socketlisten(socket, backlog)`,
  `socketclose(socket)` — all return `0` or `-1`
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

All effectful.

- `sleep(ms: Int) -> Void` — a blocking millisecond pause. Prefer this over a
  busy-wait loop.
- `task_spawn(target: Text, name: Text, arg) -> Int`,
  `task_queue(target: Text, name: Text, arg) -> Int`,
  `task_wait(handle: Int) -> Int`, `task_lookup(name: Text) -> Int`,
  `task_status(handle) -> Int`, `task_result(handle) -> Int`,
  `task_stats() -> Array`, `task_stats_json() -> Text`
- `ai_chat(model, messages, system = "", apiKey = "", endpoint = "") -> Text`
- probes: `has_ai() -> Bool`, `has_toon() -> Bool`,
  `has_builtin(category: Text, function: Text) -> Bool` — true for extended
  *and* core builtins; a core builtin matches on the function name whatever
  category you pass

`task_wait` waits on a handle, not a duration. `task_spawn` and `task_queue`
dispatch an allow-listed **runtime builtin by name** (for example `"delay"`) —
they cannot run a user-defined function. For your own code, use `par`.
