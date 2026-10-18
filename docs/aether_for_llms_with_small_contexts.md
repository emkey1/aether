# Aether for LLMs — Concise Guide (for small contexts)

*Guide version: 2026-10-18-1*

## Highest-Value Rules

1. **FX-001.** Every effectful builtin must be inside `fx { ... }`: output,
   task helpers, `ai_chat`, and all host interaction — files (`read_file`,
   `write_file`, `file_exists`), stdin (`read_line`), env (`getenv`), CLI (`args`),
   `random`/`randomize`, clock (`clock_ms`), `http*`/`socket*`, `sqlite*`. Pure math/Text/array/conversion do
   not need `fx`; `@pure` functions may call only pure builtins and other
   `@pure` functions.
2. **SYN-001.** Aether syntax only: `fn`, `let`, `const`, `ret`, `if`, `loop`,
   `type`, `mod`, `use`. Never `return`, `class`, `var`, `def`, `func`, `=>`.
   `while cond { }` / `for x in a..b { }` compile (accepted)
   but `loop` is canonical and covers every form — prefer it.
3. **BUILT-001.** Every builtin named here exists as shown. Prefer them; do
   not invent helpers (`substring`, `to_upper`, `s.upper()`, ...) or guess a name
   beyond them.
   **BUILT-002:** right name, wrong argument count is its own error, caught at
   compile time for the fixed-arity conversion/math helpers. Most common:
   `formatfloat(r, 0, 2)` — it takes `(value, precision)`, and the width slot
   belongs only to the `println`-only `r:0:prec` form.
4. **IMP-001.** Never invent `use "..."` imports. Only import verified modules.
5. **MOD-001.** If a module exports `getFortyTwo`, call `getFortyTwo`. Do not
   rename exports to `get_forty_two`, `AetherName`, `APP_NAME`, or other guesses.
6. **TOON-001.** `ToonDoc` and `ToonNode` are opaque handles: no arithmetic,
   never assign one where the other is expected.
7. **TYPE-001.** If inference is not obviously safe, add the type explicitly.
8. **ANN-001.** `@pre`, `@post`, `@pure`, `@cost` go directly above the
   function, never inside it, never bare (`@pre` with no expression).
9. **MUT-001.** Plain `let` is already mutable. Never generate `let mut`.
10. **LEN-001.** `toon_len(node)` for TOON arrays; `length(xs)` for arrays
    and `Text`; crossing them fails at run time.
11. **TUP-001.** Tuples are narrow: destructure a direct top-level call,
    `let (a, b) = pair();`, or bind it and read a field, `let t = pair(); t.0;`
    (zero-based) -- never `pair().0` on the call. A tuple fn may `ret g(x);`
    when `g` returns the same tuple type. If the producer is a method or an
    expression, return a record instead. An index past the arity is a
    compile-time TUP-001. A tuple `@post` uses `result.0`, `result.1`.
    Recursion through a tuple-returning function is fine.
12. **OUT-001.** Return raw Aether source only. No Markdown fences.
13. **ROOT-001.** If the JSON starts with `{`, extract the named array with
    `toon_key(root, "...")` before iterating. Only iterate `root` directly
    when the JSON starts with `[`.
14. **SCOPE-001.** A name must be declared before use and still be in scope at
    the use site. Do not rely on guessed globals or out-of-scope loop locals.
15. **METH-001.** Methods do not capture outer locals. If a method needs `i`,
    `name`, or another caller value, pass it as a parameter.
16. **FIELD-001.** Inside a method, a local may reuse a field name. Bare
    `valid` means the local; `self.valid` means the field.
17. **FIELD-002.** Record and type field names must exist exactly as declared.
    Do not invent fields.
18. **FIELD-003.** A field default must be a **literal**: `count: Int = 0`,
    `on: Bool = true`, `xs: Int[] = []`. Not an expression, not another field,
    not `self`, not a call, and **not a named `const`** — `n: Int = MAX;` is
    rejected as surely as `n: Int = MAX + 1;`. Set anything else at
    construction: `new T { field: value }`.
19. **FLOW-001.** Every non-`Void` helper must return a value on every
    reachable top-level path.
20. **FMT-001.** If the prompt specifies exact output, match it exactly:
    spacing, casing, line order, and decimal precision.
21. **NAME-001.** Names ignore case. Do not redeclare a local or parameter,
    name a loop variable after a variable in scope, or name a local or
    parameter after a `const` (`limit` beside `LIMIT`). Pick fresh names.
22. **MOD-002.** Canonical import form is `use "module_name";`. After import,
    call exported names directly. Never guess `export { ... }` syntax or the
    foreign object/JSON APIs listed under **TOON rules**.
23. **FUNC-001.** Functions are not values: no anonymous `fn(...) -> T { ... }`,
    no lambdas, never pass a function as an argument. No
    `map`/`filter`/`reduce`; use a `loop`. To run your own code concurrently,
    use `par { f(); g(); }`. A `fn` inside a free function's body may read and
    assign that function's parameters and earlier locals (the enclosing
    variable changes).
