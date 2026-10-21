# Aether for LLMs — Full Guide (for large contexts)

*Guide version: 2026-10-21-4*

If you only read one part of this document, read **Highest-Value Rules** and
**Never Generate These**.

## Rule labels

- **Hard rule**: violating this usually produces invalid Aether or wrong behavior.
- **Canonical**: preferred style for new Aether, especially LLM-generated Aether.
- **Accepted**: may compile, but generate only when preserving existing code.
- **Avoid**: likely wrong, fragile, or misleading even if it sometimes works.
- **LLM-critical**: high-value rule, mirrored in the medium guide and folded
  into rules in the small one.

## Highest-Value Rules

**LLM-critical.** These determine whether generated Aether works on the first try.

1. **FX-001.** Every effectful builtin must be inside `fx { ... }`: output
   (`print`/`println`), `ai_chat`, task/thread helpers, and all host
   interaction — files (`read_file`, `write_file`, `file_exists`, `mkdir`),
   stdin (`read_line`, `read_all_stdin`), environment (`get_env`), CLI
   (`args`), `random`/`randomize`, the clock (`clock_ms`, `now_text`),
   console input (`readkey`), networking (`http_*`/`socket_*`), and database
   (`sqlite_*`). Pure builtins (math, Text, arrays, conversion) do not need `fx`.
2. **SYN-001.** Use Aether keywords: `fn`, `let`, `const`, `ret`, `if`, `loop`,
   `type`, `mod`, `use`. Do not import syntax from Python, Rust, JavaScript,
   or Go (`while`/`for` are the sole Pascal-heritage exception — Accepted,
   not canonical; see the quick-reference table).
3. **TYPE-001.** Prefer explicit types when inference is not obviously safe.
4. **MUT-001.** `let` bindings are already mutable. Generate plain `let`;
   never `let mut` in new code.
5. **ANN-001.** Put `@pure`, `@pre`, `@post`, `@cost` directly above the
   function they decorate, never inside the body.
6. **TOON-001.** `ToonDoc` and `ToonNode` are opaque handle types, not
   integers or records. Never mix them or do arithmetic on them.
7. **IMP-001.** Use verified modules only. Never invent imports such as
   `use "helpers";`.
8. **LEN-001.** `toon_len(node)` for TOON arrays; `length(arrayValue)` for
   dynamic arrays and `Text`. Crossing them is not caught at compile time: the
   program stops at run time with an uncoded error.
9. **TUP-001.** Tuples are narrow: destructure direct top-level helper
   returns (`let (a, b) = pair();`), or bind to one variable and read a
   single field with `.0`/`.1`/`.2` (`let t = pair(); t.0;`) -- but not
   `pair().0` directly. A tuple-return fn may also `ret` a call to a fn
   returning the same tuple type (`ret parseNumber(s, pos);`) or a variable
   holding one. Assume nothing else. Recursive tuple-returning
   functions (direct or indirect) and concurrent `par`-branch calls to the
   same tuple-returning function are fully supported (tuple returns lower
   to a record returned by value, reentrant per call).
10. **OUT-001.** When asked to generate code, return raw Aether source only.
    No Markdown fences.
11. **BUILT-001.** The builtins listed here are the supported, recommended
    surface. Do not invent helpers (`substring`, `to_upper`, `strip`, ...) and
    do not guess names; method spellings (`s.upper()`, `xs.sort()`) are
    SCOPE-001 — call the function (`upper(s)`, `sorted(xs)`). **The complete inventory is in the appendix at the end
    of this document** — every builtin this compiler exposes, so "if it is not
    listed, it does not exist" is something you can check rather than take on
    faith. `builtins_json()` / `builtin_info(...)` also exist, but those are
    *runtime* builtins: if you are generating a source file in one shot you
    cannot call them, and the appendix is there precisely so you do not need to.
12. **BUILT-002.** Call a listed builtin with the argument count shown in its
    signature. Getting the name right and the arity wrong is a distinct error
    (`[BUILT-002] 'format_float' takes 1 to 2 arguments, but 3 were given.`) and
    is now caught at compile time for the fixed-arity conversion and math
    helpers. The classic case is splicing the width slot out of the
    `println`-only `r:0:prec` form into `format_float`, which takes
    `(value, precision)` and nothing more.
13. **ROOT-001.** `toon_root(doc)` returns the top-level value. If the JSON is
    object-shaped, extract the named array with `toon_key(...)` before
    iterating; never iterate an object root as if it were the array.
14. **SCOPE-001.** A name must be declared before use and still be in scope at
    the use site. Do not rely on guessed globals, expired loop locals, or
    helper-local names from another function.
15. **METH-001.** Methods do not capture outer locals. If a method needs a loop
    index, label, or other caller context, pass it as a parameter.
16. **FIELD-001.** Method locals may reuse field names. Bare `name` means the
    local; `self.name` means the field.
17. **FIELD-002.** Record and type field names must exist exactly as declared.
    Do not invent fields on a type.
18. **FIELD-003.** A record/type field default must be a **literal**:
    `count: Int = 0`, `name: Text = ""`, `on: Bool = true`, `xs: Int[] = []`.
    Not an expression, not another field, not `self`, not a call — and **not a
    named `const` either**. `limit: Int = MAX_SCORE;` is rejected exactly like
    `limit: Int = MAX_SCORE + 1;`, so a sentinel built from a constant must be
    set at construction instead: `new T { limit: MAX_SCORE + 1 }`.
19. **FLOW-001.** Every non-`Void` function must return a value on every
    reachable top-level path.
20. **FUNC-001.** Functions are not values. Aether has no anonymous functions,
    no inline `fn(...) -> T { ... }` literals, no lambdas, and no closures.
    Never pass a function as an argument. `task_spawn` / `task_queue` take a
    builtin *name* as `Text` (`task_spawn("delay", "worker", 5)`), not a
    function. There is no `map` / `filter` / `reduce`; transform with a `loop`.
    To run your own code concurrently, call your functions inside a
    `par { ... }` block (see **Concurrency**), not `task_spawn`. A `fn` inside
    a free function's body is a nested function (**Nested functions**): it
    reads and assigns that function's parameters and earlier locals, and is
    not a value either.
21. **PAR-001.** Each `par` branch must own the record it writes. Passing the
    same record to two branches races — the concurrent writes corrupt the heap,
    so it is rejected at compile time. Give each branch its own record and
    combine the results after the block. The same holds for an enclosing local
    that a nested fn assigns: only one branch may use it.
22. **MS-001.** `MStream` is the opaque memory-stream handle type.
    `mstream_new()` / `mstream_from_text(text)` return `MStream` — never
    declare the binding `Int` or `Text`. Extract contents with
    `mstream_text(ms) -> Text`; release with `mstream_free(ms)`. HTTP responses
    arrive in an `MStream` out-buffer (see **HTTP requests**).
23. **NAME-001.** Names are case-sensitive -- write each one exactly as it was
    declared (`total` for a `let total`; `Total` is SCOPE-001) -- and two names
    may not differ only by case. One name means one thing: do not redeclare a
    local or a parameter, name a loop variable after a variable already in
    scope, or give a local or parameter the name of a top-level or imported
    `const` (`limit` beside `LIMIT`).
24. **MOD-001 / MOD-002.** The canonical import is `use "module_name";`, and
    imported names are used exactly as exported — `use` does not rename.
25. **FMT-001.** `println` has no placeholders: `println("{} items", n)` and
    `f"..."` are compile errors, so pass the values as arguments. When the
    prompt specifies exact output, match it exactly — spacing, casing, line
    order, decimal precision.

Fast failure checks: output outside `fx` is wrong; a guessed import is wrong;
a helper not listed in this document is wrong; arithmetic on `ToonDoc` /
`ToonNode` / `MStream` is wrong; `return` is wrong; contracts inside a body are wrong;
when unsure about a type, add it explicitly.

## Never Generate These

**LLM-critical.** Common LLM failure modes. Never generate:

- `return`; use `ret` (SYN-001)
- `class`; use `type` (SYN-001)
- `var`, `func`, `def`, `=>`, Python-style colons (SYN-001)
- `for`/`while` as your first choice of loop keyword — they compile (Pascal
  heritage; **Accepted**, see the quick-reference table below), but `loop` is
  canonical and covers every form (condition, range, infinite) they do
- a field or method named after an Aether keyword (`new`, `for`, `if`, `loop`,
  `ret`, `type`) or an operator word (`div`, `mod`, `xor`, `and`, `or`, `not`);
  rename the member (SYN-001). Other languages' words — `word`, `text`, `int`,
  `match`, `class`, `join`, `mul` — are ordinary names
- `fn new()` / `fn __init__` as a constructor method; Aether has none —
  construct with `new T { field: value }` (partial sets ok; unset fields keep
  their declared defaults), or `new T()` then assign fields, or a top-level
  factory `fn` (SYN-001)
- an untyped array literal: `let xs = [1, 2, 3];` (cannot infer) — write
  `let xs: Int[] = [1, 2, 3];` (TYPE-001)
- `print` / `println` / task helpers / `ai_chat` outside `fx { ... }` (FX-001)
- invented imports such as `use "helpers";` (IMP-001)
- `use module_name;` or `export { ... }` in new code; use canonical
  `use "module_name";` and `mod Name { export ... }`
- invented helper functions not listed in this document (BUILT-001)
- `toon_parse_string(text)` — the parser is `toon_parse(text)`. The `_file`
  suffix on `toon_parse_file(path)` does **not** imply a matching `_string`
  sibling; `toon_parse` already takes `Text`. (A compatibility alias quietly
  rewrites it, so it runs — write the canonical name anyway)
- `format_float(r, 0, prec)` — `format_float` takes one or two arguments,
  `format_float(r, prec)`. The width slot belongs to the separate, `println`-only
  `r:width:prec` form. This is a compile-time `[BUILT-002]`
- anonymous functions, inline `fn(...) -> T { ... }` literals, lambdas, or
  passing a function as a value; `task_spawn` / `task_queue` take a builtin
  *name* as `Text`, and there is no `map` / `filter` / `reduce` (FUNC-001)
- another language's map spellings: `dict()`, `Dict[str, int]`, `HashMap<..>`,
  `Set[T]`, `m.get(k)`, `m.keys()`, `m.items()`, `m.pop(k)`. The map type is
  `Map[Text, T]` and its operations are functions: `get(m, k, d)`, `keys(m)`,
  `remove(m, k)`, `loop k in m` (SCOPE-001, TYPE-002) — see **Maps**
- names that were never declared, or locals reused outside their scope
  (SCOPE-001)
- arithmetic on `ToonDoc` or `ToonNode`, or assigning one where the other is
  expected (TOON-001)
- `let stream: Int = mstream_from_text(...)` — memory streams are `MStream`
  handles, not integers (MS-001)
- `copy`, `pos`, `stringofchar`, `val`/`valreal`, `itoa`/`inttostr`, the `File`
  type with `assign`/`reset`/`rewrite`/`append`/`readln`/`writeln(f, ...)`/
  `eof`/`close`/`erase`/`rename`, `paramcount`/`paramstr`, `gettime`/`getdate`,
  `setlength`, the unprefixed `mstream*` names, `mstream_append_text`/
  `mstream_append_byte` (write `mstream_write_text`/`mstream_write_byte`), and
  run-together builtin names (`formatfloat`, `httprequest`: every builtin is
  snake_case, `format_float`, `http_request`): none of them is Aether. Each
  is SCOPE-001 (`File` is TYPE-002) with a hint naming the form to write — see
  **Removed spellings** under Repair rules
- annotations inside a function body, except directly above a nested `fn`
  (ANN-001)
- a tuple index chained directly onto a call: `pair().0` — bind it first,
  `let t = pair(); t.0;` (TUP-001)
- a `fn` nested two deep or inside a method (FUNC-001)
- an implicit `Real` → `Int` narrowing: `let n: Int = 3.7;`, `n = random();`,
  `let n: Int = sqrt(2.0);`. The fractional part is discarded silently — this is
  a `[NARROW-001]` **warning**, so it still compiles. Write `int(...)` when the
  truncation is intended (NARROW-001)
- foreign object/JSON APIs such as `JsonDoc`, `JsonNode`, `json.parseFile(...)`,
  `root.get(...)`, `Int.MIN`, and `value.toString()`
- Pascal-style `write(...)` / `writeln(...)` in new code; prefer
  `print(...)` / `println(...)`
- Markdown fences around the program

## Canonical vs accepted forms

Generate the canonical form unless preserving existing code.

| Topic | Canonical | Accepted | Avoid |
|---|---|---|---|
| Mutable binding | `let x: Int = 0;` | `let mut x: Int = 0;` | treating `let` as immutable |
| Loop keyword | `loop` (condition / range / foreach / infinite) | `while cond { }`, `for x in a..b { }`, `for x in xs { }` | inventing other loop syntax |
| Logical operators | `&&`, `\|\|`, `!` | `and`, `or`, `not` (exact synonyms) | — |
| Return | `ret value;` (`ret;` for Void) | — | `return value;` |
| Output | variadic `println(a, b)` in `fx` | `+` concatenation | — |
| Text equality | `a == b` | `string_eq(a, b)` | inventing `.equals(...)` |
| Text length | `string_len(t)` | `t.len` | inventing `strlen(...)` |
| Text → Int | `parse_int(t)` | — | inventing `Int(t)` / `t.toInt()` |
| Text → Real | `parse_float(t)` | — | inventing `Real(t)` |
| Text → Bool | `parse_bool(t)` | — | comparing to `"true"` by hand |
| Text → Int, untrusted input | `let (n, ok) = try_parse_int(t);`, then require `ok` | `parse_int(t)` behind an explicit digit scan | `parse_int(t)` alone — it cannot report failure |
| Text → Real, untrusted input | `let (r, ok) = try_parse_real(t);` | — | `parse_float(t)` alone — same reason |
| Int → Text | `int_to_text(n)` | — | `n.toString()`, `itoa(n)` (SCOPE-001) |
| Upper / lower case | `upper(s)`, `lower(s)` | `upcase(s)` (whole string) | `s.upper()`, `to_upper(s)` |
| Sort | `sorted(xs)`, `sorted_by(rs, "field")` | — | `xs.sort()`, a hand-written insertion sort |
| Real type name | `Real` | `Float` | `Double`, `Number` (TYPE-002) |
| Text type name | `Text` | `String` | `Str`, `Char` (TYPE-002) |
| Real → Text | `format_float(r, prec)` — 1 or 2 args | `real_to_text(r)` (shortest, as printed) | `format_float(r, 0, prec)` (a width argument is `[BUILT-002]`); `r:0:prec` as a value (it is `println`-only) |
| Numeric cast | `int(x)` (`Real` → `Int`, truncating) | `Int(x)`, `real(n)` / `Real(n)`, `bool(n)` / `Bool(n)` | `int(text)` (`TYPE-001`; use `parse_int`) |
| Char → Int code | `ord(ch)` | — | `int(ch)` (`TYPE-001`: it casts `Real`/`Bool`, not `Text`) |
| Int code → Char | `chr(code)` | — | inventing a lookup table |
| Split text | `split(t, sep)` → `Text[]` | — | manual character scanning |
| Dynamic array length | `length(xs)` | `len(xs)`, `xs.len` | `toon_len(xs)` |
| TOON array length | `toon_len(node)` | — | `length(node)` |
| Method call | `counter.bump()` | `bump(counter)` for an extension method only | class syntax |
| Concurrency target | `task_spawn("delay", "w", n)` (builtin name) | — | passing a closure / `fn(){...}` |
| Transform a list | explicit `loop` building `xs + [v]` | — | `map(...)` / `filter(...)` with a lambda |
| Record init | `new Point { x: 3, y: 4 }` | `Point { x: 3, y: 4 }`, `Point(x: 3, y: 4)` | `fn new()` constructor |
| TOON nested lookup | bind and validate intermediates | nested calls when shape guaranteed | a guessed flattened key |
| Imports | verified `use "module";` | self-contained code | guessed modules |
| Method context | pass loop/local context as params | helper computes its own label | implicit capture of outer `i` / `name` |
| Field/local same name | `let valid = ...; if valid { self.valid = ...; }` | distinct local names | assuming bare `valid` means the field |