24. **PAR-001.** Each `par` branch must own the record it writes. Passing the
    **same** record to two branches races — the parallel writes corrupt the heap.
    So does a local that a nested `fn` assigns. Give each branch its own record
    and combine the results after the block.
25. **MS-001.** `MStream` is the opaque memory-stream handle type (HTTP
    response bodies). `mstream_new()`/`mstream_from_text(text)` return
    `MStream` — never `Int`, never `Text`. Body text comes from
    `mstream_text(ms) -> Text`; release with `mstream_free(ms)`.

Default stance: single-file programs; variadic `println("label = ", v)` over
mixed-type `+`; explicit types for TOON values and non-trivial helper results;
real logic, never hard-coded expected output; preserve provided export names
exactly.

Not every rule above is a *compiler* check: some are emitted as a stable
`[CODE]`, some are reported under a broader
code (mostly SCOPE-001), and the rest are authoring rules the compiler cannot
catch. The **Repair rules** section lists each emitted code with its fix and
flags the authoring-only ones.

## Core syntax

- Comments: prefer `// line comment`; `//` is never division (write `div`).
  Block comments are accepted, but models should still generate `//`. Text
  literals are double-quoted; escapes: `\"`, `\\`, `\n`, `\t`.
- Types: `Int`, `Real`, `Text`, `Bool` (`true`/`false`), `Void`, plus opaque
  `ToonDoc`/`ToonNode`/`MStream`. `Float` and `String` are accepted spellings of
  `Real` and `Text`; every other foreign spelling (`Double`, `Integer`,
  `Boolean`, `Char`, `List`, `Map`, lowercase `int`) is a `TYPE-002` error.
  `println(boolValue)` prints `true` or `false`. To print mixed values use
  variadic `println`, not `+`; to build a `Text`, join `Text` with `+` after
  converting numbers (`int_to_text`, `formatfloat`).