## What Aether is for

Small-to-medium automation programs that parse structured payloads, extract
typed fields, classify or transform data, and print or store results inside
`fx` — with visible effect boundaries and lightweight contracts. It is not a
dynamic scripting language, and not a place to import imagined libraries.

## Smallest useful program

```aether
fn main() -> Void {
    fx {
        println("Hello from Aether");
    }
    ret;
}
```

A program runs `fn main() -> Void`, or `fn main() -> Int`, whose value becomes
the exit status. A file with both a `fn main` and top-level statements is
`[ENTRY-001]`, because `main` would never run: put the statements inside
`main`. To stop early with a status from anywhere, call `fx { halt(n); }`.

## Safe generation algorithm for LLMs

**LLM-critical.** Generate in this order:

1. Decide whether the program needs TOON.
2. Give every function parameter an explicit type.
3. Prefer explicit `let name: Type = ...` unless the initializer is trivial.
4. Put all output, task helpers, and AI calls inside `fx`.
5. Use `ret`, never `return`.
6. Close every parsed `ToonDoc` with `toon_close(doc)`.
7. Run the Validation Checklist before final output.

## Lexical basics

- **Comments**: `// line comment` is canonical. `/* block comment */` is
  accepted, but generated code should prefer `//`. `//` always starts a
  comment — it is never integer division: `x // 2` is `[DIV-002]`; write
  `x div 2`.
- **Text literals**: double-quoted. Escapes: `\"`, `\\`, `\n`, `\t`, `\r`,
  `\xNN` and `\uNNNN`; `\0` ends the string, and an unknown escape stays as
  written (`\q` prints `\q`).
- **Statements** end with `;`. Blocks use `{ }`.

## Primitive types

| Type | Literals | Notes |
|---|---|---|
| `Int` | `42`, `-1` | integer arithmetic |
| `Real` | `3.5` | `/` always yields a `Real`; `Float` is an accepted alias |
| `Text` | `"hi"` | string type; `String` is an accepted alias |
| `Bool` | `true`, `false` | `println(flag)` prints `true` or `false` |
| `Void` | — | return type for procedures |
| `ToonDoc`, `ToonNode` | — | opaque handles (TOON-001) |
| `MStream` | — | opaque memory-stream handle (MS-001) |
| `Map[Text, T]` | `{"k": v}`, `{}` | Text keys, values of type `T`; see **Maps** |

`Float` and `String` lower identically to `Real` and `Text`, so they are correct
if you write them; the canonical spellings are still `Real` and `Text`. Those two
are the *only* foreign type names accepted. Every other one — `Double`,
`Integer`, `Long`, `Boolean`, `List`, `Dict`, `Set`, Pascal's `Char`, `Byte`,
`Word` and `Str`, and lowercase spellings such as `int` or `string` — is a
`TYPE-002` error naming the Aether spelling or shape you wanted. A type name is checked
against the builtins above plus the `type` declarations in your program and its
imports, and nothing else: an unrecognised name is always an error, never an
untyped value that quietly accepts anything.

For mixed output, prefer variadic `println(...)` / `print(...)` rather than
guessing conversion helpers or `Text + Int` coercions.

## Operators

| Category | Operators |
|---|---|
| Arithmetic | `+ - * /` on `Int`/`Real`; `/` always yields a `Real`; `a div b` is the integer quotient |
| Modulo | `%` (example: `7 % 3` = `1`); the result takes the dividend's sign (`-7 % 3` = `-1`), so test odd with `n % 2 != 0` and wrap an index with `(i % n + n) % n` |
| Comparison | `== != < <= > >=` |
| Logical | `!` for negation; `&&` and `||` for conjunction/disjunction (short-circuit); the words `not`, `and`, `or` are exact synonyms |
| Bitwise/shift | `& \| ^`/`xor` and `<< >>` on `Int` (example: `6 & 3` = `2`, `6 << 1` = `12`) |
| Text | `+` concatenation (convert numbers with `int_to_text` / `format_float` first); `==` equality |
| Array | `xs + [v]` appends (any literal length); `xs + ys` concatenates two array-valued expressions; `xs == ys` / `xs != ys` compare element by element |

`&`/`|`/`^` are bitwise, not logical -- use `&&`/`||` for Bool conjunction/
disjunction. `xor` may be spelled as the word `xor` or the symbol `^`
(both lex to the same operator); there is no word form for `&`/`|`.
Precedence (loosest to tightest): `|| , && , | , ^ , & , == != , < <= > >= ,
<< >> , + - , * / %`. The word forms `or`, `and`, `not` sit exactly where their
symbols do.

Unary minus on numeric literals and expressions is supported.

Numeric literals may contain `_` digit separators for readability
(`1_000_000`, `0xFF_FF`, `3.141_592`); the separators are ignored. Hexadecimal
(`0x…`), decimal, and floating-point literals all accept them. A `_` counts only
between two digits, so a leading or trailing `_` is not part of the number.

## Functions

```text
fn name(arg: Type, ...) -> ReturnType { ... }
```

Hard rules:

- every function declares an explicit return type; procedures use `-> Void`
- every parameter has an explicit type
- use `ret value;`, or bare `ret;` in `Void` functions; never `return`

```aether
fn add(a: Int, b: Int) -> Int {
    ret a + b;
}
```

### Nested functions

A `fn` declared inside the body of a free top-level function is a nested
function. It reads and assigns the enclosing function's parameters and the
locals declared before it, calls itself and its sibling nested functions, and
is called by name after its declaration:

```aether
fn countQueens(n: Int) -> Int {
    let count = 0;
    let cols: Int[] = [];
    fn safe(col: Int) -> Bool {
        let row = length(cols);
        loop r in 0..row {
            if cols[r] == col || abs(cols[r] - col) == row - r { ret false; }
        }
        ret true;
    }
    fn place(row: Int) -> Void {
        if row == n {
            count = count + 1;          // changes countQueens's count
            ret;
        }
        loop col in 0..n {
            if safe(col) {
                cols = cols + [col];    // appends to countQueens's cols
                place(row + 1);
                cols = cols[..row];     // and takes it off again
            }
        }
    }
    place(0);
    ret count;
}

fn main() -> Void {
    fx { println(countQueens(6), " ", countQueens(8)); }
}
```

```text
4 92
```

- An assignment, an element write (`cols[r] = c;`) or an append
  (`xs = xs + [v];`) in a nested fn changes the enclosing function's variable
  itself, and every later read, in the nested fn or in the enclosing function,
  sees the new value. Each call of the enclosing function has its own locals,
  so a recursive enclosing function is fine.
- An array it only reads is passed without a copy, so recursion over a large
  array stays cheap.
- `@pure`, `@pre` and `@post` go directly above a nested fn as above any fn. A
  `@pure` nested fn may read the enclosing locals but not assign them (ANN-001).
- In `par`, a nested fn that assigns an enclosing local may run in one branch
  only: if another branch also uses that local, it is PAR-001.
- One level deep, free functions only: a `fn` inside a nested `fn`, or inside a
  method (methods do not capture, METH-001), is FUNC-001. One declared in an
  inner block is visible only in that block.
- It is not a value: storing, passing or returning it is FUNC-001.

### Narrow tuple returns (TUP-001)

```aether
fn pair() -> (Int, Int) {
    ret (1, 2);
}

fn main() -> Void {
    let (a, b) = pair();
    fx { println(a, " ", b); }
    ret;
}
```

Limits: top-level helper functions only; destructuring must be a direct call;
no tuple-return methods. Tuple returns lower to a record returned by value,
so recursion (direct or indirect, e.g. `a() -> b() -> a()`) and calling the
same tuple-returning function from more than one concurrent `par` branch are
both fully supported and reentrant.

To read a single element instead of destructuring the whole tuple, bind the
call to a variable and use `.0`, `.1`, `.2`, ... (zero-based positional
field access):

```aether
fn pairSquares(n: Int) -> (Int, Int) {
    ret (n * n, n * n * n);
}

fn main() -> Void {
    let t = pairSquares(6);
    fx { println(t.0, " ", t.1); }
    ret;
}
```

The binding may carry the tuple's type — `let t: (Int, Int) = pairSquares(6);`
— when it matches the called function; a mismatched annotation, and a tuple
type on a parameter, a field or an array element, are `TUP-001`.

A tuple-return function can hand back another one's result unchanged: `ret`
a call to a function returning the same tuple type, or a variable holding
one. Any other non-literal `ret` there is `TUP-001`.

```aether
fn parseDigit(s: Text, pos: Int) -> (Int, Int) {
    ret (ord(s[pos]) - ord("0"), pos + 1);
}

fn parseTerm(s: Text, pos: Int) -> (Int, Int) {
    ret parseDigit(s, pos);
}
```

An index at or past the tuple's arity is a compile-time `TUP-001` error
("tuple index .N is out of range"), not a runtime crash. `.N` only works on
a variable -- chaining an index directly onto a call result
(`pairSquares(6).0`, no intermediate `let`) is not supported yet and reports
`TUP-001` with a hint to bind the call first.