- Operators: `+ - * / %`, `== != < <= > >=`, `!`, `&&`, `||` (logical, short-circuit;
  `not` / `and` / `or` are exact synonyms). Arrays compare with `==` / `!=`.
  `-7 % 2` is `-1` (the dividend's sign): test odd with `n % 2 != 0`.
- Bitwise/shift (Int only): `& | ^`/`xor` (`^` and `xor` are the same operator), `<< >>`.
  Example: `6 & 3` = `2`, `6 << 1` = `12`. Not logical -- use `&&`/`||` for Bool.
  They bind **looser than the comparisons**: `flags & mask != 0` is
  `flags & (mask != 0)` and yields an Int. Write `(flags & mask) != 0`
  (PREC-001).
- Numeric literals allow `_` digit separators between digits: `1_000_000`, `0xFF_FF`.

```aether
fn add(a: Int, b: Int) -> Int {
    ret a + b;
}
```

Every `fn` declares `-> ReturnType`; procedures use `-> Void` and bare `ret;`.
`fn helper(x: Int) { ... }` is invalid. Two results come back as a tuple:

```aether
fn minMax(a: Int, b: Int) -> (Int, Int) {
    ret (min(a, b), max(a, b));
}

fn main() -> Void {
    let (lo, hi) = minMax(9, 4);     // destructure a direct call
    let t = minMax(7, 5);            // or bind, then read t.0 / t.1
    fx { println(lo, " ", hi, " ", t.1); }
    ret;
}
```

```aether
const MAX_SCORE: Int = 100;
let count: Int = 0;
count = count + 1;
```

Conditionals (parentheses optional; canonical multi-branch is sequential
`if` + `ret`):

```aether
if score >= 90 {
    ret "ready";
}
ret "blocked";
```

Statement-level `else` and chained `else if` are supported.

Inline `if ... else ...` expressions are allowed anywhere a value is expected
— declarations, assignments, `ret`, and call arguments including
`println(...)`. Chained `else if` works in expression position too:

```aether
let label: Text = if ready { "ready" } else { "blocked" };
fx { println("status: ", if ready { "ready" } else { "blocked" }); }
let grade: Text = if score > 90 { "A" } else if score > 80 { "B" } else { "C" };
```

Loops (`break` exits, `continue` is supported). Prefer the range and foreach
forms: they **declare their own loop variable**, so they need no `let`. A range
is half-open: 1 through `n` is `1..n + 1`, and -1, 0, 1 is `-1..2`.

```aether
loop i in 0..count { fx { println(i); } }   // half-open: runs 0..count-1
loop i in 0..count step 2 { fx { println(i); } } // stepped; `step -1` counts down
loop item in items { total = total + item; } // foreach: an array, a Text, or a ToonNode array
loop index < total { index = index + 1; }   // condition form: declare `index` first
loop { break; }
```

`all x in xs: x > 0` and `any i in 0..n: xs[i] == t` are `Bool` expressions over
the same headers; empty gives `all` true, `any` false. They work in contracts.

## Records: `type`

```aether
type Counter {
    value: Int;            // fields end with ';'

    fn bump() -> Int {
        self.value = self.value + 1;   // lowercase self, never Self
        ret self.value;
    }

    fn bumpTwice() -> Int {
        self.bump();                   // a sibling method needs self. too
        ret self.bump();
    }
}
```

- methods live **inside** the `type`; `self` is implicit (never a parameter),
  members are not: write `self.field`, `self.method()`. A method's
  `@pre`/`@post` may reference `self.field`
  (`@pre self.value >= 0` above `fn bump`). A free-standing `fn bump(self: Counter)`
  with a `self`-referencing contract fails `[SCOPE-001] identifier 'self' not in
  scope`.
- a field may declare a **literal** default: `value: Int = 0` (FIELD-003).
  A literal only — no named `const`, other field, `self`, or call; for a
  computed value use `new T { field: value }`.
- `new Counter()` gives each field its declared default, else the type zero
  (Int `0`, Real `0.0`, Bool `false`, Text empty)
- construct with values: `new Point { x: 3, y: 4 }` (partial sets ok; an unset
  field keeps its default); bare `Point { x: 3, y: 4 }` / `Point(x: 3, y: 4)`
  also accepted
- records are pointer-backed: mutations through a callee are visible to the
  caller
- a top-level `fn bump(self: Counter) -> Int` is an extension method, called as
  `counter.bump()`
- **field & method names must not be Aether keywords** (`new`, `for`, `if`,
  `loop`, `ret`) **or operator words** (`div`, `mod`, `xor`, `and`, `or`, `not`)
  (SYN-001). Other languages' words (`word`, `text`, `int`, `match`, `class`)
  are ordinary names.

## Constructing records and typing bindings

`new T()` is the only constructor. Set fields at construction with
`new T { field: value }` (the recommended way; a partial set keeps each unset
field's declared default or type zero), or allocate then assign, or use a
top-level factory `fn`. There is **no** `fn new()`, `fn __init__`, or `T.new()`
(SYN-001):

```aether
let p: Point = new Point { x: 3, y: 4 }; // set fields at construction
let q: Point = new Point();              // fields take defaults / type zeroes
q.x = 3;                                 // or assign after allocation
```

Always annotate an array literal (TYPE-001): `let xs = [1, 2, 3];` fails
(`cannot infer the type of 'xs'`); write `let xs: Int[] = [1, 2, 3];`.
`let c = new C();` infers `C`. Record literals may nest directly in an array
literal: `let ps: Point[] = [new Point { x: 1, y: 2 }, new Point { x: 3, y: 4 }];`.
Appending in a loop (`ps = ps + [p];`) also still works.

## Safe inference

Omit the type only for: literals (`42`, `3.5`, `"text"`, `true`) and calls to
functions/methods with known declared return types. Annotate everything else —
array literals, TOON extractions, branchy results, and arithmetic where operand
types aren't visible at a glance.

```aether
loop i in 1..4 {
    let cube: Int = i * i * i;
    fx { println(i, " cubed is ", cube); }
}
```

## Effects (FX-001)

```aether
fn main() -> Void {
    fx {
        println("hello");   // good: inside fx
    }
    ret;
}
```

`println("hello");` at function scope is wrong — wrap it in `fx { ... }`.
`if`/`loop`/blocks nest inside `fx` freely — only the builtin calls are
gated, not the structure — so a whole loop can go in one `fx` block:
`fx { loop i in 0..n { println(i); } }`.

## Printing and Real formatting

```aether
fx {
    println("Drop ", j, " -> ID: ", tx.id);  // variadic, never '+' guessing
    println(pct:0:2);                        // width:precision => 95.50
}
```

`println(realValue)` defaults to 6 decimals; use `value:0:2` style when exact
output matters. Integer quotient: `a div b` (`7 div 2` is `3`), never `/`.
For decimals use a `Real` operand:

```aether
let pct: Real = ok * 100.0 / total;
```

Exact-output discipline:

- print exactly the labels requested, for example `avg0=0`, not `avg0 = 0`
- `println(a, b)` puts **nothing** between its arguments: write
  `println(a, " ", b)`; each requested output line is its own `println`
- for percentages and averages that must show decimals, format them explicitly
- do not add extra headings, blank lines, or explanatory text

## Text

```aether
let same: Bool = name == "Aether";     // canonical; string_eq(a,b) accepted
let nameLen: Int = string_len(name);   // canonical; name.len accepted
let tag: Text = name + "#" + int_to_text(count);   // `+` joins Text

// turning text into scalars — these exist, call them (do not inline):
let parts: Text[] = split("12,7,5", ",");  // Text -> Text[]  ["12","7","5"]
let n: Int = parse_int(parts[0]);          // Text -> Int
let r: Real = parse_float("3.5");          // Text -> Real
let ok: Bool = parse_bool("true");         // Text -> Bool
let s: Text = int_to_text(99);             // Int  -> Text
let f: Text = formatfloat(3.14159, 2);     // Real -> Text  "3.14" (realtostr(r) = 6 dp)
let code: Int = ord("A");                  // Text(1 char) -> Int  65
let ch: Text = chr(65);                    // Int -> Text(1 char)  "A"
let (v, good) = try_parse_int("12x");      // checked: (0, false); " 42" -> (42, true)
```

Text helpers (all pure; positions 0-based, `-1` when absent): `contains(s, x)`,
`starts_with`, `ends_with`, `index_of(s, x)`, `count(s, x)`, `upper(s)`,
`lower(s)`, `trim(s)`, `replace(s, old, new)` (every occurrence),
`repeat(s, n)`, `reversed(s)`, `join(parts, sep)`, `split`. Not Aether:
`copy`, `pos`, `itoa`, `val`, `File`/`readln`, `paramstr`, `gettime`,
`setlength`. Note: the `value:width:precision` spec only works
inside `println`; use `formatfloat` to build a `Text`. `formatfloat` takes one
or two arguments — never borrow the width slot from the `println` form:
`formatfloat(r, 0, 2)` is a compile-time `[BUILT-002]`. `int(x)` is a numeric
cast (`Real`/`Bool` -> `Int`, truncating) -- it does not parse or read the
code point of `Text`; passing it a `Text` is a `TYPE-001` error (`Int(x)`,
`real(n)`, `bool(n)` are accepted cast spellings). Use `parse_int` for numeric
strings and `ord` for character codes.

The `parse_*` family never reports failure: `parse_int("abc")` is `0`,
`parse_int("12x")` is `12` (leading digits only, rest discarded), and
`parse_float("zz")` is `0.0`. A `0` therefore does not mean the text was
numeric. When that distinction matters use `let (n, ok) = try_parse_int(t);`
(or `try_parse_real`): `ok` is `true` only if the whole text parsed, and `n` is
`0` otherwise.

There is no map, dict, or set type: use two parallel arrays and a linear scan,
or a `type` with an array field. For keys from a parsed payload, use the TOON
accessors — the node is already keyed.

**`Text` indexes exactly like an array: 0-based, half-open slices.** `s[0]` is
the first character, `s[length(s) - 1]` the last, and `s[a..b]` is the
substring from `a` up to but not including `b`; `s[i] = "x"` replaces one
character and `loop ch in s { }` visits each. So one idiom covers both:

```aether
fx { loop i in 0..length(s) { print(s[i]); } }   // every character, exactly once
```

## Math

Pascal naming: **`arctan`** not `atan`, **`ln`** not `log`. Trig in radians.
Args/results are `Real` unless noted.

- rounding (→ `Int`): `round`, `trunc`, `floor`, `ceil`; sign: `abs`
- powers: `sqrt`, `sqr`, `pow(base, exp)` (`power` alias), `exp`, `ln`, `log10`
- trig: `sin`, `cos`, `tan`, `arcsin`, `arccos`, `arctan`, `atan2(y, x)`, `cotan`
- hyperbolic: `sinh`, `cosh`, `tanh`
- helpers: `min(a, b)`, `max(a, b)`, `clamp(x, lo, hi)`, `odd(n) -> Bool`,
  `factorial(n)`, `fibonacci(n)`, `randomize()`, and **two different `random`s**:
  `random()` returns a **Real** in `[0,1)`, `random(n)` an **Int** in `[0,n)`.
  Assigning `random()` to an `Int` truncates to a constant `0` -- `[NARROW-001]`.

```aether
let pi: Real = 16.0 * arctan(1.0 / 5.0) - 4.0 * arctan(1.0 / 239.0);
```

## Dynamic arrays

```aether
let xs: Int[] = [];
xs = xs + [7];              // append (any literal length, incl. [7, 8] or [])
let n: Int = length(xs);    // len(xs) and xs.len accepted
let first: Int = xs[0];     // indexed read
xs[0] = 9;                  // indexed write
let ys: Int[] = [1, 2, 3];  // multi-element literal
xs = xs + ys;                // concatenation: `arr1 + arr2`, both already-built arrays
let mid: Int[] = ys[0..2];  // slice: half-open range, elements 0 and 1 (a copy, not a view)
```

**Nested arrays**: a row is an `Int[]`, so a table is `Int[][]`. Build it
`row = row + [v];` then `table = table + [row];`, index `table[r][c]`, and bound
the inner loop with `length(table[r])` (rows may be jagged). Declaring `Int[]`
and indexing twice is `ARR-002`.

Array helpers (pure, new values): `sum(xs)`, `min_of(xs)`,
`max_of(xs)`, `sorted(xs)`, `sorted_desc(xs)`, `reversed(xs)`, `contains(xs, v)`,
`index_of(xs, v)`, `count(xs, v)`, and for records
`sorted_by(rs, "field")` / `sorted_by_desc` (a literal field name). `[0; n]` is
n zeros; shrink with a slice (`xs = xs[..n];`); `xs == ys`
compares element by element.
`a..b` is half-open in a slice as in `loop i in a..b`, and means nothing
outside `[...]` or a `loop` header. A slice may drop a bound: `xs[a..]` runs to
the end (`queue = queue[1..];`), `xs[..b]` starts at 0 (`Text` too).

**Arrays are value-copied at the call boundary; records are not.** Passing
`xs` into a function gives that function its own copy -- mutating it there
(`arr[0] = 9;`) never changes the caller's array. If you want a function to
transform an array, return the new array and reassign at the call site:

```aether
fn doubleAll(arr: Int[]) -> Int[] {
    loop i in 0..length(arr) { arr[i] = arr[i] * 2; }
    ret arr;                    // <-- required; the caller never sees in-place writes otherwise
}
xs = doubleAll(xs);
```

A `Void` function that writes into an array parameter and never returns it
gets an `[ARR-001]` warning (compiles fine, but almost certainly not what you
meant) -- this is exactly the array/record asymmetry above: the identical
pattern on a record parameter works, because records are pointer-backed.

A loop variable is scoped to its loop, so a later loop may reuse the name; it
may not reuse the name of a variable already in scope (NAME-001).

`println`/`print` cannot print a whole array -- `println("data: ", xs)` is a
`TYPE-001` error. Loop over the array and print each element yourself:

```aether
fx {
    loop i in 0..length(xs) {
        print(xs[i], " ");
    }
    println("");
}
```

## TOON rules

Helpers (complete surface): `has_toon()`, `toon_parse(text)`,
`toon_parse_file(path)`, `toon_root(doc)`, `toon_close(doc)`,
`toon_key(node, key)`, `toon_key_or(node, key, toon_null())`,
`toon_at(node, i)`, `toon_len(node)`,
`toon_get_text(node, key)`, `toon_get_int(node, key)`,
`toon_get_real(node, key)`, `toon_get_bool(node, key)`, plus the
`toon_get_*_or(node, key, fallback)` variants (`toon_get_text_or`,
`toon_get_int_or`, `toon_get_real_or`, `toon_get_bool_or`: the fallback when
the key is missing, null or of another type);
`toon_text_value(node)`, `toon_int_value(node)`, `toon_real_value(node)`,
`toon_bool_value(node)`, `toon_null_value(node)`; `toon_type(node)`,
`toon_has_key(node, key)`, `toon_has_at(node, i)`;
`toon_is_text(node)`, `toon_is_int(node)`, `toon_is_real(node)`,
`toon_is_bool(node)`, `toon_is_null(node)`, `toon_is_arr(node)`,
`toon_is_obj(node)`.

Node-value accessors keep the `_value` suffix (`toon_int_value`); field
getters do not (`toon_get_int`). There is no bare `toon_int`/`toon_text`.

Keys are `Text`, indexes `Int`. Always `toon_close(doc)`.

Handle ownership:

- `toon_parse_file(path)` is **effectful** (file I/O — call inside `fx`);
  `toon_parse(text)` and the node ops below are pure (call outside `fx`)
- the text parser is `toon_parse(text)`, not `toon_parse_string`
- `toon_close(doc)` releases the document and every `ToonNode` made from it
- in large loops, `toon_free(node)` releases a temporary node early

Root shape (ROOT-001):

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

Never do this:

```aether
let doc: ToonDoc;
fx { doc = toon_parse_file("payload.json"); }
let name: Text = toon_get_text(doc, "name");
```

Always do this:

```aether
let doc: ToonDoc;
fx { doc = toon_parse_file("payload.json"); }
let root: ToonNode = toon_root(doc);
let name: Text = toon_get_text(root, "name");
```

Large-loop pattern:

```aether
let doc: ToonDoc;
fx { doc = toon_parse_file(path); }
let root: ToonNode = toon_root(doc);
let jobs: ToonNode = toon_key(root, "jobs");

loop i in 0..toon_len(jobs) {
    let job: ToonNode = toon_at(jobs, i);
    let name: ToonNode = toon_key(job, "name");

    fx {
        println(toon_text_value(name));
    }

    toon_free(name);
    toon_free(job);
}

toon_free(jobs);
toon_close(doc);
```

Key fidelity (KEY-001): copy JSON keys exactly; never flatten nested objects
into guessed keys (`"appName"`). A dotted key walks nested
*objects* (never an array index) in `toon_key`, `toon_has_key` and every getter:

```aether
let port: Int = toon_get_int_or(root, "server.port", 0);
```

Nested lookups (NEST-001): a missing intermediate degrades (fallback, `false`,
zero value, `toon_len` 0), never crashes, so a dotted key or
`toon_key_or(node, key, toon_null())` makes a path of any depth total with no
`if`:

```aether
let gb: Int = toon_get_int_or(row, "spec.mem.gb", 0);
let spec: ToonNode = toon_key_or(row, "spec", toon_null());
let gbAgain: Int = toon_get_int_or(toon_key_or(spec, "mem", toon_null()), "gb", 0);
```

An `if toon_has_key(row, "meta") { ... }` guard is also correct, one nesting
level per segment; use it when the output must say which level was missing.
(`toon_has_at` raises on a null node; test `i < toon_len(n)`.)

Never generate foreign JSON/object APIs such as `JsonDoc`, `JsonNode`,
`json.parseFile(...)`, `root.get(...)`, `Int.MIN`, or `value.toString()`.

## Concurrency: `par`

Run your own functions concurrently with `par { ... }`. The body holds direct
calls only (no assignments, loops, or inline `fx`); the calls run in parallel
and the block joins before continuing. Return results through pointer-backed
records passed as arguments — one record **per branch** (a record shared by two
branches races and is rejected, PAR-001).

```aether
par {
    tally(a, 100);   // a, b are separate records; each callee writes its own
    tally(b, 200);
}
fx { println("a=", a.count, " b=", b.count); }
```

## Imports (IMP-001, MOD-001)

Imports are advanced; most generated Aether should be single-file.

- only `use "..."` for real, verified modules; a missing import's symbols do
  not exist even if the line is silently ignored
- **MOD-001**: imported names match exported names exactly — `use` does not
  rename. Module exports `labelScore` → call `labelScore(...)`,
  never a guessed `label(...)`. Module exports `BaseScore` → use `BaseScore`,
  not `BASE_SCORE` or another invented spelling.
- file naming: `mod ModuleConsts` → `use "module_consts";`
- when combining purity with module export, write `@pure` above `export fn`
- for generated code, assume modules export `const` and `fn`; do not generate
  exported `type` blocks

```aether
use "score_utils";
let score: Int = clampScore(141);
let status: Text = labelScore(score);
```

Minimal import recipe:

```aether
use "geometry";

fn main() -> Void {
    let value: Int = area(3, 4);
    fx {
        println(value);
    }
    ret;
}
```

## Contracts (ANN-001)

```aether
@pre score >= 0
@post result >= 0
fn normalize(score: Int) -> Int {
    ret score;
}
```

Bad: bare `@pre` / `@post` with no expression; annotations inside the body.
`@post` may reference `result`. `@cost 5ms` units: `ns us ms s op ops step
steps`; syntax-checked but non-binding (no runtime enforcement).

On a collection return (`-> T[]`) a contract must compare a **property** of the
collection, not the collection itself: use `@post length(result) > 0`, never
`@post result > 0` (an array is not comparable to a scalar → ANN-001). Same for
`@pre` over an array parameter: `@pre length(xs) > 0`; every element:
`@pre all x in xs: x >= 0`.

## Copyable templates

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

```aether
fn main() -> Void {
    if !has_toon() {
        fx { println("structured data support unavailable"); }
        ret;
    }

    let doc: ToonDoc = toon_parse("{\"name\":\"Aether\"}");
    let root: ToonNode = toon_root(doc);
    let name: Text = toon_get_text(root, "name");

    fx {
        println("name = ", name);
    }

    toon_close(doc);
    ret;
}
```

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

Large-report recipe:

- parse file
- `let root: ToonNode = toon_root(doc);`
- `let items: ToonNode = toon_key(root, "...");`
- one pure normalize helper
- one pure classify helper
- one mutable totals type
- one loop that extracts, classifies, updates totals, prints
- one final totals block
- `toon_close(doc);`

## Repair rules

The compiler prints a stable code in brackets. Read the code, then apply the
fix:

- **[FX-001]** an output, task, or `ai_chat` call outside an effect block → wrap
  it in `fx { ... }`.
- **[SYN-001]** non-Aether syntax → `ret` not `return`, `type` not `class`;
  drop `var`, `def`, `=>` (the message names the Aether form). Also a **field
  or method named after an Aether keyword or operator word** (`new`, `for`,
  `loop`, `div`, ...) → rename the member (see Records: `type`).
- **[SCOPE-001]** a name/scope problem — the catch-all. It is one of:
  - a helper not listed in this document → not available here; inline the logic (BUILT-001)
  - an export called by a guessed name → use the exact exported name (MOD-001)
  - a method reaching an outer local → pass it in as a parameter (METH-001)
  - `method '<m>' is not defined on type '<T>'` → define `fn <m>(...)` inside
    `type <T> { ... }`, or fix the call name (methods do not capture; METH-001)
  - a genuinely undeclared / out-of-scope name → declare it earlier or pass it in (SCOPE-001)
- **[NAME-001]** a name declared twice (names ignore case; a loop variable,
  local or parameter may not reuse a visible variable or `const`) → pick a
  fresh name, or assign without `let`.
- **[IMP-001]** a malformed `use` → write `use "module_name";` and call the
  exports directly (MOD-002).
- **[TYPE-001]** a type cannot be inferred → annotate it (including an untyped
  array literal, `let xs: Int[] = [...]`). Also `println` of a whole array,
  `int()` of a `Text`, and a `Real` where an `Int` is needed (`%`, `div`).
- **[TYPE-002]** a type name that does not exist, in any position (declaration
  order does not matter) → use `Int`, `Real`, `Text`, `Bool`, `Void`, `T[]`, or
  a `type` you declared. The message names the Aether
  spelling when there is one (`Double` → `Real`, `Integer` → `Int`, `Char` →
  `Text`, `int` → `Int`).
- **[TOON-001]** a `ToonDoc`/`ToonNode` handle misuse → e.g. add
  `let root: ToonNode = toon_root(doc);`.
- **[MS-001]** an `MStream` handle misuse → declare stream bindings `MStream`
  (rule 25).
- **[FIELD-002]** `Unknown field` → use the exact declared field name.
- **[FIELD-003]** a field default that is not a literal (an expression, a named
  `const`, another field, `self`, a call), or a populated array default → use a
  literal (`count: Int = 0`), or set the value at construction with
  `new T { field: value }`. A type-mismatched default (`value: Int = "x"`)
  is `[TYPE-001]` instead — match the field's type.
- **[FLOW-001]** a fallthrough path with no return value → add a final `ret ...`
  on every reachable top-level path.
- **[FLOW-002]** an empty `ret;` in a non-`Void` function → give the return a
  value (`ret <expr>;`), or declare the function `-> Void`.
- **[ANN-001]** a misplaced annotation, or a `@pure` function calling an effect →
  move `@pre`/`@post`/`@pure`/`@cost` directly above the function, keep effects out of pure code, and mark its helpers `@pure`.
- **[TUP-001]** tuple misuse → the forms in rule 11; otherwise return a record
  and read its fields.
- **[NARROW-001]** (warning) a Real value stored in an `Int` target (`let n: Int
  = 3.7;`, `n = random();`) → the fraction is discarded. Use a `Real` target, or
  `int(...)` if truncating is intended.
- **[ARR-002]** a one-dimensional array indexed twice (`let dp: Int[] = [];`
  then `dp[i][j]`) → nested arrays are real: declare `Int[][]` and build each row
  as its own `Int[]`, or index once with `dp[r * width + c]`.
- **[PREC-001]** (warning) `flags & mask != 0` → write `(flags & mask) != 0`.
  As an error: `0 <= i < n` → `0 <= i && i < n`.
- **`Length expects a string or array argument`** / **`... expects a single
  value handle`** (runtime, no code) → `toon_len` and `length` are crossed
  (LEN-001).
- **[ARR-003]** (runtime) an index outside the array → the message gives the
  valid range; `loop i in 0..n` ends at `n - 1`, and a row being built is not in
  the table until it is appended.
- **[CON-001]** (runtime) `Aether @pre failed in f` (or `@post`) → a call broke
  `f`'s contract: `@pre`, a bad argument; `@post`, a wrong `result`.
- **[FMT-001]** a placeholder (`"{}"`, `%d`, `f"..."`) in `println` → pass
  values as arguments: `println("n = ", n)`.
- **[DIV-002]** `x // 2` → `//` starts a comment; integer division is `x div 2`.
- **[ENTRY-001]** top-level statements beside `fn main`, or a `main` with
  parameters → put the statements in `fn main() -> Void`.
- **[PAR-001]** the same record passed to more than one `par` branch (concurrent
  writes race) → give each branch its own record and combine after the block.
- **[PAR-002]** a non-call statement inside `par { ... }` → wrap the work in a
  `fn` and call that inside `par`.

If the program *compiles* but the output is wrong — extra headings, wrong
spacing or precision (an integer where decimals are expected → a `Real`
target and a `Real` operand), wrong JSON keys, a fallback that hid a missing level, or
iterating an object root — no code is printed: these are authoring rules the
compiler cannot check (FMT-001, OUT-001, KEY-001, NEST-001, ROOT-001,
FIELD-001). Re-read the prompt and match it exactly.

## Validation checklist

- all output, task, and `ai_chat` calls inside `fx { ... }` (FX-001)
- every called helper is named in this document, not guessed (BUILT-001)
- imports verified; exported names used exactly (IMP-001, MOD-001)
- all parameters typed; uncertain types annotated; array literals typed
  (`let xs: Int[] = [...]`) (TYPE-001)
- `ret` not `return`; `type` not `class` (SYN-001)
- field & method names are not Aether keywords or operator words (`new`/`for`/
  `loop`/`div`); no `fn new()`/`__init__` constructor method (SYN-001)
- no arithmetic on / cross-assignment of TOON handles; docs closed (TOON-001)
- stream bindings declared `MStream`; bodies read via `mstream_text` (MS-001)
- object roots: named array extracted (ROOT-001); keys copied exactly
  (KEY-001); a missing intermediate handled deliberately (NEST-001)
- `toon_len` vs `length` used correctly (LEN-001)
- `Real` operand where decimals matter; `value:width:precision` for stable output
- tuples destructured directly, or bound and read with `.0`/`.1`/`.2`, never
  chained onto the call (TUP-001); annotations above functions (ANN-001)

---

## Appendix: host capabilities

Reach for these only when the task requires them.

### Files and environment

Host access for automation. **All effectful — call inside `fx`** (FX-001), and
banned in `@pure` functions:

- `read_file(path) -> Text`, `read_lines(path) -> Text[]` (no line breaks),
  `write_file(path, text)`, `append_file(path, text)` (end lines with `"\n"`)
- `file_exists(path) -> Bool`, `delete_file(path) -> Bool`,
  `rename_file(a, b) -> Bool`, `getcurrentdir() -> Text`, `mkdir(path)`
- `read_line() -> Text` (stdin, `""` at the end), `read_all_stdin() -> Text`
- `args() -> Text[]`, `getenv(name) -> Text`, `getenvint(name, fallback) -> Int`
- clock: `clock_ms() -> Int` (milliseconds since 1970), `now_text() -> Text`

```aether
fx {
    write_file("notes.txt", "first line\n");
    loop line in read_lines("notes.txt") { println(line); }
}
```

Sockets (`socket*`), `sqlite*`, `random`/`randomize`, and the clock are also
effectful.

### HTTP requests (MS-001)

The `http*` calls are effectful (FX-001). The response body lands in an
`MStream` out-buffer:

- `httpsession() -> Int`, `httpclose(session)`,
  `httpsetheader(session, name, value)`
- `httprequest(session, method, url, body, out) -> Int` (HTTP status; `body`
  is the request payload, `""` for GET; `out` is an initialized `MStream`)
- `mstream_new() -> MStream`, `mstream_text(ms) -> Text`,
  `mstream_free(ms)` (all pure, no `fx`)

```aether
fx {
    let session: Int = httpsession();
    let out: MStream = mstream_new();
    let status: Int = httprequest(session, "GET", url, "", out);
    if status == 200 { println(mstream_text(out)); }
    mstream_free(out);
    httpclose(session);
}
```

`mstream_text(out)` reads the body — never `print(out)`, never
`let stream: Int = ...`. JSON responses pair with TOON:
`toon_parse(mstream_text(out))`.

### Sockets

Real BSD-socket-style API (`socket*`) and `dnslookup` — not `tcpsocket*`/
`udpsocket*`/`resolve`, which do not exist. All effectful (FX-001).
`socketreceive` returns an `MStream` (MS-001), never `Text`/`Int` directly.

- `socketcreate(type[, family]) -> Int`: `type` `0`=TCP, `1`=UDP; `family`
  `4`=IPv4 (default), `6`=IPv6
- `socketconnect(socket, host, port) -> Int`,
  `socketbind(socket, port) -> Int` (all interfaces),
  `socketbindaddr(socket, host, port) -> Int` (one address),
  `socketlisten(socket, backlog) -> Int` (all `0`/`-1`);
  `socketclose(socket)` returns nothing
- `socketaccept(socket) -> Int` (new socket handle) **blocks** until a peer
  connects; `socketreceive(socket, maxlen) -> MStream` **blocks** until data
  arrives (`0`-length result = peer closed, not failure)
- `socketsend(socket, data) -> Int` (`data`: `Text` or `MStream`, bytes sent)
- `socketpeeraddr(socket) -> Text`, `socketlasterror() -> Int`
- `socketsetblocking(socket, blocking: Bool)`,
  `socketpoll(socket, timeoutMs, flags) -> Int` (`flags` `1`=readable,
  `2`=writable, bitwise-or; `0` on timeout) — the non-blocking alternative to
  a bare `socketaccept`/`socketreceive`
- `dnslookup(hostname) -> Text` (forward only; `""` on failure)

`socketaccept`/`socketreceive` hang forever with no peer. Either run a
same-process client+server pair via `par` (create/bind/listen *before* the
`par` block so the client can't race a not-yet-listening server; `par`
joins), or use `socketsetblocking(s, false)` + poll `socketpoll` in a loop.

```aether
type Message {
    payload: Text;
}
fn server(listenSock: Int, out: Message) -> Void {
    fx {
        let conn: Int = socketaccept(listenSock);
        let ms: MStream = socketreceive(conn, 256);
        out.payload = mstream_text(ms);
        socketsend(conn, "pong");
        mstream_free(ms);
        socketclose(conn);
        socketclose(listenSock);
    }
    ret;
}
fn client(port: Int, out: Message) -> Void {
    fx {
        let sock: Int = socketcreate(0);
        socketconnect(sock, "127.0.0.1", port);
        socketsend(sock, "ping");
        let ms: MStream = socketreceive(sock, 256);
        out.payload = mstream_text(ms);
        mstream_free(ms);
        socketclose(sock);
    }
    ret;
}
fn main() -> Void {
    let port: Int = 23557;
    let listenSock: Int = 0;
    let serverMsg: Message = new Message();
    let clientMsg: Message = new Message();
    fx {
        listenSock = socketcreate(0);
        socketbind(listenSock, port);
        socketlisten(listenSock, 1);
    }
    par {
        server(listenSock, serverMsg);
        client(port, clientMsg);
    }
    ret;
}
```

### Tasks and AI

`sleep(ms: Int) -> Void`,
`ai_chat(model: Text, messages: Text, system: Text = "", apiKey: Text = "", endpoint: Text = "") -> Text`,
probes `has_ai() -> Bool`, `has_builtin(category: Text, function: Text) -> Bool`
(true for extended *and* core builtins; a core builtin matches on the function
name whatever category you pass).
`sleep` and `ai_chat` are effectful and must stay inside `fx`; the `has_*`
probes are pure. `sleep(ms)` is a blocking millisecond pause. To run your own
code concurrently use `par`.

Discovery (for builtins beyond this guide — deeper I/O, networking, DB, and
host-registered **custom** builtins). Agentic only: these are run-time calls.
- `builtins_json()` -> JSON list of all Aether-visible builtins
- `builtins_json(true)` -> richer metadata (`signature`, `effectful`, ...)
- `builtin_info(name)` -> metadata for one builtin
- `aether --dump-ext-builtins` -> registered custom/extended builtins

Discover the exact name, signature, and `effectful` flag, then call it (inside
`fx` if effectful). Writing in one shot? Stay within this guide; never guess
an unlisted name or its signature.