When the values do not come from a direct call to a defined top-level
tuple-return function (for example a method, an undefined helper, or a nested
expression), the compiler reports `TUP-001` ("tuple destructuring target is not
a known tuple-return function"). Return a record/object instead and read its
fields:

```aether
type Pair {
    first: Int;
    second: Int;
}

fn split_pair(s: Text) -> Pair {
    ret new Pair { first: 3, second: 7 };
}

fn main() -> Void {
    let r: Pair = split_pair("x");
    fx { println(r.first, " ", r.second); }
    ret;
}
```

`@post` is allowed on tuple-return helpers, but only with positional result
slots:

```aether
@post result.0 <= result.1
fn ordered(a: Int, b: Int) -> (Int, Int) {
    ret (a, b);
}
```

Use `result.0`, `result.1`, and so on inside tuple `@post` expressions. Do not
write bare `result` there.

## Variables and constants

```aether
const MAX_COUNT: Int = 42;
let count: Int = 0;
let label: Text = "Aether";
```

Names are case-sensitive: a reference must be spelled exactly as its
declaration (`count`, not `Count`, for `let count`; a mismatch is SCOPE-001
naming the declared spelling), and builtins are lowercase (`println`, never
`Println`). Two names may not differ only by case (`limit` beside `LIMIT` is
NAME-001), so give a constant and a local different words, not just different
case.

`let` bindings are mutable (MUT-001). `let mut` is accepted, redundant, and
ignored; never generate it.

### Inference policy (TYPE-001)

Omit the type only for these initializers:

- literals: `42`, `3.5`, `"text"`, `true`
- calls to functions with known declared return types, including methods on
  known typed bindings and the TOON/string helpers in this document

Everything else gets an explicit type — especially array literals
(`let xs = [1, 2, 3];` cannot be inferred at all — see **Constructing records
and typing bindings**), TOON extractions with non-trivial shape, branchy
results, and arithmetic such as `base + 1` where the operand types are not
visible at a glance. When in doubt, annotate.

Canonical loop-local example:

```aether
loop i in 1..4 {
    let cube: Int = i * i * i;
    fx {
        println(i, " cubed is ", cube);
    }
}
```

## Records: `type`

```aether
type JobSummary {
    name: Text;
    score: Int;

    fn isReady() -> Bool {
        ret self.score >= 90;
    }

    fn label() -> Text {
        if self.isReady() {            // a sibling method is called through self
            ret self.name + ": ready";
        }
        ret self.name + ": waiting";
    }
}
```

- use `type`, never `class`; fields are `name: Type;` (semicolons)
- a field may carry a **literal** default: `name: Type = <literal>`
  (`count: Int = 0`, `name: Text = ""`, `on: Bool = true`, `xs: Int[] = []`).
  The default must be a **literal** — not an expression, not another field, not
  `self`, not a call, and not a named `const` (`= MAX_SCORE` fails just as
  `= MAX_SCORE + 1` does) (FIELD-003). For anything else, set it at
  construction with `new T { field: value }`. A field with no default
  falls back to the type zero (integers `0`, reals `0.0`, booleans `false`,
  text empty).
- **define methods inside the `type` block; `self` is implicit — never give a
  method a `self` parameter.** Inside a method use lowercase `self`, never `Self`.
  Members are not implicit: write `self.field` and `self.method()`; a bare call
  to a sibling method is SCOPE-001.
- **only a method declared inside the `type` binds `self`, so only its
  `@pre`/`@post` may reference `self.field`** (e.g. `@pre self.v >= 0`). A
  `self`-referencing contract on a top-level `fn` fails `[SCOPE-001] identifier
  'self' not in scope` (contrast below).
- top-level helpers whose first parameter is `self: Type` are extension-style
  methods; call them with method syntax (`counter.bump()`) — but such a top-level
  `fn` cannot carry a `@pre`/`@post` that names `self`; put the method and its
  contract inside the `type` instead
- methods do not implicitly capture outer loop locals or helper locals from
  another scope; pass needed context in the parameter list
- if a local and a field share a name, bare `name` means the local and
  `self.name` means the field
- field names must exist exactly as declared on the type; do not invent
  `self.blockers` if the type has no `blockers` field
- a field may hold an array of records (`items: Item[] = [];`), grown with the
  usual append (`order.items = order.items + [new Item { qty: 3 }];`) and read
  back as `order.items[i].qty`
- **field and method names must not be Aether keywords.** A member named after
  a keyword (`new`, `for`, `if`, `loop`, `ret`, `type`) or an operator word
  (`div`, `mod`, `xor`, `and`, `or`, `not`) is rejected at parse time —
  `'<name>' is a reserved … and cannot be used as a field/method name`
  (SYN-001); rename it (`loop` → `loopCount`). Words that *other* languages
  reserve are ordinary names here: `word`, `text`, `int`, `byte`, `match`,
  `class`, `join`, `mul` all work as fields, methods and locals. There is **no
  constructor method**: do not write `fn new()` (nor `fn __init__` /
  `Type.new()`); allocate with `new T()` and assign fields, or add a top-level
  factory `fn makeT(...) -> T` with a non-reserved name.

A record method goes **inside** the `type` with an implicit `self`, and its
contract may then reference `self`. A free-standing `fn` with an explicit `self`
parameter plus a `self`-referencing contract does not compile:

```aether
// WRONG — free-standing method + explicit self → [SCOPE-001] 'self' not in scope
type C { v: Int; }
@pre self.v >= 0
fn get(self: C) -> Int { ret self.v; }
```

```aether
// RIGHT — method inside the type; self is implicit; the contract may name self
type C {
    v: Int;
    @pre self.v >= 0
    fn get() -> Int { ret self.v; }
}
```

Record values are pointer-backed: passing a record to a function or method
and mutating its fields is visible to the caller.

### Constructing records and typing bindings

There is **no constructor method** (SYN-001). Allocate with `new T()` — the only
constructor — and set fields at construction with the record-literal form
`new T { field: value, ... }` (the recommended way to initialize with values).
Do not write `fn new()`, `fn __init__`, or a static `T.new()`; all three are
rejected:

```aether
let point: Point = new Point { x: 3, y: 4 };  // set fields at construction
let counter: Counter = new Counter();         // fields take declared defaults / type zeroes
counter.value = 41;                           // or assign after allocation
```

The record-literal set is **partial**: any field you omit keeps its declared
default (FIELD-003) or, if it has none, the type zero. An explicit
`new T { field: value }` value always overrides that field's default.

`new Type()` defaults, in order of precedence: a field's declared constant
default (`count: Int = 0`), else the type zero — integers `0`, reals `0.0`,
booleans `false`, text empty, pointers `nil`, arrays empty-initialized. The
bare `T { ... }` and `Type(x: 3, y: 4)` forms are also accepted but
non-canonical; prefer `new T { ... }`. Generated code should not otherwise use
pointers or `nil`; they are backend details. Want a constructor with logic?
Write a top-level factory `fn` with a non-reserved name
(`fn makePoint(x: Int, y: Int) -> Point { ... }`).

**Always annotate a binding that holds an array literal** (TYPE-001):
`let xs = [1, 2, 3];` fails with `cannot infer the type of 'xs'`. Write
`let xs: Int[] = [1, 2, 3];`. `let c = new C();` infers `C`; writing
`let c: C = new C();` is a matter of style.

Record literals (`new T { ... }` or bare `T { ... }`) may appear directly
inside an array literal: `let ps: Point[] = [new Point { x: 1, y: 2 }, new
Point { x: 3, y: 4 }];`. Building the array with append-in-a-loop (`ps = ps +
[p];`) still works too and is a reasonable alternative when each element's
fields come from a computation rather than a literal.

## Conditionals

Parentheses are optional. The canonical multi-branch pattern is sequential
`if` + `ret`:

```aether
if score >= 90 {
    ret "ready";
}
if score >= 70 {
    ret "review";
}
ret "blocked";
```

Statement-level `else` is supported:

```aether
if score >= 90 {
    ret "ready";
} else {
    ret "blocked";
}
```

`else if` is also supported, but sequential `if` + `ret` remains the most
predictable style for generated classification helpers.

Do not write a non-`Void` helper with one branch that returns and another that
falls through to the closing brace. Add an explicit final `ret ...` on every
reachable top-level path.

Inline conditional expressions are allowed anywhere a value is expected —
declarations, assignments, returns, and call arguments including
`println(...)`. Chained `else if` works in expression position too, exactly
like the statement form above:

```aether
let label: Text = if ready { "ready" } else { "blocked" };
fx {
    println(label);
    println("status: ", if ready { "ready" } else { "blocked" });
}
let grade: Text = if score > 90 { "A" } else if score > 80 { "B" } else { "C" };
```

## Loops

Prefer the range and foreach forms. Both **declare their own loop variable** —
there is no separate `let` for `i` or `item`, and writing one is a
redeclaration. Only the condition form needs a counter declared ahead of it.
A range is half-open: `0..n` ends at `n - 1`, so 1 through `n` is `1..n + 1`
and -1, 0, 1 is `-1..2`.

```aether
loop i in 0..count {        // half-open range; i is Int, declared by the loop
    fx { println(i); }
}

loop i in 0..10 step 3 {    // stepped range: 0 3 6 9; `step -1` counts down
    fx { println(i); }
}

loop item in items {        // foreach: item takes the collection's element type
    total = total + item;
}

loop index < total {        // condition loop; `index` declared before this
    index = index + 1;
}

loop {                      // infinite
    break;
}
```

`loop item in items` iterates an array (`T[]`, giving each `T`), a `Text` (one
character at a time, as a one-character `Text`), or a TOON array node
(`ToonNode`, giving each element node). The collection may be a variable, a
field (`o.items`, `self.items` in a method, `rows[i].items`), a call with a
declared return type, or a slice. A tuple item (`t.0`) or a field of a type
from a `use`d module is `[TYPE-001] cannot infer the type of the collection`:
bind it to a typed local first (`let items: Int[] = t.0;`) and loop over that.

```aether
type Order {
    items: Int[] = [];
}

fn totalOf(o: Order) -> Int {
    let total: Int = 0;
    loop x in o.items {              // a field is a collection
        total = total + x;
    }
    ret total;
}
```

A row of a nested array
is bound by value, so writing to `item` never changes the collection. `step n`
takes any `Int` expression: a literal step is inlined and a negative one counts
down (`i > high`), any other step is evaluated once before the loop and its
sign checked at run time, and a literal `step 0` is rejected. `break` exits the
nearest loop; `continue` moves to the next element, advancing the counter or
applying the step. The loop variable is scoped to its loop, so reusing the name
in a later loop in the same function is fine.

### Quantifiers: `all` and `any`

`all NAME in COLLECTION: COND` and `any NAME in COLLECTION: COND` are `Bool`
expressions, usable anywhere a value is, contracts included. The header is a
`loop` header: an array, a `Text`, a TOON array, or a range `a..b` with an
optional `step`. `all` is true when COND holds for every element, `any` when it
holds for at least one. Both stop at the first element that decides, so an empty
collection gives `all` true and `any` false.

```aether
@pre all x in xs: x >= 0
fn total(xs: Int[]) -> Int {
    ret sum(xs);
}

fn main() -> Void {
    let xs: Int[] = [3, 8, 1];
    let ascending: Bool = all i in 0..length(xs) - 1: xs[i] <= xs[i + 1];
    fx {
        println(ascending, " ", any x in xs: x > 5, " ", total(xs));
        if any c in "hello": c == "l" { println("has l"); }
    }
}
```

```text
false true 12
has l
```

NAME is visible only in COND and follows the loop-variable rule (NAME-001). COND
extends to the end of the expression, so `all x in xs: x > 0 && x < 10` tests
both comparisons; parenthesize a quantifier inside a larger condition:
`(all x in xs: x > 0) && n > 0`. Calling `all(...)` or `any(...)` as a function
is SCOPE-001.

## Effects: `fx` (FX-001)

**LLM-critical.** All effects go inside `fx { ... }`: `print`, `println`, task
helpers, `ai_chat`, and every builtin that touches the outside world —
filesystem, environment, CLI, `random`, the clock, console input, networking,
and database. Compute pure values (math, string, conversion) outside `fx`, then
perform effects inside it. `@pure` functions may not contain `fx`, so they
cannot call any of these.

```aether
fn report(msg: Text) -> Void {
    fx {
        println("report: ", msg);
    }
    ret;
}
```

Wrong: `println("hi");` at function scope. Right: wrap it in `fx { ... }`.

Control flow (`if`, `loop`, blocks) nests inside `fx` with no restriction —
only the *builtin calls* are gated, not the surrounding structure:

```aether
fx {
    loop i in 0..count {
        println("item ", i, " of ", count);
    }
}
```

There's no need to hoist a loop out of `fx` and re-enter `fx` on every
iteration; put the whole loop inside one `fx` block when every iteration's
work is effectful.

## Printing and formatting

`print` / `println` are the Aether output builtins. For mixed types, use
variadic arguments; to build a `Text`, convert numbers with `int_to_text` /
`format_float` before `+`:

```aether
fx {
    println("Drop ", j, " -> ID: ", tx.id, " | Amt: ", tx.amount);
    println("pi ~= ", 3.14159:0:2);   // width:precision => 3.14
}
```

`println(realValue)` prints the shortest form that reads back as the same
value, as Python does: `2.5`, `100.0`, `0.30000000000000004`, `1e+20`; so do
`"x=" + r` and `real_to_text(r)`. For fixed decimals use `value:width:precision`;
width `0` means "precision only". The `value:width:precision` spec works **only
inside `print`/`println` arguments**. To capture a formatted Real as `Text` (to
return from a helper or concatenate), use `format_float(value, precision)`.

`println(a, b)` writes its arguments with **nothing between them**, then a
newline: `println(1, 2)` prints `12`. Put the separators in yourself
(`println(a, " ", b)`), and when values belong on separate lines give each
line its own `println`.

| Use case | Syntax | Output |
|---|---|---|
| percentage | `value:0:2` | `95.50` |
| right-aligned | `value:10:2` | `     95.50` |
| default | `value` | `3.14159` |

Division: `/` on two `Int`s gives a `Real` (`println(7 / 2)` prints
`3.5`). An `Int` binding, field, array element, parameter or `-> Int`
`ret` truncates only the final value, so `a / 2 * 2` gives back `a`, and a
quotient used where an `Int` is required — an operand of `%` or `div`, a
`Text` index, `int_to_text` — is a `TYPE-001` error. Write `div` for an
integer quotient (`7 div 2` is `3`, `-7 div 2` is `-3`: it truncates toward
zero), and give `/` a `Real` operand for decimals:

```aether
let successRate: Real = successful * 100.0 / total;
```

## Math builtins

Numeric builtins use Pascal naming — note **`arctan`** (not `atan`) and **`ln`**
(not `log`). Trig is in radians. Unless noted, arguments and results are `Real`.

| Group | Builtins |
|---|---|
| sign / rounding | `abs(x)`, `round(x) -> Int`, `trunc(x) -> Int`, `floor(x) -> Int`, `ceil(x) -> Int` |
| powers / roots | `sqrt(x)`, `sqr(x)` (x²), `pow(base, exp)`, `power(base, exp)` (alias of `pow`) |
| exp / log | `exp(x)`, `ln(x)` (natural log), `log10(x)` |
| trig | `sin(x)`, `cos(x)`, `tan(x)`, `arcsin(x)`, `arccos(x)`, `arctan(x)`, `atan2(y, x)`, `cotan(x)` |
| hyperbolic | `sinh(x)`, `cosh(x)`, `tanh(x)` |
| compare / clamp | `min(a, b)`, `max(a, b)`, `clamp(x, lo, hi)` |
| integer | `odd(n) -> Bool`, `factorial(n) -> Int`, `fibonacci(n) -> Int` |
| random | `random() -> Real` in `[0, 1)`; `random(n) -> Int` in `[0, n)`; seed with `randomize()` |
| decimal rounding | `round_to(x, digits) -> Real`, half away from zero: `round_to(2.675, 2)` is `2.68`, `round_to(-2.5, 0)` is `-3.0` |
| clock | `clock_ms() -> Int` (milliseconds since 1970-01-01 UTC, for timing), `now_text() -> Text` (`"YYYY-MM-DD HH:MM:SS"`, local), `realtimeclock() -> Int` (epoch seconds) — all effectful |

`random` is two functions sharing a name, and they return **different types**.
Assigning the no-argument form to an `Int` truncates `[0, 1)` to a constant `0`,
so every "random" choice becomes the same choice; when you want an index or a
roll, pass the bound — `random(n)`. This now raises `[NARROW-001]`.

`randomize()` seeds from the clock. Each thread derives its own stream, so
`par` branches draw independent numbers; with no `randomize()` call a run is
reproducible, and prints the same numbers on every platform.

`abs`, `min`, `max`, `clamp` preserve their operand type (`Int` in → `Int` out);
`round`/`trunc`/`floor`/`ceil` always return `Int`.

`format_float(x, d)` and `x:0:d` round the stored binary value, so
`format_float(2.675, 2)` is `"2.67"` (2.675 is stored as 2.67499...). When a spec
says amounts round half up, round first: `format_float(round_to(x, 2), 2)`.

Numeric casts: `int(x)` truncates a `Real` (or `Bool`) to `Int`, `real(n)`
widens an `Int` to `Real`, `bool(n)` is `n != 0`; the capitalized spellings
`Int(x)`, `Real(n)`, `Bool(n)` are accepted too. `int(text)` is a `TYPE-001`
error — parse text with `parse_int`.

```aether
let pi: Real = 16.0 * arctan(1.0 / 5.0) - 4.0 * arctan(1.0 / 239.0);  // full Real precision
let hyp: Real = sqrt(sqr(3.0) + sqr(4.0));                            // 5.0
fx { println("pi=", pi:0:6, " hyp=", hyp:0:1); }
```

## TOON handle ownership

`ToonDoc` owns all `ToonNode` handles derived from it.

- `toon_parse(text)` (pure) and `toon_parse_file(path)` (**effectful** — file
  I/O, so call it inside `fx`) both return a `ToonDoc`
- `toon_root(...)`, `toon_key(...)`, and `toon_at(...)` return `ToonNode`
  handles associated with that document (all pure — call outside `fx`)
- `toon_free(node)` releases one node handle early
- `toon_close(doc)` releases the document and any remaining child handles
  derived from it

Practical rule:

- if a document is short-lived, `toon_close(doc)` at the end is usually
  sufficient
- if a loop creates many temporary node handles, prefer `toon_free(...)` for
  those transient nodes to reduce temporary handle-table growth before close

Example. `toon_parse_file` is effectful (file I/O), so parse it inside `fx`,
then use the pure handle ops outside:

```aether
let doc: ToonDoc;
fx {
    doc = toon_parse_file(path);
}
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

## Purity and contracts (ANN-001)

Annotations attach to the next function and sit directly above it — never
inside the body, never bare (`@pre` with no expression).

```aether
@pure
fn classify(score: Int) -> Text {
    if score >= 90 {
        ret "ready";
    }
    ret "blocked";
}

@pre score >= 0
@post result <= 100
fn clamp(score: Int) -> Int {
    if score > 100 {
        ret 100;
    }
    ret score;
}
```

- `@pure` functions reject effectful builtins and calls to any function that
  is not itself marked `@pure` — mark every helper a pure function calls
- `@pre` / `@post` take expressions; `@post` may reference `result`
- a method's `@pre`/`@post` may reference `self.field`, but only when the method
  is declared inside its `type` block (where `self` is implicit); the same
  contract on a top-level `fn` with an explicit `self` parameter fails
  `[SCOPE-001] identifier 'self' not in scope`
- on a collection return (`-> T[]`), a contract must compare a *property* of the
  collection, not the collection itself: `@post length(result) > 0`, never
  `@post result > 0` (an array is not comparable to a scalar, so the whole-array
  form is rejected at compile time with ANN-001). The same applies to a `@pre`
  over an array parameter: `@pre length(xs) > 0`
- a quantifier states a property of every element: `@pre all x in xs: x >= 0`,
  `@post all v in result: v > 0` (see **Quantifiers**)
- for tuple-return helpers, `@post` must use positional slots such as
  `result.0` and `result.1`
- multiple `@pre` and `@post` lines may stack on one function
- `@cost <n><unit>` declares a budget annotation; units: `ns us ms s op ops
  step steps`. The syntax is validated at compile time, but the budget is
  **non-binding today**: nothing tracks or enforces it at runtime (unlike
  `@pre`/`@post`, which become real guards, and `@pure`, which is checked at
  compile time). Treat it as machine-readable documentation of intent

## Strings

Core safe `Text` surface for generated code:

```aether
let nameLen: Int = string_len(name);   // canonical; name.len accepted
let isReady: Bool = status == "ready"; // canonical; string_eq(a,b) accepted
let s: Text = "ab" + "cd";             // concatenation of Text operands
let label: Text = int_to_text(count);  // Int -> Text
```

`int_to_text(n)` is the Int-to-`Text` conversion. Reach for it when you need a
`Text` *value* — for
concatenation, a field, or a return — not merely to print a number
(`println(n)` already prints an `Int` directly).

Additional verified `Text` builtins (all pure, no `fx` needed). Positions are
0-based code points, like `s[i]`:

| Builtin | Effect |
|---|---|
| `s[i]` | the character at `i`; **0-based**, like arrays (`s[0]` is the first) |
| `s[a..b]` | substring from `a` up to but not including `b`, like `arr[a..b]` |
| `s[a..]` / `s[..b]` | from `a` to the end / the first `b` characters; past the end gives `""` |
| `s[i] = "x"` | replace the character at `i` with a one-character `Text` |
| `loop ch in s { ... }` | each character in turn, as a one-character `Text` |
| `contains(s, x) -> Bool` | whether `x` occurs in `s` |
| `starts_with(s, p)` / `ends_with(s, p)` | `Bool` |
| `index_of(s, x) -> Int` | first position of `x` in `s`, **`-1` if absent** (`0` is a match at the start) |
| `count(s, x) -> Int` | non-overlapping occurrences of `x` (`0` for an empty `x`) |
| `upper(s)` / `lower(s)` | the whole `Text` with its ASCII letters case-mapped (`upcase` is an alias of `upper`) |
| `is_digit(t)`, `is_alpha(t)`, `is_alnum(t)`, `is_space(t)`, `is_upper(t)`, `is_lower(t)` | `Bool`: `t` is non-empty and **every** character is in the ASCII class, so `is_digit(s[i])` tests one character and `is_digit("123")` a whole field; `""` is `false`. Space is space, tab, `\n`, `\r`, form feed, vertical tab |
| `trim(s)` | strip leading/trailing whitespace |
| `replace(s, old, new)` | every non-overlapping `old` replaced, left to right; `s` unchanged when `old` is `""` |
| `repeat(s, n)` | `s` written `n` times; `""` when `n <= 0` |
| `reversed(s)` | the characters in reverse order |
| `join(parts, sep)` | a `Text[]` joined with `sep` between elements (`""` for an empty array) |
| `split(t, sep)` | the pieces of `t` between each `sep`, as `Text[]` (at least one) |

```aether
let csv: Text = "  Alice,Bob,Carol  ";
let names: Text[] = split(trim(csv), ",");
let shout: Text = upper(join(names, " & "));          // "ALICE & BOB & CAROL"
let ok: Bool = starts_with(names[0], "Al") && contains(csv, "Bob");
let at: Int = index_of(csv, "Bob");                    // 8
let clean: Text = replace(csv, ",", ";");
let rule: Text = repeat("-", 10);
```

A method spelling (`s.upper()`, `s.contains(x)`, `s.startsWith(p)`,
`s.is_digit()`) is `SCOPE-001` with a hint naming the function, and so is
`isdigit(t)`. `s.trim()` and `s.split(sep)` are
accepted as calls of `trim(s)` / `split(s, sep)`.

### Parsing text into scalars — `parse_*` fails silently, `try_parse_*` does not

`parse_int(t)`, `parse_float(t)`, and `parse_bool(t)` are the canonical
`Text` → scalar conversions. **None of them can report failure.** They are total
functions: every input produces a value, including inputs that are not numbers
at all. When the input is untrusted, use `try_parse_int` / `try_parse_real`
instead — see **Checked parsing** below.

| Call | Result | Why |
|---|---|---|
| `parse_int("42")` | `42` | ordinary success |
| `parse_int("abc")` | `0` | no leading digits — indistinguishable from parsing `"0"` |
| `parse_int("12x")` | `12` | **leading digits only**; the rest is discarded silently |
| `parse_int("")` | `0` | same as `"abc"` |
| `parse_float("zz")` | `0.0` | same rule, as a `Real` |
| `parse_bool("maybe")` | `false` | anything not `true`/`1`/`yes`/`t` is `false` |

Two consequences worth internalizing:

- **A `0` does not mean the input was numeric.** `parse_int(raw) == 0` is true
  for the string `"0"`, for `"abc"`, and for `""` alike. Never use the result
  as its own validity check.
- **A partial parse is not an error.** `"12x"`, `"3.5kg"`, and `"7 items"` all
  parse to their numeric prefix. If a field must be *entirely* numeric, the
  parse will not tell you it was not.

### Checked parsing — `try_parse_int` and `try_parse_real`

`try_parse_int(t) -> (Int, Bool)` and `try_parse_real(t) -> (Real, Bool)` return
the value and whether the **whole** text was a number. They are the right tool
for any field arriving from a file, an argument, stdin, an environment variable,
or a model. Both are pure (no `fx`, legal in `@pure`), and they return a tuple,
so destructure the direct call:

```aether
let (n, ok) = try_parse_int(raw);
if !ok {
    n = -1;
}
```

| Call | value | ok |
|---|---|---|
| `try_parse_int("42")` | `42` | `true` |
| `try_parse_int(" -12 ")` | `-12` | `true` — surrounding spaces and one sign are fine |
| `try_parse_int("12x")` | `0` | `false` — a partial number is a failure |
| `try_parse_int("")` | `0` | `false` — empty is a failure |
| `try_parse_int("3.5")` / `("0x1F")` | `0` | `false` — decimal digits only |
| `try_parse_int("9223372036854775808")` | `0` | `false` — out of range |
| `try_parse_real("3.5")` / `(".5")` / `("1e3")` | `3.5` / `0.5` / `1000.0` | `true` |
| `try_parse_real("inf")` / `("nan")` / `("1e400")` | `0.0` | `false` |

The value is `0` (or `0.0`) whenever `ok` is false, so **check `ok` before
trusting the value** — `0` is also a legitimate parse of `"0"`.

`try_parse_int` is stricter than `parse_int`, not just louder: `"12x"` is a
successful `parse_int` returning `12` and a failed `try_parse_int`. That is the
point: it is what tells you a field was not entirely numeric. A helper that
forwards the result is a tuple-return function too:

```aether
fn readCount(raw: Text) -> (Int, Bool) {
    ret try_parse_int(trim(raw));
}
```

### Validating before parsing

`try_parse_int` returns a tuple, which an expression such as a `@pre` cannot
read. When you want the requirement carried by a contract instead, `is_digit`
is a pure `Bool` over the whole `Text` (`false` for `""`), so it can serve as
a `@pre` on the helper that consumes the field:

```aether
@pre is_digit(raw)
@pure
fn scoreOf(raw: Text) -> Int {
    ret parse_int(raw);
}
```

Guard at the call site with `if is_digit(raw) { ... }`, or let the contract
carry the requirement as above. Note `is_digit` rejects a leading `-` and
surrounding spaces, which `try_parse_int` accepts; write
`is_digit(raw) || (starts_with(raw, "-") && is_digit(raw[1..]))` if negative
fields are in scope. Prefer
`try_parse_int` unless you specifically need the requirement expressed as a
contract — it is one call, it already handles a sign and surrounding
whitespace, and it cannot drift out of sync with the parse it guards.

## Dynamic arrays

**Nested arrays are real.** A row is an `Int[]`, so a table is `Int[][]`, and
every 1-D operation composes into it:

```aether
let table: Int[][] = [];
loop r in 0..rows {
    let row: Int[] = [];
    loop c in 0..cols {
        row = row + [r * cols + c];
    }
    table = table + [row];
}
table[1][2] = 99;                       // writable through both indexes
let width: Int = length(table[1]);      // rows are independent -- may be jagged
```

Bound an inner loop with `length(table[r])`, never the first row's width.
Declaring `Int[]` and then writing `dp[i][j]` is `[ARR-002]`: the rank of the
*declaration* is wrong, not the indexing syntax. If you would rather keep one
dimension, index with a computed offset — `table[r * width + c]`.

```aether
let xs: Int[] = [];
xs = xs + [7];          // append pattern (any literal length: [7], [7, 8], even [])
let n: Int = length(xs);
let first: Int = xs[0]; // indexed read
xs[0] = 9;              // indexed write
let ys: Int[] = [1, 2, 3];
let zs: Int[] = xs + ys; // concatenation: `arr1 + arr2`, both already-built arrays
xs = xs + ys;            // same idiom, self-reassignment form
let mid: Int[] = ys[0..2]; // slice: a COPY of elements 0..1 (half-open, like loop ranges)
let rest: Int[] = ys[1..]; // open end: elements 1 to the end; ys[..2] is ys[0..2]
```

- `Type[]` declares; `[]` is the empty literal
- indexed reads/writes such as `xs[0]` and `xs[0] = v;` are supported
- multi-element literals such as `[1, 2, 3]` are supported
- `xs = xs + [a, b, ...];` appends every element of a literal, in order,
  regardless of the literal's length (one element, many, or even `[]`, a no-op)
- `xs = xs + ys;` / `let zs: T[] = xs + ys;` (`+` between two array-valued
  expressions, neither necessarily a literal) concatenates: the RHS is
  evaluated once and every one of its elements is appended, in order
- `xs[a..b]` slices out a **copy** of elements `a` through `b - 1`
  (half-open, same convention as `loop i in a..b`). Either bound may be left
  out: `xs[a..]` runs to the end (`queue = queue[1..];` drops the head) and
  `xs[..b]` starts at 0. A start past the end, or after the stop, is a run-time
  `ARR-003`. This is sugar scoped to indexing brackets, not a first-class Range
  value -- `a..b` has no meaning outside `[...]`, and a `loop` range still needs
  both bounds
- `length(xs)` canonical; `len(xs)` and `xs.len` accepted
- `[v; n]` is n copies of v: `let counts: Int[] = [0; 26];`, a table
  `let grid: Int[][] = [[0; cols]; rows];` (each row its own copy); v and n are
  evaluated once, and n <= 0 gives `[]`. A record value is `TYPE-001` (one
  record would fill every slot): build records in a loop
- there is no resize call: grow with `xs = xs + [v];`, shrink with a slice
  (`xs = xs[..n];`, `stack = stack[..length(stack) - 1];` pops)
- `xs == ys` / `xs != ys` compare structurally: equal when both have the same
  length and every element compares equal (nested arrays and records recurse)
- records and tuples compare by value too: `a == b` is true when every field
  (tuple slot) is equal, recursively; `same(a, b)` asks whether two records are
  the same record. `== nil` works as before
- copying a nested array (`let b: Int[][] = a;`, passing it, or placing it in a
  literal) copies its rows too, however they were built: `b[0][0] = 99` never
  changes `a`
- never `toon_len(xs)` on a dynamic array, nor `length(node)` on a `ToonNode`
  (LEN-001): both compile, and stop at run time with an uncoded error
- `println`/`print` cannot print a whole array: `println("data: ", xs)` is a
  `TYPE-001` error. Loop and print each element:
  `fx { loop x in xs { print(x, " "); } println(""); }`

**Arrays are value-copied where they cross a function boundary; records are
pointer-backed and are not.** Passing `xs` into a function hands that
function its own copy: mutating it there (`arr[0] = 9;` inside the callee)
never changes the caller's array, unlike the identical pattern on a record
parameter (which *does* propagate, since records are reference-backed). If a
function needs to transform an array for its caller, return the new array
and reassign at the call site:

```aether
fn doubleAll(arr: Int[]) -> Int[] {
    loop i in 0..length(arr) { arr[i] = arr[i] * 2; }
    ret arr;                    // required -- in-place writes alone are invisible to the caller
}
xs = doubleAll(xs);
```

A function that changes an array parameter -- an element (`arr[i] = v`), a
nested element (`grid[r][c] = v`), an append or a reassignment -- and never
returns it is a compile-time `[ARR-001]` error, and so is a call statement that
throws away the array it returns (`sortDesc(xs);`, `sorted(xs);`): the caller
would never see the change. Return it and reassign, `xs = f(xs)`. For scratch
work inside a function, copy the parameter to a local first
(`let work: Int[] = xs;`) and change the copy.

Scalar parameters (`Int`, `Real`, `Bool`, `Text`) are copies too: a parser
helper that advances its `pos` never moves the caller's `pos`, so every level
re-reads from the start. Return the new position with the value
(`-> (Int, Int)`, then `let (v, next) = f(s, pos); pos = next;`), or write the
helpers as nested `fn`s that assign the enclosing `pos`, or keep the cursor in a
record field; the compiler warns `[PARAM-001]` on the shapes it can see.

## Standard helpers

The helpers models most often reach for exist. They are pure, they return a
new value and never modify their argument, and they work on `Int[]`, `Real[]`
and `Text[]` unless noted. Use them instead of hand-written loops (`BUILT-001`:
a guessed name never compiles, a listed one does).

| Helper | Result |
|---|---|
| `sum(xs)` | `Int` for an `Int[]`, `Real` for a `Real[]`; `0` when empty |
| `min_of(xs)` / `max_of(xs)` | the smallest / largest element (`Text` compares by code point); a run-time error when `xs` is empty |
| `sorted(xs)` / `sorted_desc(xs)` | a new array, ascending / descending; stable |
| `reversed(xs)` | a new array, last element first |
| `contains(xs, v)` | `Bool` |
| `index_of(xs, v)` | the first index of `v`, or `-1` |
| `count(xs, v)` | how many elements equal `v` |
| `sorted_by(rs, "field")` / `sorted_by_desc(rs, "field")` | an array of records sorted by one `Int`, `Real` or `Text` field; stable |

```aether
let xs: Int[] = [5, 3, 9, 3, 1];
let ys: Int[] = sorted(xs);                  // [1, 3, 3, 5, 9]; xs is unchanged
let mean: Real = sum(xs) * 1.0 / length(xs); // 4.2
let spread: Int = max_of(xs) - min_of(xs);   // 8
let threes: Int = count(xs, 3);              // 2
```

`min` and `max` (two arguments) still compare two values; `min_of` / `max_of`
take the array.

**Sorting records.** `sorted_by` takes the field's name as a **string literal**;
the compiler checks it names an `Int`, `Real` or `Text` field of the element
type (`FIELD-002` otherwise), so a typo cannot reach run time. There is no
comparator (functions are not values, FUNC-001); for a two-key order, sort by
the minor key first and then by the major one — the sorts are stable:

```aether
type Player {
    name: Text;
    team: Text;
    score: Int;
}

fn main() -> Void {
    let ps: Player[] = [new Player { name: "ann", team: "red", score: 7 },
                        new Player { name: "bob", team: "blue", score: 9 },
                        new Player { name: "cy", team: "red", score: 9 }];
    let byScore: Player[] = sorted_by_desc(ps, "score");          // bob, cy, ann
    let byTeam: Player[] = sorted_by(sorted_by_desc(ps, "score"), "team");
    fx {
        loop p in byScore { print(p.name, " "); }
        println("");
        loop p in byTeam { print(p.team, ":", p.name, " "); }
        println("");
    }
    ret;
}
```

### What is still written by hand

A missing helper is not a missing capability. These are ordinary first-order
Aether — no closures, no generics, no container types beyond arrays and
records; the absence of `map` is a deliberate consequence of FUNC-001, not a
gap.

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

### Maps

`Map[Text, T]` maps Text keys to values of one type `T` (any type: `Int`,
`Real`, `Text`, `Bool`, a record, an array, another map). Use it for counting,
grouping and lookups by name.

```aether
@pure
fn word_counts(words: Text[]) -> Map[Text, Int] {
    let counts: Map[Text, Int] = {};
    loop w in words {
        counts[w] = get(counts, w, 0) + 1;
    }
    ret counts;
}

fn main() -> Void {
    let counts = word_counts(split("the cat and the hat", " "));
    let ages: Map[Text, Int] = {"ann": 31, "bob": 27};
    ages["cid"] = 40;               // insert or overwrite
    remove(ages, "bob");            // nothing happens if absent
    let groups: Map[Text, Text[]] = {};
    loop w in split("apple avocado bean", " ") {
        let k = w[..1];
        if !has(groups, k) { groups[k] = []; }
        groups[k] = groups[k] + [w];
    }
    fx {
        println(counts["the"], " ", length(counts));   // 2 4
        loop k in ages { println(k, "=", ages[k]); }   // ann=31, cid=40
        println(groups);           // {"a": ["apple", "avocado"], "b": ["bean"]}
    }
}
```

- `m[k]` reads a key that must be there; a missing key stops the program with
  a run-time error naming it. Test first with `has(m, k)` (or `k in m`, `k not
  in m`), or read with a default: `get(m, k, default)`.
- `m[k] = v` inserts or overwrites; `m[k] += 1` and writes into a stored value
  (`m[k][i] = v`, `m[a][b] = v`, `m[k].field = v`) need the key to be there.
- `keys(m)` and `values(m)` are arrays in insertion order; `loop k in m` visits
  a snapshot of the keys in that order; `length(m)` counts entries. Overwriting
  a key keeps its place; `remove` then re-insert moves it to the end. For
  sorted output, loop over `sorted(keys(m))`.
- `{}` needs a declared type: `let m: Map[Text, Int] = {};`. A literal's values
  are one type, keys are Text (`m[int_to_text(i)]` for a number), and a key
  written twice in one literal is NAME-001.
- A map is a **value**, like an array: `let b = m;` and a call copy it. A
  function that changes a map returns it, `m = add(m, w)`; a write to a map
  parameter that is not returned is lost (ARR-001).
- `==` compares keys and values, ignoring order. `println(m)` prints
  `{"ann": 31, "cid": 40}` in insertion order; a map of records does not print
  (loop and print the fields).
- There are no methods on a map and no set type: a set of Text is
  `Map[Text, Bool]`.

For keys that arrive from a parsed payload rather than from your own code,
the TOON accessors (`toon_get_int_or`, `toon_has_key`) read the node directly —
it is already keyed.

## Structured data: TOON

**LLM-critical.** TOON helpers ride on yyjson; JSON-compatible payloads are
the safe path. `ToonDoc` = parsed document, `ToonNode` = node within it; both
opaque (TOON-001). Keys are `Text`, indexes `Int`. Always `toon_close(doc)`.

Golden shape rules:

- `toon_parse(...)` / `toon_parse_file(...)` returns `ToonDoc`
- `toon_root(doc)` returns `ToonNode`
- `toon_get_*`, `toon_key`, and `toon_at` operate on `ToonNode`, never
  directly on `ToonDoc`
- if the payload starts with `{`, inspect named keys on `root`; if it starts
  with `[`, iterate `root` directly
- a missing intermediate object degrades to the `_or` fallback; it never
  crashes (NEST-001)

### Helper surface (complete — BUILT-001)

| Need | Helper |
|---|---|
| availability | `has_toon()` |
| parse text / file | `toon_parse(text)`, `toon_parse_file(path)` |
| root / close | `toon_root(doc)`, `toon_close(doc)` |
| object field / array element | `toon_key(node, key)`, `toon_at(node, i)` |
| defaulted field node | `toon_key_or(node, key, fallback)` — pair it with `toon_null()`, an always-null node (`toon_is_null` is true) |
| missing key | `toon_key(node, "absent")` is a *missing* node, not a null one: every `toon_is_*` is false and `toon_type` is `""`; test `toon_has_key` first, or use `toon_key_or` |
| TOON array length | `toon_len(node)` |
| typed field get | `toon_get_text(node, key)`, `toon_get_int(node, key)`, `toon_get_real(node, key)`, `toon_get_bool(node, key)` |
| typed field get w/ fallback | `toon_get_text_or(node, key, fb)`, `toon_get_int_or(node, key, fb)`, `toon_get_real_or(node, key, fb)`, `toon_get_bool_or(node, key, fb)` — `fb` when the key is missing, null or of another type (an Int satisfies `toon_get_real_or`) |
| node value | `toon_text_value(node)`, `toon_int_value(node)`, `toon_real_value(node)`, `toon_bool_value(node)`, `toon_null_value(node)` (a `Bool`: is the node null?) |
| kind / membership | `toon_type(node)`, `toon_has_key(node, key)`, `toon_has_at(node, i)` |
| shape checks | `toon_is_text(node)`, `toon_is_int(node)`, `toon_is_real(node)`, `toon_is_bool(node)`, `toon_is_null(node)`, `toon_is_arr(node)`, `toon_is_obj(node)` |

Every name above is spelled out in full on purpose. The **node-value**
accessors take the `_value` suffix (`toon_int_value`) while the **field**
getters do not (`toon_get_int`). There is no bare `toon_int`, `toon_text`,
or `toon_bool` — those are the most common wrong guesses.

Prefer `toon_is_*` predicates for control flow instead of depending on exact
`toon_type(node)` string values.

### Root shape rule (ROOT-001)

Decide the top-level shape first:

```aether
// JSON starts with '[': root IS the array
let root: ToonNode = toon_root(doc);
loop i in 0..toon_len(root) {
    let user: ToonNode = toon_at(root, i);
}

// JSON starts with '{': extract the named array, then iterate
let root: ToonNode = toon_root(doc);
let jobs: ToonNode = toon_key(root, "jobs");
loop i in 0..toon_len(jobs) {
    let job: ToonNode = toon_at(jobs, i);
}
```

`toon_at(root, 0)` is the first *element*, never "the array". If the prompt
names a collection (`jobs`, `users`, `releases`) and the payload is
object-shaped, bind that named array first.

Wrong:

```aether
let doc: ToonDoc;
fx {
    doc = toon_parse_file("payload.json");
}
let name: Text = toon_get_text(doc, "name");
```

Right:

```aether
let doc: ToonDoc;
fx {
    doc = toon_parse_file("payload.json");
}
let root: ToonNode = toon_root(doc);
let name: Text = toon_get_text(root, "name");
```

### Key fidelity (KEY-001)

Copy JSON keys exactly; never normalize (`"name"` under `"app"` is
`toon_get_text_or(app, "name", "")`, not `toon_get_text_or(root, "appName",
"")`). One-character keys like `"v"` are valid.

A key may be a **dotted path** through nested objects: `toon_key`,
`toon_has_key` and every `toon_get_*` getter accept `"server.port"` and walk
one object per segment, so `toon_get_int_or(root, "server.port", 0)` reads the
nested field in one call. A missing or non-object segment degrades exactly like
a missing key — the `_or` fallback, `false` from `toon_has_key`, the zero value
from a plain getter. Segments are object keys only; an array index is not a
segment (`"rows.0.k"` is simply absent), so index arrays with `toon_at`.

### Nested lookup safety (NEST-001)

A missing intermediate never crashes: `toon_key` on an absent key yields an
absent node, and every accessor on an absent node degrades — `_or` getters
return their fallback, `toon_has_key` is `false`, `toon_len` is 0, plain getters
return the zero value. So both one-call forms are safe:

```aether
let code: Text = toon_get_text_or(row, "meta.code", "EMPTY");
let same: Text = toon_get_text_or(toon_key(row, "meta"), "code", "EMPTY");
```

What the fallback cannot tell you is *which* segment was missing. When the
output must distinguish "no meta" from "meta without a code", guard the
intermediate and bind it:

```aether
let code: Text = "EMPTY";
if toon_has_key(row, "meta") {
    let meta: ToonNode = toon_key(row, "meta");
    code = toon_get_text_or(meta, "code", "EMPTY");
}
```

### Worked example

```aether
fn main() -> Void {
    if !has_toon() {
        fx { println("structured data support unavailable"); }
        ret;
    }

    let doc: ToonDoc = toon_parse("{\"name\":\"Aether\",\"enabled\":true,\"count\":2}");
    let root: ToonNode = toon_root(doc);

    let name: Text = toon_get_text(root, "name");
    let enabled: Bool = toon_get_bool(root, "enabled");
    let count: Int = toon_get_int_or(root, "count", 0);

    fx {
        println("name = ", name);
        println("enabled = ", enabled);
        println("count = ", count);
    }

    toon_close(doc);
    ret;
}
```

## Concurrency: `par`

To run your own functions concurrently, use a `par { ... }` block. Each
statement inside must be a direct function call; the calls run in parallel and
the block joins (waits for all) before execution continues.

```aether
par {
    workerA();
    workerB();
}
```

`par` bodies accept direct calls only — no assignments, loops, or inline `fx`
(push those into the called functions). Return results through pointer-backed
records passed as arguments: a callee that writes `out.count = n` makes the
value visible to the caller after the block joins.

```aether
type Box {
    count: Int;
}

fn tally(out: Box, n: Int) -> Void {
    let acc: Int = 0;
    loop i in 0..n { acc = acc + i; }
    out.count = acc;
    ret;
}

fn main() -> Void {
    let a: Box = new Box();
    let b: Box = new Box();
    par {
        tally(a, 100);
        tally(b, 200);
    }
    fx { println("a=", a.count, " b=", b.count); }
    ret;
}
```

A branch target may be defined anywhere in the file, above or below the function
holding the `par` block, exactly like an ordinary call.

This is the capture-free alternative to spawning closures (FUNC-001): you
parallelize ordinary calls and collect results through records, with no
function values. `par` is the mechanism for user code; the task helpers below
are a lower-level handle API over runtime builtins.

## Tasks and AI helpers

Compact aliases over shared runtime helpers; all are effectful — call inside
`fx` (FX-001) — except the `has_*` probes, which are pure. `task_spawn` /
`task_queue` dispatch an allow-listed runtime *builtin* by its Aether name
(for example `"sleep"` or `"dns_lookup"`), not a user-defined function;
passing your own function name is an error. To parallelize your own functions, use `par` (above).

Core signatures:

- `sleep(ms: Int) -> Void`
- `task_spawn(target: Text, name: Text, arg) -> Int`
- `task_queue(target: Text, name: Text, arg) -> Int`
- `task_wait(handle: Int) -> Int` — joins the task; `0` when it succeeded. It
  consumes the stored status
- `task_lookup(name: Text) -> Int`
- `task_status(handle: Int) -> Bool` — readable only once the task has finished
  and before `task_wait`; on a running or an already-joined task it is a
  run-time error
- `task_result(handle: Int)` — the target builtin's own result (nothing for a
  procedure such as `"delay"`); a run-time error while the task is still running
- `task_stats() -> Array`: worker-pool records with no Aether type, so count
  them (`length(task_stats())`) and read fields through `task_stats_json()`
- `task_stats_json() -> Text`
- `ai_chat(model: Text, messages: Text, system: Text = "", apiKey: Text = "", endpoint: Text = "") -> Text`:
  `messages` is a JSON array of `{"role": ..., "content": ...}` objects;
  `system` is sent as a system message ahead of them. Text that is JSON object
  fields instead (`"\"temperature\": 0"`) is merged into the request
  object, not sent as a message
- `aether_version() -> Text`: this compiler's version, as `aether --version`
  prints it after "Version:", e.g. `"2026-10-20-14 (1.0-beta3)"`: the language
  version, then the release label. Pure, and fixed for a given compiler.
- probes: `has_ai() -> Bool`, `has_builtin(category: Text, function: Text) -> Bool`.
  `has_builtin` answers "can I call this?" for the names this guide uses
  (`has_builtin("sqlite", "sqlite_open")`), across both the extended and the
  core builtin registries; a core builtin matches on the function name
  whatever category you pass, since core builtins are not filed under one

Use `sleep(ms)` for a blocking millisecond pause. It is the compact Aether
spelling for the shared PSCAL `delay(ms)` builtin and is effectful, so it must
stay inside `fx`. `task_wait(handle)` waits for a task handle; it is not a
timer and should not be used like `task_wait(100)`.

## Discovering builtins beyond this guide

This guide documents the **core surface**: the builtins you reach for in almost
every program (output, TOON, types, control flow, math, strings, conversion,
files/env, tasks/`par`, contracts, modules). The runtime also exposes many more
— deeper networking, database, lower-level I/O — and a host program can register
**custom builtins** that no static guide could anticipate. Rather than list them
all, discover them:

- `builtins_json()` → a JSON array with one object per Aether-visible builtin
  (`name`, `kind`, `category`, `effectful`, `return_type`, ...), not an
  array of names: read `toon_get_text(toon_at(root, i), "name")`
- `builtins_json(true)` → richer metadata per builtin (`signature`,
  `return_type`, `effectful`, `usage`, `category`)
- `builtin_info(name)` → the metadata for one builtin
- `aether --dump-ext-builtins` (CLI) → the registered custom/extended builtins

**This is an agentic capability.** You can only query by running the compiler or
a probe program, so it is for an agent that can execute and read the result. The
workflow: discover the exact **name, signature, and `effectful` flag**, then call
it — wrapping it in `fx` if `effectful` is true. If you cannot run the compiler
(one-shot generation), stay within this guide's documented surface; never call,
or guess the signature of, a builtin you have not verified. Discovering a name is
not permission to guess how it is called.

## Modules (IMP-001, MOD-001)

Hard rules:

- write `use "..."` only when the module is provided in the prompt, present in
  the repository, or otherwise verified; otherwise stay self-contained
- a missing `use` target may be silently ignored in compatibility mode, but
  its names still do not exist
- canonical import form is `use "module_name";` in new code
- **MOD-001**: imported names must match exported names exactly; `use` does
  not rename. If a module exports `labelScore`, call
  `labelScore(...)` — never a guessed local like `label(...)` unless
  you define the wrapper yourself
- after import, exported constants and helpers may be called directly; prefer
  direct calls like `area(3, 4)` and `Sides` over guessed foreign object APIs
- file naming: `mod PascalCaseName` lives in a snake_case file consumed as
  `use "pascal_case_name";` (e.g. `mod ModuleConsts` → `use "module_consts";`)
- when combining purity with module export, write `@pure` above `export fn`
- a module follows the same effect rules as your file: its effects need
  `fx`, its functions may not write an array parameter they never return
  (ARR-001), and a `@pure` function cannot call an export that performs
  effects (ANN-001)
- for generated code, assume modules export `const` and `fn`; do not generate
  exported `type` blocks
- if the task provides a module, copy its export names exactly; do not invent
  wrapper names such as `label(...)`, `normalize(...)`, or `getArea()`

```aether
mod ScoreUtils {
    export const BaseScore: Int = 50;

    @pure
    export fn clampScore(score: Int) -> Int {
        if score < 0 {
            ret 0;
        }
        if score > 100 {
            ret 100;
        }
        ret score;
    }
}
```

```aether
use "score_utils";

fn summarize(job: ToonNode) -> Int {
    let raw: Int = toon_get_int_or(job, "score", BaseScore);
    ret clampScore(raw);
}
```

Wrong:

```aether
use "score_utils";
let score: Int = normalize(141);
let status: Text = label(score);
```

Real module examples: `Examples/aether/base/module_demo`,
`Examples/aether/base/module_consts_demo`.

Module recipe for generated code:

1. Add `use "module_name";` only when the module is verified or provided.
2. Copy exported names exactly as written in the module file.
3. Bind imported results to typed locals if they feed later logic.
4. Do not invent wrappers unless you define them yourself.

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

## Diagnostics

Failures report original Aether source lines (`file:line:` prefix in CLI
mode), not lowered Rea lines; trust the Aether line first.
`--diagnostics-json` / `--diagnostics-toon` emit structured failures.
`--verbose-compat` (alias `--verbose-errors`) surfaces normally-quiet
compatibility shims such as ignored missing imports.

## Copyable templates

### Pure helper + effectful main

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

### TOON array classification

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
        let name: Text = toon_get_text(row, "name");
        let score: Int = toon_get_int_or(row, "score", 0);
        let status: Text = classify(score);

        fx {
            println(name, ": ", status);
        }
    }

    toon_close(doc);
    ret;
}
```

### Safe nested TOON extraction

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

The `if toon_has_key(...)` guard above is correct, but it costs a nesting level
for every segment of the path, and a three-segment path such as
`spec.mem.gb` turns one field read into a dozen lines.

`toon_key_or(node, key, toon_null())` collapses it. On a missing key it returns a
null node, and a null node absorbs the remainder of the walk: `toon_len` is 0 and
every `_or` getter yields its fallback. Chains of any depth are therefore total,
with no guard and no mutable accumulator:

```aether
let spec: ToonNode = toon_key_or(row, "spec", toon_null());
let mem: ToonNode = toon_key_or(spec, "mem", toon_null());
let gb: Int = toon_get_int_or(mem, "gb", 0);   // 0 if spec OR mem is absent
```

Prefer this for anything deeper than one level. Reach for `toon_has_key` when you
need to *distinguish* an absent key from a present one holding a falsy value —
the null-absorbing chain deliberately conflates the two.

One exception to the absorption rule: `toon_has_at(node, i)` raises on a null
node rather than returning false. Take the length instead — `toon_len` is 0 on a
null node, so `i < toon_len(node)` is the safe index test.

### Large report recipe

Use this shape for long report programs:

1. `const` defaults first
2. pure normalization / classification helpers next
3. one mutable `type` for counters or rollup state
4. in `main`, parse TOON, bind `root`, extract the named array, iterate once
5. inside the loop: extract, normalize, classify, update counters, print
6. after the loop: print totals, then `toon_close(doc);`

```aether
type Totals {
    total: Int;
    ready: Int;
    review: Int;
    blocked: Int;

    fn record(status: Text) -> Void {
        self.total = self.total + 1;
        if status == "ready" {
            self.ready = self.ready + 1;
            ret;
        }
        if status == "review" {
            self.review = self.review + 1;
            ret;
        }
        self.blocked = self.blocked + 1;
        ret;
    }
}
```

### Compact object with method

```aether
type Counter {
    value: Int;

    fn bump() -> Int {
        self.value = self.value + 1;
        ret self.value;
    }
}

fn main() -> Void {
    let counter: Counter = new Counter();
    counter.value = 41;
    let answer: Int = counter.bump();
    fx {
        println(answer);
    }
    ret;
}
```

### Method with explicit loop context

```aether
type Audit {
    valid: Int;
    invalid: Int;

    fn record(rowIndex: Int, ok: Bool, amount: Int) -> Void {
        if ok {
            self.valid = self.valid + 1;
        } else {
            self.invalid = self.invalid + 1;
        }
        fx {
            println("row ", rowIndex, " -> ", amount);
        }
        ret;
    }
}
```

Larger example: `Examples/aether/showcase/agent_report`.

## Repair rules

The compiler tags most rejections with a stable code in brackets and usually a
`help: see <CODE> ...` line. Read the code, then apply the fix.
The codes the compiler actually emits are FX-001, SYN-001, ANN-001, IMP-001,
SCOPE-001, TOON-001, MS-001, TYPE-001, TYPE-002, TUP-001, FLOW-001, FLOW-002,
FIELD-002, FIELD-003, PAR-001, PAR-002, NAME-001, BUILT-002, ARR-001, ARR-002,
REC-001, FUNC-001, NARROW-001, PARAM-001, PREC-001, FMT-001, DIV-002, and ENTRY-001; the finer rule names
below map onto them. Run-time errors (ARR-003, CON-001, `AETH-RUNTIME-*`) carry their
own message and hint. A compile-time code you receive that is not on this list
does not exist — re-read the message rather than inventing a rule for it.

- **[FX-001]** an output, task helper, or `ai_chat` call outside an effect block
  → wrap it in `fx { ... }`.
- **[SYN-001]** non-Aether syntax → `ret` not `return`, `type` (with Aether field
  syntax) not `class`; drop `var`, `def`, `=>`. The message names the word and
  the Aether form (`'return' is not Aether syntax` … `write ret value;`). Also a
  **field or method named after an Aether keyword or operator word** (`new`,
  `for`, `loop`, `div`, `and`, ...) → rename the member (see Records: `type`).
  And `new Int[n]`: there is no sized array constructor → write `[0; n]`.
- **[SCOPE-001]** a name/scope problem — the catch-all. It is one of:
  - a helper not listed in this document → it does not exist; inline the logic (BUILT-001)
  - an export called by a guessed name → use the exact exported name (MOD-001)
  - a method reaching an outer local → pass it in as a parameter (METH-001)
  - `method '<m>' is not defined on type '<T>'` → define `fn <m>(...)` inside
    `type <T> { ... }`, or fix the call name (METH-001)
  - a genuinely undeclared / out-of-scope name → declare it earlier, pass it in,
    or rename the local you actually meant (SCOPE-001)
  - a removed spelling → write the form its hint names (**Removed spellings**
    below)
  - a method spelling on a builtin type (`s.upper()`, `xs.sort()`,
    `xs.join(",")`) → call the function the hint names (`upper(s)`,
    `sorted(xs)`, `join(xs, ",")`)
- **[NAME-001]** a name declared twice → pick a fresh name, or assign without
  `let`. Two names may not differ only by case, so this covers `let limit` beside `const LIMIT` (top
  level or imported), a parameter named like a const, `x` beside `X`, a `let`
  that redeclares a parameter, and a loop variable named like a variable
  already in scope (the loop would get its own, leaving the outer one
  unchanged). Also a method or function named `length`, `setlength`, `copy` or
  `halt`, a function named like a builtin the compiler handles by name (`print`,
  `println`, `printf`, `write`, `writeln`, `lock`, `unlock`, `destroy`, `mutex`,
  the TOON and memory-stream getters), and any identifier starting with `__`.
  Any other builtin name (`rename`, `swap`, `pow`) is fine for a function:
  calls reach the function.
- **[IMP-001]** a malformed `use` → write `use "module_name";` and call exports
  directly (MOD-002). A `use` of a module that does not exist is not reported
  by itself; its names then fail with SCOPE-001.
- **[TYPE-001]** a type cannot be inferred → annotate it (including an untyped
  array literal such as `let xs: Int[] = [1, 2, 3];`). Also a foreach over a
  tuple item or a field of a type from a `use`d module (`cannot infer the type
  of the collection`) → bind it first, `let items: Int[] = t.0;`, and loop
  over `items`. Also
  a value of the wrong kind, where the message names the fix: `println` of
  a whole array, `int()` of a `Text`, and a `Real` where an `Int` is required
  (an operand of `%`, `div` or a shift, an array or `Text` index, a loop range
  bound or step, `int_to_text`, `chr`, `copy`): `/` always gives a `Real`, so
  an integer quotient is `a div b`.
- **[TYPE-002]** a type *name* that does not exist, in any position (binding,
  parameter, return type, record field, tuple item) and at any array depth — it
  is neither a builtin nor a `type` you declared or imported; declaration order
  does not matter. Distinct from TYPE-001, where the type is
  real and used wrongly: here the fix is the spelling. Use `Int`, `Real`, `Text`,
  `Bool`, `Void`, `T[]`, or one of your own `type` names. The message names the
  Aether spelling whenever one exists (`Double` → `Real`, `Integer` → `Int`,
  `Boolean` → `Bool`, `Char` → `Text`, `Byte` / `Word` → `Int`, lowercase `int` →
  `Int`), and for `List` / `Dict` / `Set` it names the shape instead:
  an array is `T[]`, a map `Map[Text, T]`, a set `Map[Text, Bool]`. `Float` and `String` are
  the two foreign spellings that are *accepted*, as aliases of `Real` and `Text`.
- **[TOON-001]** a `ToonDoc` / `ToonNode` handle misuse → check handle types; do
  not mix, do arithmetic on, or cross-assign them. A handle is not an array:
  `node[i]` is TOON-001 (`toon_at(node, i)`, `toon_len(node)`), and `ms[i]` on
  an `MStream` is MS-001 (index `mstream_text(ms)`).
- **[FIELD-002]** `Unknown field` → use the exact declared field name, or extend
  the type explicitly if the prompt really requires that field.
- **[FIELD-003]** a field default that is not a literal — an expression, a named
  `const` (`= MAX_SCORE` is rejected, not just `= MAX_SCORE + 1`), or (references
  another field, `self`, or calls a function), or a populated array default →
  use a literal/constant (`count: Int = 0`), or drop the default and set the
  value at construction with `new T { field: value }`. A type-mismatched default
  (`value: Int = "x"`) is a `[TYPE-001]` instead — make the default's type match
  the field.
- **[PREC-001]** (warning) a bitwise operator against a comparison,
  `flags & mask != 0` → `&`, `|` and `^` bind looser than `== != < <= > >=`, so
  this groups as `flags & (mask != 0)` and evaluates to an **Int**, not a Bool.
  It compiles and prints a number, so the test looks like it passes.
  Parenthesize the mask: `(flags & mask) != 0`. As an **error**: comparisons do
  not chain — `0 <= i < n`, `a == b == c` and an unparenthesised `not a == b` →
  write `0 <= i && i < n`.
- **[ARR-002]** a one-dimensional array indexed twice → the *declaration's* rank
  is wrong, not the indexing syntax. Nested arrays are real: declare `T[][]`,
  build each row as its own `T[]` (`row = row + [v];` then
  `table = table + [row];`), and bound inner loops with `length(table[r])` since
  rows may be jagged. Or keep one dimension and index with a computed offset,
  `table[r * width + c]`. A `Text[]` is the exception: `lines[i][0]` is the
  first character of `lines[i]`, not a second dimension.
- **`Length expects a string or array argument`** or **`YyjsonGetLength expects a
  single value handle`** *(runtime, no bracketed code)* → `length` was handed a
  `ToonNode`, or `toon_len` an array or a `Text` (LEN-001). Use
  `toon_len(node)` for a `ToonNode` and `length(xs)` for an array or a `Text`.
  Both mistakes compile; only running the program shows them.
- **[ARR-003]** *(runtime)* an index outside the array's valid range. The message
  gives the offending index, which dimension it was in, and the valid range, and
  the `[Error Location]` line that follows gives the source line. Read the range
  first: `valid indices are 0..0` means the array holds exactly one element, so
  the bug is usually that the row you are indexing **has not been appended yet**.
  Building a table row by row is the classic case — inside the loop, the current
  row is still the local `row`, not `table[i]`:

  ```aether
  loop i in 0..rows {
      let row: Int[] = [];
      loop j in 0..cols {
          // ✗ table[i][j - 1] -- row i is not in `table` until after this loop
          // ✓ row[j - 1]      -- the row being built right now
          row = row + [j];
      }
      table = table + [row];   // only now does table[i] exist
  }
  ```

  Previously-appended rows *are* readable as `table[i - 1][j]`. If the index is
  genuinely meant to be in range, the loop bound is what to fix — remember
  `loop i in 0..n` is half-open, so the last valid index is `n - 1`.
- **[CON-001]** *(runtime)* `Aether @pre failed in f` (or `@post`) on stderr,
  exit status 1: a call broke `f`'s contract. For `@pre` the caller passed a bad
  argument; for `@post`, `f` computed the wrong `result`. Fix the code, not the
  contract, unless the contract is what is wrong.
- **[FLOW-001]** a fallthrough path with no return value → add an explicit final
  `ret ...` on every reachable top-level path in the non-`Void` helper.
- **[FLOW-002]** an empty `ret;` in a non-`Void` function (a `ret` with no value)
  → give the return a value (`ret <expr>;`), or declare the function `-> Void`.
- **[ANN-001]** a misplaced annotation, or a `@pure` function calling an effect or
  a function that is not `@pure` → move `@pre`/`@post`/`@pure`/`@cost` directly
  above the function; keep effects out of pure code and mark its helpers `@pure`.
- **[REC-001]** `==` on a record type that contains itself (`next: Node`) →
  `same(a, b)` for "the same record", or compare the fields you mean.
- **[TUP-001]** tuple misuse → destructure a direct top-level call
  (`let (a, b) = pair();`), or bind it and read one field (`let t = pair();
  t.0;`); an index past the arity or chained directly onto the call
  (`pair().0`) both fail here too. If the callee isn't a known top-level
  tuple-returning function, return a record instead and read its fields.
- **[BUILT-002]** a listed builtin called with the wrong number of arguments →
  use the count in its signature (`format_float(r, 2)`, never a width slot).
- **[ARR-001]** a function changes an array or map parameter it never returns
  (`xs[i] = v`, `grid[r][c] = v`, `m[k] = v`, an append), or a call's returned
  array or map is thrown away (`sortDesc(xs);`) → return it and reassign,
  `xs = f(xs)`; for scratch work, copy it to a local first.
- **[NARROW-001]** (warning) a `Real` stored in an `Int` target → the fraction
  is discarded; use a `Real` target, or `int(...)` when truncating is intended.
- **[PARAM-001]** (warning) a function changes a scalar parameter its caller
  expects to move (`pos = pos + 1` in a parser helper) → the parameter is a
  copy: return it with the result (`-> (Int, Int)`, `let (v, next) = f(s, pos);
  pos = next;`), or make the helper a nested `fn` that assigns the enclosing
  `pos`, or keep the cursor in a record field.
- **[FMT-001]** a placeholder in a `println` string — `"{}"`, `{name}`, `{0}`,
  `{:.2}`, `${}`, `%d` / `%.2f`, or an `f"..."` literal → `println` has no
  placeholders: pass the values as arguments (`println("n = ", n)`) and format
  decimals with `format_float(x, 2)`.
- **[DIV-002]** `x // 2` → `//` always starts a comment; integer division is
  `x div 2`.
- **[ENTRY-001]** `fn main` would not run — the file also has top-level
  statements — or there is nothing to run, or `main` takes parameters or
  returns `Text` → move the statements into `fn main() -> Void`.
- **[PAR-001]** the same record passed to more than one `par` branch (concurrent
  writes race) → give each branch its own record and combine after the block.
- **[PAR-002]** a non-call statement inside `par { ... }` (an assignment, `fx`,
  loop, …) → `par` bodies allow only direct call statements; wrap the work in a
  `fn` and call that inside `par`.

The compiler cannot check *output correctness*. If the program compiles but the
result is wrong, no code is printed — these are authoring rules: integer result
where decimals are expected → introduce a `Real` operand (`100.0`); unstable
decimal output → `value:0:precision`; wrong receiver spelling → `self`, never
`Self`; iterating an object root → extract the array with `toon_key` first
(ROOT-001); a fallback that hid which level was missing → guard the intermediate
node (NEST-001); exact-output mismatch → match spacing, casing, and precision
(FMT-001); Markdown fences in the answer → return raw Aether source only
(OUT-001); flattened or renamed JSON keys → copy them exactly (KEY-001).

### Removed spellings

These names exist in the shared engine for other front ends but are not Aether.
Each is `SCOPE-001` (the type `File` is `TYPE-002`) and the hint names the
replacement:

| Written | Write instead |
|---|---|
| `copy(s, a, n)` | `s[a..a + n]` (`s[a..]`, `s[..b]`) |
| `pos(needle, s)` | `index_of(s, needle)` — the text comes first; or `contains`, `starts_with`, `ends_with` |
| `stringofchar(c, n)` | `repeat(c, n)` |
| `val(t, n, code)` / `valreal(t, r, code)` | `let (n, ok) = try_parse_int(t);` / `try_parse_real(t)` |
| `itoa(n)`, `inttostr(n)` | `int_to_text(n)` |
| `let f: File;` with `assign` / `reset` / `rewrite` / `append` / `readln(f, l)` / `writeln(f, ...)` / `eof(f)` / `close` | `read_file(path)`, `read_lines(path)`, `write_file(path, t)`, `append_file(path, t)` |
| `erase(f)`, `rename(f, to)` | `delete_file(path)`, `rename_file(from, to)` |
| `readln(x)`, `read(x)`, `eof()` on stdin | `x = read_line();`, `read_all_stdin()` |
| `paramcount()`, `paramstr(i)` | `length(args())`, `args()[i - 1]` |
| `gettime(h, m, s, cs)`, `getdate(y, m, d, wd)` | `clock_ms()`, `now_text()` |
| `setlength(xs, n)` | `[0; n]` for a fresh array, `xs = xs[..n];` to shrink, `xs = xs + [v];` to grow |
| `mstreamcreate`, `mstreamfromstring`, `mstreambuffer`, `mstreamappendbyte`, `mstreamloadfromfile`, `mstreamsavetofile`, `mstreamfree` | `mstream_new`, `mstream_from_text`, `mstream_text`, `mstream_write_byte`, `mstream_load(path)`, `mstream_save`, `mstream_free` |
| `mstream_append_text`, `mstream_append_byte` | `mstream_write_text`, `mstream_write_byte` |
| run-together names: `fileexists`, `formatfloat`, `realtostr`, `getcurrentdir`, `getenv`, `getenvint`, `dnslookup`, and `httprequest`, `socketcreate`, `sqliteopen` and the rest of those families | snake_case (D72): `file_exists`, `format_float`, `real_to_text`, `get_current_dir`, `get_env`, `get_env_int`, `dns_lookup`, `http_request`, `socket_create`, `sqlite_open`, ... -- an underscore between the words |

A program may still declare its own function with one of these names
(`fn rename(...)`); calls then reach it.

## Validation Checklist

Before submitting Aether code, verify:

- all output, task helper, and `ai_chat` calls are inside `fx { ... }` (FX-001)
- every called helper appears in this document (BUILT-001)
- all `use "..."` imports reference real, verified modules; imported names are
  used exactly as exported (IMP-001, MOD-001)
- all function parameters have explicit types; uncertain types are annotated;
  array literals carry an explicit type (`let xs: Int[] = [1, 2, 3];`)
  (TYPE-001)
- `ret` not `return`; `type` not `class` (SYN-001)
- no field or method named after an Aether keyword or operator word (`new`,
  `for`, `loop`, `div`, `and`); no `fn new()`/`__init__` constructor method (SYN-001)
- no `let mut` in new code (MUT-001)
- no arithmetic on or cross-assignment of `ToonDoc` / `ToonNode`; every parsed
  doc is closed with `toon_close(doc)` (TOON-001)
- object-shaped payloads: named array extracted before iteration (ROOT-001);
  JSON keys copied exactly, dotted paths only through objects (KEY-001); a
  missing intermediate handled deliberately (NEST-001)
- `toon_len` for TOON arrays, `length` for dynamic arrays (LEN-001)
- real arithmetic has a `Real` operand where decimals matter; stable decimal
  output uses `value:width:precision`
- tuple returns destructured directly, or bound to a variable and read with
  `.0`/`.1`/`.2` — never chained directly onto the call (TUP-001)
- annotations sit above their functions (ANN-001)
- canonical forms used unless preserving accepted-but-non-canonical source

## Files and environment

Host access for automation. **All of these are effectful — call inside `fx`**
(FX-001), and they are rejected inside `@pure` functions.

Each file operation is one call on a path; there are no file handles to open
or close. When a file may be missing, use the checked `try_` forms, which
return `(value, ok)` like `try_parse_int`: `let (text, ok) = try_read_file(p);`
inside `fx`.

| Builtin | Signature |
|---|---|
| `read_file(path)` | `(Text) -> Text` — the whole file; a file that cannot be read is a run-time error |
| `read_lines(path)` | `(Text) -> Text[]` — its lines without `"\n"` / `"\r\n"`; a final line break does not add an empty line |
| `try_read_file(path)` | `(Text) -> (Text, Bool)` — checked: `(text, ok)`, with `ok` false and `text` `""` when the file cannot be read, never a run-time error |
| `try_read_lines(path)` | `(Text) -> (Text[], Bool)` — checked `read_lines`: `([], false)` when the file cannot be read |
| `try_toon_parse_file(path)` | `(Text) -> (ToonDoc, Bool)` — checked `toon_parse_file`: `ok` is false for a missing file or bad JSON, and the document is then JSON `null` (every accessor degrades; close it as usual) |
| `write_file(path, text)` | `(Text, Text) -> Void` — create or truncate, then write `text` as is |
| `append_file(path, text)` | `(Text, Text) -> Void` — add `text` at the end, creating the file |
| `file_exists(path)` | `(Text) -> Bool` (`file_exists` is accepted) |
| `delete_file(path)` | `(Text) -> Bool` — true when it was deleted |
| `rename_file(from, to)` | `(Text, Text) -> Bool` — true when it was renamed |
| `read_line()` | `() -> Text` — the next stdin line without its line break, `""` at the end of input |
| `read_all_stdin()` | `() -> Text` — the rest of stdin, each line ending in `"\n"` |
| `args()` | `() -> Text[]` — the command-line arguments after the program (`args()[0]` is the first) |
| `get_env(name)` | `(Text) -> Text` (empty if unset) |
| `get_env_int(name, fallback)` | `(Text, Int) -> Int` |
| `get_current_dir()` | `() -> Text` |
| `mkdir(path)` / `rmdir(path)` | `(Text) -> Int` (0 on success, else errno; `mkdir` of a directory that already exists is `0`) |

`write_file` and `append_file` write exactly the text given: end each line with
`"\n"` yourself. A blank line read from stdin is `""` too, so to process all of
stdin, read it at once — `split(read_all_stdin(), "\n")` ends with an empty
element when the input ends in a line break.

```aether
fn main() -> Void {
    fx {
        write_file("notes.txt", "first line\n");
        append_file("notes.txt", "second line\n");
        loop line in read_lines("notes.txt") {
            println(line);
        }
        println("bytes=", length(read_file("notes.txt")), " args=", length(args()));
        let gone: Bool = delete_file("notes.txt");
        println("exists=", file_exists("notes.txt"));
    }
    ret;
}
```

Database builtins (`sqlite_*`) are effectful too; they are outside the curated
surface, and their signatures are in the appendix. A SQL error in
`sqlite_exec` or `sqlite_prepare` (a syntax error, a missing table) stops the
program with the SQLite message. `sqlite_step` returns SQLite's result code
instead (`100` a row, `101` done, `19` a constraint failure), and
`sqlite_errmsg(db)` then reads the message.

## HTTP requests and memory streams (MS-001)

HTTP support needs a build with libcurl networking (`AETHER_ENABLE_CURL=ON`);
without it the `http_*` builtins fail at runtime. The response body arrives in
an `MStream` — an opaque memory-stream handle (never `Int`, never `Text`).

| Builtin | Signature | fx? |
|---|---|---|
| `http_session()` | `() -> Int` (session handle, `-1` on failure) | yes |
| `http_request(session, method, url, body, out)` | `(Int, Text, Text, Text, MStream) -> Int` (HTTP status; a negative error class on a transport error, below) | yes |
| `http_set_header(session, name, value)` | `(Int, Text, Text) -> Void` | yes |
| `http_close(session)` | `(Int) -> Void` | yes |
| `mstream_new()` | `() -> MStream` | no (pure memory) |
| `mstream_from_text(text)` | `(Text) -> MStream` | no |
| `mstream_text(ms)` | `(MStream) -> Text` (stream contents) | no |
| `mstream_write_text(ms, text)` | `(MStream, Text) -> Void` (writes at the end) | no |
| `mstream_write_byte(ms, byte)` | `(MStream, Int) -> Void` (writes one byte at the end, `byte & 0xFF`) | no |
| `mstream_length(ms)` | `(MStream) -> Int` (bytes) | no |
| `mstream_free(ms)` | `(MStream) -> Void` | no |
| `mstream_load(path)` | `(Text) -> MStream` — a new stream with the file's bytes (a file that cannot be read is a run-time error) | yes (file I/O) |
| `mstream_save(ms, path)` | `(MStream, Text) -> Void` | yes (file I/O) |

Golden shape — a GET request end to end (all `http_*` calls inside `fx`):

```aether
fn main() -> Void {
    fx {
        let session: Int = http_session();
        let out: MStream = mstream_new();
        let status: Int = http_request(session, "GET", "https://example.com/", "", out);
        if status == 200 {
            let body: Text = mstream_text(out);
            println(body);
        }
        mstream_free(out);
        http_close(session);
    }
    ret;
}
```

- the `out` argument must be an initialized `MStream` (`mstream_new()`);
  `http_request` clears it before writing the response into it
- `mstream_text(out)` is the way to get the body as `Text` — never
  `print(out)` and never `let body: Int = ...`
- `body` (arg 4) is the request payload — pass `""` for GET
- a JSON response pairs naturally with TOON: `toon_parse(mstream_text(out))`
- a transport error (no HTTP response at all) returns the negated error class,
  which `http_error_code(session)` also reads as a positive number, with the
  libcurl message in `http_last_error(session)`: `1` generic, `2` I/O, `3`
  timeout, `4` TLS, `5` name resolution, `6` connection (`-6` for a refused
  connection, `-5` for an unknown host)
- an HTTP error status is still a status: a 404 or a 503 comes back as `404` or
  `503` with the response body in `out`. For a 5xx, `http_error_code` is `1`
  and `http_last_error` reads `"HTTP status 503"`
- `MStream` handles are opaque: no arithmetic, no cross-assignment with
  `ToonDoc`/`ToonNode`/`Int` (MS-001)

## Sockets

A real, working BSD-socket-style API — not the `tcpsocket*`/`udpsocket*`/
`resolve` names a model might guess by analogy with other languages; those do
not exist. All `socket_*` calls and `dns_lookup` are effectful (FX-001, inside
`fx`). `socket_receive` returns its data in an `MStream` (MS-001), same as
`http_*` — never `Int`, never `Text` directly.

| Builtin | Signature | Notes |
|---|---|---|
| `socket_create(type[, family])` | `(Int, Int?) -> Int` (socket handle, `-1` on failure) | `type`: `0` = TCP (`SOCK_STREAM`), `1` = UDP (`SOCK_DGRAM`), any other type is `-1`; `family`: `4` = IPv4 (default), `6` = IPv6 |
| `socket_connect(socket, host, port)` | `(Int, Text, Int) -> Int` (`0`/`-1`) | resolves `host` (hostname or literal address) and connects |
| `socket_bind(socket, port)` | `(Int, Int) -> Int` (`0`/`-1`) | binds on all local interfaces (`INADDR_ANY`) |
| `socket_bind_addr(socket, host, port)` | `(Int, Text, Int) -> Int` (`0`/`-1`) | binds to one specific local address |
| `socket_listen(socket, backlog)` | `(Int, Int) -> Int` (`0`/`-1`) | |
| `socket_accept(socket)` | `(Int) -> Int` (new connected-socket handle, `-1` on failure) | **blocks** until a peer connects |
| `socket_peer_addr(socket)` | `(Int) -> Text` | remote IP of a connected socket |
| `socket_send(socket, data)` | `(Int, Text\|MStream) -> Int` (bytes sent, `-1` on failure) | `data` may be `Text` or an `MStream` |
| `socket_receive(socket, maxlen)` | `(Int, Int) -> MStream` | **blocks** until data arrives (or the peer closes — a `0`-length result then means EOF, not failure); use `mstream_text(...)` to read it |
| `socket_set_blocking(socket, blocking)` | `(Int, Bool) -> Void` | flips a socket to non-blocking mode |
| `socket_poll(socket, timeoutMs, flags)` | `(Int, Int, Int) -> Int` | `flags`: `1` = check readable, `2` = check writable (bitwise-or both); returns `0` on timeout, else a bitmask of what's ready |
| `socket_close(socket)` | `(Int) -> Void` | |
| `socket_last_error()` | `() -> Int` | last socket error code (errno/WSA), `0` if the last call succeeded |
| `dns_lookup(hostname)` | `(Text) -> Text` | forward lookup only (no reverse lookup); returns `""` on failure |

The gotcha every generated socket example must handle: `socket_accept` and
`socket_receive` **block the calling task indefinitely** until a peer shows up
— call either one with no counterpart running concurrently and the program
hangs forever. Two ways to avoid that:

1. **Same-process client + server via `par`** (recommended for a
   self-contained demo). Create, bind, and put the listening socket into
   listen state *before* the `par` block — not inside one of its branches —
   so the client branch can never race ahead of a not-yet-listening server;
   `par` already joins (waits for both) before continuing (see
   **Concurrency**).
2. **Non-blocking + `socket_poll`.** Call `socket_set_blocking(socket, false)`,
   then loop on `socket_poll(socket, timeoutMs, 1)` until it reports readable
   (or a deadline passes) before calling `socket_accept`/`socket_receive`.

Golden shape — a same-process TCP echo, listener set up before `par`, verified
to run to completion in well under a second:

```aether
type Message {
    payload: Text;
}

fn server(listenSock: Int, out: Message) -> Void {
    fx {
        let conn: Int = socket_accept(listenSock);
        let ms: MStream = socket_receive(conn, 256);
        out.payload = mstream_text(ms);
        println("server got: ", out.payload);
        socket_send(conn, "pong");
        mstream_free(ms);
        socket_close(conn);
        socket_close(listenSock);
    }
    ret;
}

fn client(port: Int, out: Message) -> Void {
    fx {
        let sock: Int = socket_create(0);
        socket_connect(sock, "127.0.0.1", port);
        socket_send(sock, "ping");
        let ms: MStream = socket_receive(sock, 256);
        out.payload = mstream_text(ms);
        println("client got: ", out.payload);
        mstream_free(ms);
        socket_close(sock);
    }
    ret;
}

fn main() -> Void {
    let port: Int = 23557;
    let listenSock: Int = 0;
    let serverMsg: Message = new Message();
    let clientMsg: Message = new Message();
    fx {
        listenSock = socket_create(0);
        socket_bind(listenSock, port);
        socket_listen(listenSock, 1);
    }
    par {
        server(listenSock, serverMsg);
        client(port, clientMsg);
    }
    fx { println("done"); }
    ret;
}
```

---

## Appendix: complete builtin inventory

<!-- BEGIN GENERATED BUILTIN INVENTORY -->

*Generated by `tools/gen_builtin_appendix.py`. Do not edit by hand.*

Every builtin this compiler exposes: **179** with a signature below, **41** whose signatures are in the tables earlier in this document, and **153** more by name only. If a helper appears in none of the three, it does not exist — inline the logic instead of reaching for it.

`fx` marks a builtin as effectful: it must be called inside an `fx { ... }` block and may not be called from a `@pure` function (FX-001).

### Documented signatures

Safe to call exactly as written.

**TOON (structured data)**

| Signature | Effect |
|---|---|
| `toon_at(node: ToonNode, index: Int) -> ToonNode` | pure |
| `toon_bool_value(node: ToonNode) -> Bool` | pure |
| `toon_close(doc: ToonDoc) -> Void` | pure |
| `toon_free(node: ToonNode) -> Void` | pure |
| `toon_get_bool(node: ToonNode, key: Text) -> Bool` | pure |
| `toon_get_bool_or(node: ToonNode, key: Text, fallback: Bool) -> Bool` | pure |
| `toon_get_int(node: ToonNode, key: Text) -> Int` | pure |
| `toon_get_int_or(node: ToonNode, key: Text, fallback: Int) -> Int` | pure |
| `toon_get_real(node: ToonNode, key: Text) -> Real` | pure |
| `toon_get_real_or(node: ToonNode, key: Text, fallback: Real) -> Real` | pure |
| `toon_get_text(node: ToonNode, key: Text) -> Text` | pure |
| `toon_get_text_or(node: ToonNode, key: Text, fallback: Text) -> Text` | pure |
| `toon_has_at(node: ToonNode, index: Int) -> Bool` | pure |
| `toon_has_key(node: ToonNode, key: Text) -> Bool` | pure |
| `toon_int_value(node: ToonNode) -> Int` | pure |
| `toon_is_arr(node: ToonNode) -> Bool` | pure |
| `toon_is_bool(node: ToonNode) -> Bool` | pure |
| `toon_is_int(node: ToonNode) -> Bool` | pure |
| `toon_is_null(node: ToonNode) -> Bool` | pure |
| `toon_is_obj(node: ToonNode) -> Bool` | pure |
| `toon_is_real(node: ToonNode) -> Bool` | pure |
| `toon_is_text(node: ToonNode) -> Bool` | pure |
| `toon_key(node: ToonNode, key: Text) -> ToonNode` | pure |
| `toon_key_or(node: ToonNode, key: Text, fallback: ToonNode) -> ToonNode` | pure |
| `toon_len(node: ToonNode) -> Int` | pure |
| `toon_null() -> ToonNode` | pure |
| `toon_null_value(node: ToonNode) -> Bool` | pure |
| `toon_parse(payload: Text) -> ToonDoc` | pure |
| `toon_parse_file(path: Text) -> ToonDoc` | `fx` |
| `toon_real_value(node: ToonNode) -> Real` | pure |
| `toon_root(doc: ToonDoc) -> ToonNode` | pure |
| `toon_text_value(node: ToonNode) -> Text` | pure |
| `toon_type(node: ToonNode) -> Text` | pure |

**AI**

| Signature | Effect |
|---|---|
| `ai_chat(model: Text, messages: Text, system: Text = "", apiKey: Text = "", endpoint: Text = "") -> Text` | `fx` |

**Capability probes**

| Signature | Effect |
|---|---|
| `aether_version() -> Text` | pure |
| `builtin_info(name: Text) -> Text` | pure |
| `builtins_json(detail: Bool = false) -> Text` | pure |
| `has_ai() -> Bool` | pure |
| `has_builtin(category: Text, function: Text) -> Bool` | pure |
| `has_toon() -> Bool` | pure |

**Collections**

| Signature | Effect |
|---|---|
| `count(xs: T[], v: T) -> Int; count(s: Text, x: Text) -> Int` | pure |
| `get(m: Map[Text, T], k: Text, default: T) -> T` | pure |
| `has(m: Map[Text, T], k: Text) -> Bool` | pure |
| `keys(m: Map[Text, T]) -> Text[]` | pure |
| `len(arrayOrText) -> Int` | pure |
| `max_of(xs: T[]) -> T` | pure |
| `min_of(xs: T[]) -> T` | pure |
| `remove(m: Map[Text, T], k: Text) -> Void` | pure |
| `reversed(xs: T[]) -> T[]; reversed(s: Text) -> Text` | pure |
| `same(a: T, b: T) -> Bool` | pure |
| `sorted(xs: T[]) -> T[]` | pure |
| `sorted_by(xs: R[], "field") -> R[]` | pure |
| `sorted_by_desc(xs: R[], "field") -> R[]` | pure |
| `sorted_desc(xs: T[]) -> T[]` | pure |
| `sum(xs: Int[]) -> Int; sum(xs: Real[]) -> Real` | pure |
| `values(m: Map[Text, T]) -> T[]` | pure |

**Conversion**

| Signature | Effect |
|---|---|
| `format_float(value: Real, precision: Int) -> Text` | pure |
| `int_to_text(n: Int) -> Text` | pure |
| `real_to_text(value: Real) -> Text` | pure |
| `round_to(x: Real, digits: Int) -> Real` | pure |
| `try_parse_int(t: Text) -> (Int, Bool)` | pure |
| `try_parse_real(t: Text) -> (Real, Bool)` | pure |

**Filesystem**

| Signature | Effect |
|---|---|
| `append_file(path: Text, text: Text) -> Void` | `fx` |
| `delete_file(path: Text) -> Bool` | `fx` |
| `file_exists(path: Text) -> Bool` | `fx` |
| `get_current_dir() -> Text` | `fx` |
| `mkdir(path: Text) -> Int` | `fx` |
| `mstream_load(path: Text) -> MStream` | `fx` |
| `mstream_save(ms: MStream, path: Text) -> Void` | `fx` |
| `read_file(path: Text) -> Text` | `fx` |
| `read_lines(path: Text) -> Text[]` | `fx` |
| `rename_file(from: Text, to: Text) -> Bool` | `fx` |
| `rmdir(path: Text) -> Int` | `fx` |
| `try_read_file(path: Text) -> (Text, Bool)` | `fx` |
| `try_read_lines(path: Text) -> (Text[], Bool)` | `fx` |
| `try_toon_parse_file(path: Text) -> (ToonDoc, Bool)` | `fx` |
| `write_file(path: Text, text: Text) -> Void` | `fx` |

**Input / output**

| Signature | Effect |
|---|---|
| `print(args...) -> Void` | `fx` |
| `println(args...) -> Void` | `fx` |
| `read_all_stdin() -> Text` | `fx` |
| `read_line() -> Text` | `fx` |

**Network and HTTP**

| Signature | Effect |
|---|---|
| `dns_lookup(hostname: Text) -> Text` | `fx` |
| `http_await(id: Int, out: MStream) -> Int` | `fx` |
| `http_cancel(id: Int) -> Void` | `fx` |
| `http_clear_headers(session: Int) -> Void` | `fx` |
| `http_close(session: Int) -> Void` | `fx` |
| `http_error_code(session: Int) -> Int` | `fx` |
| `http_get_async_progress(id: Int) -> Int` | `fx` |
| `http_get_async_total(id: Int) -> Int` | `fx` |
| `http_get_header(session: Int, name: Text) -> Text` | `fx` |
| `http_get_last_headers(session: Int) -> Text` | `fx` |
| `http_is_done(id: Int) -> Bool` | `fx` |
| `http_last_error(session: Int) -> Text` | `fx` |
| `http_request(session: Int, method: Text, url: Text, body: Text, out: MStream) -> Int` | `fx` |
| `http_request_async(session: Int, method: Text, url: Text, body: Text) -> Int` | `fx` |
| `http_request_async_to_file(session: Int, method: Text, url: Text, body: Text, out: Text) -> Int` | `fx` |
| `http_request_to_file(session: Int, method: Text, url: Text, body: Text, out: Text) -> Int` | `fx` |
| `http_session() -> Int` | `fx` |
| `http_set_header(session: Int, name: Text, value: Text) -> Void` | `fx` |
| `http_set_option(session: Int, key: Text, value: Int) -> Void; http_set_option(session: Int, key: Text, value: Text) -> Void` | `fx` |
| `http_try_await(id: Int, out: MStream) -> Int` | `fx` |
| `socket_accept(socket: Int) -> Int` | `fx` |
| `socket_bind(socket: Int, port: Int) -> Int` | `fx` |
| `socket_bind_addr(socket: Int, host: Text, port: Int) -> Int` | `fx` |
| `socket_close(socket: Int) -> Void` | `fx` |
| `socket_connect(socket: Int, host: Text, port: Int) -> Int` | `fx` |
| `socket_create(type: Int, family: Int = 4) -> Int` | `fx` |
| `socket_last_error() -> Int` | `fx` |
| `socket_listen(socket: Int, backlog: Int) -> Int` | `fx` |
| `socket_peer_addr(socket: Int) -> Text` | `fx` |
| `socket_poll(socket: Int, timeoutMs: Int, flags: Int) -> Int` | `fx` |
| `socket_receive(socket: Int, maxlen: Int) -> MStream` | `fx` |
| `socket_send(socket: Int, data: Text) -> Int; socket_send(socket: Int, data: MStream) -> Int` | `fx` |
| `socket_set_blocking(socket: Int, blocking: Bool) -> Void` | `fx` |

**SQLite**

| Signature | Effect |
|---|---|
| `sqlite_bind_double(stmt: Int, index: Int, value: Real) -> Int` | `fx` |
| `sqlite_bind_int(stmt: Int, index: Int, value: Int) -> Int` | `fx` |
| `sqlite_bind_null(stmt: Int, index: Int) -> Int` | `fx` |
| `sqlite_bind_text(stmt: Int, index: Int, value: Text) -> Int` | `fx` |
| `sqlite_changes(db: Int) -> Int` | `fx` |
| `sqlite_clear_bindings(stmt: Int) -> Int` | `fx` |
| `sqlite_close(db: Int) -> Int` | `fx` |
| `sqlite_column_count(stmt: Int) -> Int` | `fx` |
| `sqlite_column_double(stmt: Int, column: Int) -> Real` | `fx` |
| `sqlite_column_int(stmt: Int, column: Int) -> Int` | `fx` |
| `sqlite_column_name(stmt: Int, column: Int) -> Text` | `fx` |
| `sqlite_column_text(stmt: Int, column: Int) -> Text` | `fx` |
| `sqlite_column_type(stmt: Int, column: Int) -> Text` | `fx` |
| `sqlite_errmsg(db: Int) -> Text` | `fx` |
| `sqlite_exec(db: Int, sql: Text) -> Int` | `fx` |
| `sqlite_finalize(stmt: Int) -> Int` | `fx` |
| `sqlite_last_insert_rowid(db: Int) -> Int` | `fx` |
| `sqlite_open(path: Text) -> Int` | `fx` |
| `sqlite_prepare(db: Int, sql: Text) -> Int` | `fx` |
| `sqlite_reset(stmt: Int) -> Int` | `fx` |
| `sqlite_step(stmt: Int) -> Int` | `fx` |

**Strings**

| Signature | Effect |
|---|---|
| `contains(s: Text, x: Text) -> Bool; contains(xs: T[], v: T) -> Bool` | pure |
| `ends_with(s: Text, suffix: Text) -> Bool` | pure |
| `index_of(s: Text, x: Text) -> Int; index_of(xs: T[], v: T) -> Int` | pure |
| `is_alnum(t: Text) -> Bool` | pure |
| `is_alpha(t: Text) -> Bool` | pure |
| `is_digit(t: Text) -> Bool` | pure |
| `is_lower(t: Text) -> Bool` | pure |
| `is_space(t: Text) -> Bool` | pure |
| `is_upper(t: Text) -> Bool` | pure |
| `join(xs: Text[], sep: Text) -> Text` | pure |
| `lower(s: Text) -> Text` | pure |
| `mstream_free(ms: MStream) -> Void` | pure |
| `mstream_from_text(t: Text) -> MStream` | pure |
| `mstream_length(ms: MStream) -> Int` | pure |
| `mstream_new() -> MStream` | pure |
| `mstream_text(ms: MStream) -> Text` | pure |
| `mstream_write_byte(ms: MStream, b: Int) -> Void` | pure |
| `mstream_write_text(ms: MStream, t: Text) -> Void` | pure |
| `repeat(s: Text, n: Int) -> Text` | pure |
| `replace(s: Text, old: Text, new: Text) -> Text` | pure |
| `starts_with(s: Text, prefix: Text) -> Bool` | pure |
| `trim(s: Text) -> Text` | pure |
| `upper(s: Text) -> Text` | pure |

**System and process**

| Signature | Effect |
|---|---|
| `args() -> Text[]` | `fx` |
| `get_env(name: Text) -> Text` | `fx` |
| `get_env_int(name: Text, fallback: Int) -> Int` | `fx` |
| `random() -> Real in [0,1); random(n: Int) -> Int in [0,n)` | `fx` |
| `randomize() -> Void` | `fx` |

**Tasks and concurrency**

| Signature | Effect |
|---|---|
| `task_cancel(handle: Int) -> Bool` | `fx` |
| `task_lookup(name: Text) -> Int` | `fx` |
| `task_pause(handle: Int) -> Bool` | `fx` |
| `task_queue(target: Text, name: Text, arg) -> Int` | `fx` |
| `task_result(handle: Int, consume: Bool = false) -> T (the target's result: Text for "dns_lookup")` | `fx` |
| `task_resume(handle: Int) -> Bool` | `fx` |
| `task_set_name(handle: Int, name: Text) -> Bool` | `fx` |
| `task_spawn(target: Text, name: Text, arg) -> Int` | `fx` |
| `task_stats() -> Array` | `fx` |
| `task_stats_json() -> Text` | `fx` |
| `task_status(handle: Int, consume: Bool = false) -> Bool` | `fx` |
| `task_wait(handle: Int) -> Int` | `fx` |

**Text**

| Signature | Effect |
|---|---|
| `string_len(text: Text) -> Int` | pure |

**Clock**

| Signature | Effect |
|---|---|
| `clock_ms() -> Int` | `fx` |
| `now_text() -> Text` | `fx` |
| `sleep(ms: Int) -> Void` | `fx` |

### Documented earlier in this document

Real, and their signatures **are** documented — in the conversion, arity, math, and helper tables above, not in the table below. They are listed here only so that "absent from the appendix" still means "does not exist". Scroll up for the signature; do not treat these as unknown and do not hand-roll a replacement.

**Core** — `abs`, `arccos`, `arcsin`, `arctan`, `atan2`, `bool`, `ceil`, `chr`, `clamp`, `cos`, `cosh`, `cotan`, `exp`, `floor`, `int`, `length`, `ln`, `log10`, `max`, `min`, `odd`, `ord`, `pow`, `power`, `real`, `round`, `sin`, `sinh`, `sqr`, `sqrt`, `tan`, `tanh`, `trunc`, `upcase`

**math** — `factorial`, `fibonacci`

**Strings** — `parse_bool`, `parse_float`, `parse_int`, `split`

**System and process** — `realtimeclock` *fx*

### Name only

These names exist, but their signatures are not documented here. That is deliberate and it is still useful: it confirms a name is real **without** licensing a guess at its arguments. Calling one with an invented argument list is how `BUILT-002` and uncoded runtime errors happen. If you need one of these and cannot verify its signature by running the compiler, restructure to use a documented helper above.

**Core** — `apireceive` *fx*, `apisend` *fx*, `beep` *fx*, `biblinktext`, `biboldtext`, `biclrscr`, `bilowvideo`, `binormvideo`, `biunderlinetext`, `biwherex`, `biwherey`, `blinktext`, `blockread` *fx*, `blockwrite` *fx*, `boldtext`, `byte`, `bytecodeversion`, `channelclose`, `channelcreate`, `channelisclosed`, `channelreceive`, `channelsend`, `channeltryreceive`, `channeltrysend`, `char`, `clreol`, `clrscr`, `cursoroff`, `cursoron`, `dec`, `deline`, `dispose`, `dosexec` *fx*, `dosfindfirst` *fx*, `dosfindnext` *fx*, `dosgetdate` *fx*, `dosgetenv` *fx*, `dosgetfattr` *fx*, `dosgettime` *fx*, `dosmkdir` *fx*, `dosrmdir` *fx*, `double`, `exec` *fx*, `exit` *fx*, `extbuiltincategorycount`, `extbuiltincategoryname`, `extbuiltinfunctioncount`, `extbuiltinfunctionname`, `extbuiltingroupcount`, `extbuiltingroupfunctioncount`, `extbuiltingroupfunctionname`, `extbuiltingroupname`, `fclose` *fx*, `fflush` *fx*, `filesize` *fx*, `findfirst` *fx*, `findnext` *fx*, `float`, `flush` *fx*, `fopen` *fx*, `format`, `fprintf` *fx*, `getfattr` *fx*, `gotoxy`, `halt` *fx*, `hidecursor`, `high`, `highvideo`, `inc`, `insline`, `invertcolors`, `ioresult` *fx*, `jsonget`, `keypressed` *fx*, `landscapesetlightingpreset`, `landscapesetpalettepreset`, `low`, `lowvideo`, `new`, `newobj`, `normalcolors`, `normvideo`, `pollkeyany`, `popscreen`, `printf` *fx*, `pushscreen`, `quitrequested`, `readkey` *fx*, `restorecursor`, `savecursor`, `screencols`, `screenrows`, `showcursor`, `sizeof`, `str`, `succ`, `taskawait` *fx*, `taskcancel` *fx*, `taskdone` *fx*, `taskspawn` *fx*, `textbackground`, `textbackgrounde`, `textcolor`, `textcolore`, `threadcancel` *fx*, `threadgetresult` *fx*, `threadgetstatus` *fx*, `threadlookup` *fx*, `threadpause` *fx*, `threadpoolsubmit` *fx*, `threadresume` *fx*, `threadsetname` *fx*, `threadspawnbuiltin` *fx*, `threadstats` *fx*, `threadstatsjson` *fx*, `tobool`, `tobyte`, `tochar`, `todouble`, `tofloat`, `toint`, `toupper`, `underlinetext`, `vmversion`, `waitforthread` *fx*, `wherex`, `wherey`, `window`

**math** — `chudnovsky`, `mandelbrotrow`

**Strings** — `atoi`

**System and process** — `getpid` *fx*, `swap` *fx*

**yyjson** — `yyjsondocfree`, `yyjsonfreevalue`, `yyjsongetbool`, `yyjsongetboolor`, `yyjsongetindex`, `yyjsongetint`, `yyjsongetintor`, `yyjsongetkey`, `yyjsongetlength`, `yyjsongetnumber`, `yyjsongetnumberor`, `yyjsongetroot`, `yyjsongetstring`, `yyjsongetstringor`, `yyjsongettype`, `yyjsonhasindex`, `yyjsonhaskey`, `yyjsonisnull`, `yyjsonread`, `yyjsonreadfile` *fx*

<!-- END GENERATED BUILTIN INVENTORY -->
