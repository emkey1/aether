# Aether for Humans and LLMs

*Guide version: 2026-06-28-3*
Aether is a compact front end for the PSCAL suite. It targets the existing
shared PSCAL backend, bytecode compiler, and VM. It is not a separate runtime.

If you only read one part of this document, read **Highest-Value Rules** and
**Never Generate These**.

## Rule labels

- **Hard rule**: violating this usually produces invalid Aether or wrong behavior.
- **Canonical**: preferred style for new Aether, especially LLM-generated Aether.
- **Accepted**: may compile, but generate only when preserving existing code.
- **Avoid**: likely wrong, fragile, or misleading even if it sometimes works.
- **LLM-critical**: high-value rule mirrored in the small-context guide.

## Highest-Value Rules

**LLM-critical.** These determine whether generated Aether works on the first try.

1. **FX-001.** Every effectful builtin must be inside `fx { ... }`: output
   (`print`/`println`), `ai_chat`, task/thread helpers, and all host
   interaction — filesystem (`mkdir`, `fileexists`, file I/O), environment
   (`getenv`), CLI (`paramstr`), `random`/`randomize`, the clock (`gettime`),
   console input (`readkey`), networking (`http*`/`socket*`), and database
   (`sqlite*`). Pure builtins (math, string, conversion) do not need `fx`.
2. **SYN-001.** Use Aether keywords: `fn`, `let`, `const`, `ret`, `if`, `loop`,
   `type`, `mod`, `use`. Do not import syntax from Python, Rust, JavaScript,
   Go, or Pascal.
3. **TYPE-001.** Prefer explicit types when inference is not obviously safe.
4. **MUT-001.** `let` bindings are already mutable. Generate plain `let`;
   never `let mut` in new code.
5. **ANN-001.** Put `@pure`, `@pre`, `@post`, `@cost` directly above the
   function they decorate, never inside the body.
6. **TOON-001.** `ToonDoc` and `ToonNode` are opaque handle types, not
   integers or records. Never mix them or do arithmetic on them.
7. **IMP-001.** Use verified modules only. Never invent imports such as
   `use "helpers";`.
8. **ORDER-001.** Define types and helper functions before `main` uses them.
9. **LEN-001.** `toon_len(node)` for TOON arrays; `length(arrayValue)` for
   dynamic arrays.
10. **TUP-001.** Tuples are narrow: destructure direct top-level helper
    returns (`let (a, b) = pair();`) only. Assume nothing else.
11. **OUT-001.** When asked to generate code, return raw Aether source only.
    No Markdown fences.
12. **BUILT-001.** The builtins listed here are the supported, recommended
    surface. Do not invent helpers (`substring`, `to_upper`, `replace`, ...) and
    do not guess names. More builtins exist and are discoverable via
    `builtins_json()` / `builtin_info(...)`, but if one is not listed here,
    discover its exact name and signature before calling rather than assuming.
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
18. **FLOW-001.** Every non-`Void` function must return a value on every
    reachable top-level path.
19. **FUNC-001.** Functions are not values. Aether has no anonymous functions,
    no inline `fn(...) -> T { ... }` literals, no lambdas, and no closures.
    Never pass a function as an argument. `task_spawn` / `task_queue` take a
    builtin *name* as `Text` (`task_spawn("delay", "worker", 5)`), not a
    function. There is no `map` / `filter` / `reduce`; transform with a `loop`.
    To run your own code concurrently, call your functions inside a
    `par { ... }` block (see **Concurrency**), not `task_spawn`.

Fast failure checks: output outside `fx` is wrong; a guessed import is wrong;
a helper not listed in this document is wrong; arithmetic on `ToonDoc` /
`ToonNode` is wrong; `return` is wrong; contracts inside a body are wrong;
when unsure about a type, add it explicitly.

## Never Generate These

**LLM-critical.** Common LLM failure modes. Never generate:

- `return`; use `ret` (SYN-001)
- `class`; use `type` (SYN-001)
- `for`, `while`, `var`, `func`, `def`, `=>`, Python-style colons (SYN-001)
- `print` / `println` / task helpers / `ai_chat` outside `fx { ... }` (FX-001)
- invented imports such as `use "helpers";` (IMP-001)
- `use module_name;` or `export { ... }` in new code; use canonical
  `use "module_name";` and `mod Name { export ... }`
- invented helper functions not listed in this document (BUILT-001)
- anonymous functions, inline `fn(...) -> T { ... }` literals, lambdas, or
  passing a function as a value; `task_spawn` / `task_queue` take a builtin
  *name* as `Text`, and there is no `map` / `filter` / `reduce` (FUNC-001)
- names that were never declared, or locals reused outside their scope
  (SCOPE-001)
- arithmetic on `ToonDoc` or `ToonNode`, or assigning one where the other is
  expected (TOON-001)
- annotations inside a function body (ANN-001)
- a tuple-return call bound to one variable: `let value = pair();` (TUP-001)
- mixed-type output that guesses `+` will stringify numbers
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
| Return | `ret value;` (`ret;` for Void) | — | `return value;` |
| Output | variadic `println(a, b)` in `fx` | text-only `+` concatenation | `Text + Int` guessing |
| Text equality | `a == b` | `string_eq(a, b)` | inventing `.equals(...)` |
| Text length | `string_len(t)` | `t.len` | inventing `strlen(...)` |
| Text → Int | `parse_int(t)` | — | inventing `Int(t)` / `t.toInt()` |
| Text → Real | `parse_float(t)` | — | inventing `Real(t)` |
| Text → Bool | `parse_bool(t)` | — | comparing to `"true"` by hand |
| Int → Text | `itoa(n)` | `int_to_text(n)` | inventing `n.toString()` |
| Real → Text | `formatfloat(r, prec)` | `realtostr(r)` (always 6 dp) | `r:0:prec` as a value (it is `println`-only) |
| Split text | `split(t, sep)` → `Text[]` | — | manual character scanning |
| Dynamic array length | `length(xs)` | `len(xs)`, `xs.len` | `toon_len(xs)` |
| TOON array length | `toon_len(node)` | — | `length(node)` |
| Method call | `counter.bump()` | `bump(counter)` if receiver obvious | class syntax |
| Concurrency target | `task_spawn("delay", "w", n)` (builtin name) | — | passing a closure / `fn(){...}` |
| Transform a list | explicit `loop` building `xs + [v]` | — | `map(...)` / `filter(...)` with a lambda |
| Record init | `Point { x: 3, y: 4 }` | `Point(x: 3, y: 4)` | — |
| TOON nested lookup | bind and validate intermediates | nested calls when shape guaranteed | `_or` on missing intermediate |
| Imports | verified `use "module";` | self-contained code | guessed modules |
| Method context | pass loop/local context as params | helper computes its own label | implicit capture of outer `i` / `name` |
| Field/local same name | `let valid = ...; if valid { self.valid = ...; }` | distinct local names | assuming bare `valid` means the field |

## What Aether is for

Small-to-medium automation programs that parse structured payloads, extract
typed fields, classify or transform data, and print or store results inside
`fx` — with visible effect boundaries and lightweight contracts, lowering onto
the shared PSCAL toolchain. It is not a separate runtime, not a dynamic
scripting language, and not a place to import imagined libraries.

## Smallest useful program

```aether
fn main() -> Void {
    fx {
        println("Hello from Aether");
    }
    ret;
}
```

## Safe generation algorithm for LLMs

**LLM-critical.** Generate in this order:

1. Decide whether the program needs TOON.
2. Define `type` blocks before functions that instantiate them.
3. Define helper functions before callers.
4. Give every function parameter an explicit type.
5. Prefer explicit `let name: Type = ...` unless the initializer is trivial.
6. Put all output, task helpers, and AI calls inside `fx`.
7. Use `ret`, never `return`.
8. Close every parsed `ToonDoc` with `toon_close(doc)`.
9. Run the Validation Checklist before final output.

## Lexical basics

- **Comments**: `// line comment` is canonical. `/* block comment */` is
  accepted, but generated code should prefer `//`.
- **Text literals**: double-quoted; escape embedded quotes as `\"`. Keep
  generated strings simple and avoid escape-heavy text unless the prompt
  explicitly requires it.
- **Statements** end with `;`. Blocks use `{ }`.

## Primitive types

| Type | Literals | Notes |
|---|---|---|
| `Int` | `42`, `-1` | integer arithmetic |
| `Real` | `3.5` | use a `Real` operand to force real division |
| `Text` | `"hi"` | string type |
| `Bool` | `true`, `false` | `println(flag)` prints `true` or `false` |
| `Void` | — | return type for procedures |
| `ToonDoc`, `ToonNode` | — | opaque handles (TOON-001) |

For mixed output, prefer variadic `println(...)` / `print(...)` rather than
guessing conversion helpers or `Text + Int` coercions.

## Operators

| Category | Operators |
|---|---|
| Arithmetic | `+ - * /` on `Int`/`Real`; `Int / Int` is integer-style division |
| Modulo | `%` (example: `7 % 3` = `1`) |
| Comparison | `== != < <= > >=` |
| Logical | `!` for negation; `&&` and `||` for conjunction/disjunction |
| Text | `+` concatenation (text-compatible operands only); `==` equality |
| Array | `xs + [v]` append only; do not assume `xs + ys` concatenates arrays |

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
- `ret` is not legal inside an `fx` block; return from the surrounding
  function outside the effect block

```aether
fn add(a: Int, b: Int) -> Int {
    ret a + b;
}
```

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
no binding to a single name; no tuple-return methods. `@post` is allowed on
tuple-return helpers, but only with positional result slots:

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
const Limit: Int = 42;
let count: Int = 0;
let label: Text = "Aether";
```

`let` bindings are mutable (MUT-001). `let mut` is accepted, redundant, and
ignored; never generate it.

### Inference policy (TYPE-001)

Omit the type only for these initializers:

- literals: `42`, `3.5`, `"text"`, `true`
- `new Type()`
- calls to functions with known declared return types, including methods on
  known typed bindings and the TOON/string helpers in this document

Everything else gets an explicit type — especially TOON extractions with
non-trivial shape, branchy results, and arithmetic such as `base + 1` where
the operand types are not visible at a glance. When in doubt, annotate.

Canonical loop-local example:

```aether
loop i in 0..5 {
    let square: Int = i * i;
    fx {
        println(i, " => ", square);
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
}
```

- use `type`, never `class`; fields are `name: Type;` (semicolons)
- inside methods use lowercase `self`, never `Self`
- top-level helpers whose first parameter is `self: Type` are extension-style
  methods; call them with method syntax (`counter.bump()`)
- methods do not implicitly capture outer loop locals or helper locals from
  another scope; pass needed context in the parameter list
- if a local and a field share a name, bare `name` means the local and
  `self.name` means the field
- field names must exist exactly as declared on the type; do not invent
  `self.blockers` if the type has no `blockers` field

Record values are pointer-backed: passing a record to a function or method
and mutating its fields is visible to the caller.

### Construction

```aether
let counter = new Counter();        // zeroed defaults
let point: Point = Point { x: 3, y: 4 };  // canonical field init
```

`new Type()` defaults: integers `0`, reals `0.0`, booleans `false`, text empty,
pointers `nil`, arrays empty-initialized. `Type(x: 3, y: 4)` is accepted but
non-canonical. Generated code should not otherwise use pointers or `nil`;
they are backend details.

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

Inline conditional expressions are allowed on the right-hand side of
declarations, assignments, and returns — but never directly inside
`println(...)` argument lists; bind first:

```aether
let label: Text = if ready { "ready" } else { "blocked" };
fx { println(label); }
```

## Loops

```aether
loop index < total {        // condition loop
    index = index + 1;
}

loop i in 0..count {        // half-open range; i is Int
    fx { println(i); }
}

loop {                      // infinite
    break;
}
```

`break` exits the nearest loop. `continue` is supported.

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

## Printing and formatting

`print` / `println` are the Aether output builtins. For mixed types, use
variadic arguments — never guess that `+` stringifies:

```aether
fx {
    println("Drop ", j, " -> ID: ", tx.id, " | Amt: ", tx.amount);
    println("pi ~= ", 3.14159:0:2);   // width:precision => 3.14
}
```

`println(realValue)` uses the backend default (6 decimals). For stable decimal
output use `value:width:precision`; width `0` means "precision only". The
`value:width:precision` spec works **only inside `print`/`println` arguments**.
To capture a formatted Real as `Text` (to return from a helper or concatenate),
use `formatfloat(value, precision)` or `realtostr(value)` (always 6 decimals).

| Use case | Syntax | Output |
|---|---|---|
| percentage | `value:0:2` | `95.50` |
| right-aligned | `value:10:2` | `     95.50` |
| default | `value` | `3.141593` |

Real division: `Int / Int` is integer-style; introduce a `Real` operand to
force a real result:

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

`abs`, `min`, `max`, `clamp` preserve their operand type (`Int` in → `Int` out);
`round`/`trunc`/`floor`/`ceil` always return `Int`.

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

- `@pure` functions reject effectful builtins and calls into known non-pure
  Aether functions
- `@pre` / `@post` take expressions; `@post` may reference `result`
- for tuple-return helpers, `@post` must use positional slots such as
  `result.0` and `result.1`
- multiple `@pre` and `@post` lines may stack on one function
- `@cost <n><unit>` validates a budget annotation; units: `ns us ms s op ops
  step steps`

## Strings

Core safe `Text` surface for generated code:

```aether
let nameLen: Int = string_len(name);   // canonical; name.len accepted
if status == "ready" { ... }           // canonical; string_eq(a,b) accepted
let s: Text = "ab" + "cd";             // concatenation of Text operands
let label: Text = int_to_text(count);  // Int -> Text (canonical surface for IntToStr)
```

`int_to_text(n)` is the canonical Int-to-`Text` conversion (it lowers to the
backend `IntToStr`). Reach for it when you need a `Text` *value* — for
concatenation, a field, or a return — not merely to print a number
(`println(n)` already prints an `Int` directly).

Additional verified `Text` builtins (all pure, no `fx` needed):

| Builtin | Effect |
|---|---|
| `copy(s, start, count)` | substring; `start` is 1-based |
| `pos(needle, s)` | 1-based index of `needle` in `s`, `0` if absent |
| `trim(s)` | strip leading/trailing whitespace |
| `stringofchar(ch, n)` | `n` copies of single-char `ch` |

Beyond these, do not invent string helpers: there is no `replace`, and no
whole-string `to_upper`/`to_lower` (`upcase`/`toupper` change a single character
only). If the prompt depends on more, use `builtin_info(...)` or prompt-supplied
signatures and call the exact discovered names.

## Files and environment

Host access for automation. **All of these are effectful — call inside `fx`**
(FX-001), and they are rejected inside `@pure` functions.

| Builtin | Signature |
|---|---|
| `fileexists(path)` | `(Text) -> Bool` |
| `getenv(name)` | `(Text) -> Text` (empty if unset) |
| `getenvint(name, fallback)` | `(Text, Int) -> Int` |
| `getcurrentdir()` | `() -> Text` |
| `mkdir(path)` / `rmdir(path)` | `(Text) -> Int` (0 on success, else errno) |
| `paramcount()` | `() -> Int` (CLI arg count) |
| `paramstr(i)` | `(Int) -> Text` (`paramstr(0)` is the program) |

```aether
fn main() -> Void {
    fx {
        let home: Text = getenv("HOME");
        if fileexists("/etc/hosts") { println("hosts present, home=", home); }
    }
    ret;
}
```

Networking (`http*`, `socket*`), database (`sqlite*`), `random`/`randomize`,
and the clock are also effectful but are not part of the curated surface; reach
for them via `builtin_info(...)` discovery.

## Dynamic arrays

```aether
let xs: Int[] = [];
xs = xs + [7];          // append pattern
let n: Int = length(xs);
let first: Int = xs[0]; // indexed read
xs[0] = 9;              // indexed write
let ys: Int[] = [1, 2, 3];
```

- `Type[]` declares; `[]` is the empty literal
- indexed reads/writes such as `xs[0]` and `xs[0] = v;` are supported
- multi-element literals such as `[1, 2, 3]` are supported
- `length(xs)` canonical; `len(xs)` and `xs.len` accepted
- never `toon_len(xs)` on a dynamic array (LEN-001)

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
- `_or` helpers only protect the final key lookup, not a missing intermediate
  object

### Helper surface (complete — BUILT-001)

| Need | Helper |
|---|---|
| availability | `has_toon()` |
| parse text / file | `toon_parse(text)`, `toon_parse_file(path)` |
| root / close | `toon_root(doc)`, `toon_close(doc)` |
| object field / array element | `toon_key(node, key)`, `toon_at(node, i)` |
| TOON array length | `toon_len(node)` |
| typed field get | `toon_get_text/int/real/bool(node, key)` |
| typed field get w/ fallback | `toon_get_text_or/int_or/real_or/bool_or(node, key, fb)` |
| node value | `toon_text/int/real/bool/null_value(node)` |
| kind / membership | `toon_type(node)`, `toon_has_key(node, key)`, `toon_has_at(node, i)` |
| shape checks | `toon_is_text/int/real/bool/null/arr/obj(node)` |

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

### Nested lookup safety (NEST-001)

`_or` helpers protect only the final keyed lookup on a valid object node —
not the path to it. Guard intermediates:

```aether
let code: Text = "EMPTY";
if toon_has_key(row, "meta") {
    let meta: ToonNode = toon_key(row, "meta");
    code = toon_get_text_or(meta, "code", "EMPTY");
}
```

Avoid `toon_get_text_or(toon_key(row, "meta"), "code", "EMPTY")` unless
`"meta"` is guaranteed present. Nested calls are fine when the shape is known,
but intermediate bindings are easier to read and debug.

Wrong:

```aether
let code: Text = toon_get_text_or(toon_key(row, "meta"), "code", "EMPTY");
```

### Worked example

```aether
fn main() -> Void {
    if !has_toon() {
        fx { println("yyjson unavailable"); }
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

This is the capture-free alternative to spawning closures (FUNC-001): you
parallelize ordinary calls and collect results through records, with no
function values. `par` is the mechanism for user code; the task helpers below
are a lower-level handle API over runtime builtins.

## Tasks and AI helpers

Compact aliases over shared runtime helpers; all are effectful — call inside
`fx` (FX-001). `task_spawn` / `task_queue` dispatch an allow-listed runtime
*builtin* by name (for example `"delay"`), not a user-defined function; passing
your own function name is an error. To parallelize your own functions, use
`par` (above).

Core signatures:

- `sleep(ms: Int) -> Void`
- `task_spawn(target: Text, name: Text, arg) -> Int`
- `task_queue(target: Text, name: Text, arg) -> Int`
- `task_wait(handle: Int) -> Int`
- `task_lookup(name: Text) -> Int`
- `task_status(handle: Int) -> Int`
- `task_result(handle: Int) -> Int`
- `task_stats() -> Array`
- `task_stats_json() -> Text`
- `ai_chat(model: Text, messages: Text, system: Text = "", apiKey: Text = "", endpoint: Text = "") -> Text`
- probes: `has_ai() -> Bool`, `has_builtin(category: Text, function: Text) -> Bool`

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

- `builtins_json()` → JSON array of every Aether-visible builtin name
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
  not rename. If a module exports `classifySupport`, call
  `classifySupport(...)` — never a guessed local like `classify(...)` unless
  you define the wrapper yourself
- after import, exported constants and helpers may be called directly; prefer
  direct calls like `answer()` and `Greeting` over guessed foreign object APIs
- file naming: `mod PascalCaseName` lives in a snake_case file consumed as
  `use "pascal_case_name";` (e.g. `mod ModuleConsts` → `use "module_consts";`)
- when combining purity with module export, write `@pure` above `export fn`
- for generated code, assume modules export `const` and `fn`; do not generate
  exported `type` blocks
- if the task provides a module, copy its export names exactly; do not invent
  wrapper names such as `classify(...)`, `normalize(...)`, or `getAnswer()`

```aether
mod BenchSupport {
    export const DefaultScore: Int = 50;
    @pure
    export fn clampSupport(score: Int) -> Int { ... }
}
```

```aether
use "bench_support";

fn summarize(job: ToonNode) -> Int {
    let raw: Int = toon_get_int_or(job, "score", DefaultScore);
    ret clampSupport(raw);
}
```

Wrong:

```aether
use "bench_support";
let score: Int = normalize(raw);
let status: Text = classify(score);
```

Real module examples: `Examples/aether/base/module_demo`,
`Examples/aether/base/module_consts_demo`.

Module recipe for generated code:

1. Add `use "module_name";` only when the module is verified or provided.
2. Copy exported names exactly as written in the module file.
3. Bind imported results to typed locals if they feed later logic.
4. Do not invent wrappers unless you define them yourself.

```aether
use "bench_math";

fn main() -> Void {
    let value: Int = answer();
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
compatibility shims such as ignored missing imports and redundant `let mut`.

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
        fx { println("yyjson unavailable"); }
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
        fx { println("yyjson unavailable"); }
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

### Large report recipe

Use this shape for long benchmark-style programs:

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

Larger examples: `Examples/aether/showcase/agent_report`,
`Examples/aether/showcase/release_board`.

## Repair rules

The compiler tags every rejection with a stable code in brackets, and on newer
builds prints a `help: see <CODE> ...` line. Read the code, then apply the fix.
The codes the compiler actually emits are FX-001, SYN-001, ANN-001, IMP-001,
SCOPE-001, TOON-001, TYPE-001, TUP-001, FLOW-001, MUT-001, FIELD-002, and
NAME-001; the finer rule names below map onto them.

- **[FX-001]** an output, task helper, or `ai_chat` call outside an effect block
  → wrap it in `fx { ... }`.
- **[SYN-001]** non-Aether syntax → `ret` not `return`, `type` (with Aether field
  syntax) not `class`, `loop` not `for`/`while`; drop `var`, `def`, `=>`.
- **[SCOPE-001]** a name/scope problem — the catch-all. It is one of:
  - a helper not listed in this document → it does not exist; inline the logic (BUILT-001)
  - an export called by a guessed name → use the exact exported name (MOD-001)
  - a type or helper used before it is defined → define it earlier (ORDER-001)
  - a method reaching an outer local → pass it in as a parameter (METH-001)
  - a genuinely undeclared / out-of-scope name → declare it earlier, pass it in,
    or rename the local you actually meant (SCOPE-001)
- **[NAME-001]** a local redeclared in the same scope (`'...' is already
  declared in this scope`) → pick a fresh name.
- **[IMP-001]** an invented or malformed import → remove the `use` unless
  verified, or write `use "module_name";` and call exports directly (MOD-002).
- **[TYPE-001]** a type cannot be inferred → annotate it. Also covers
  `toon_len(node)` for TOON arrays vs `length(xs)` for dynamic arrays (LEN-001).
- **[TOON-001]** a `ToonDoc` / `ToonNode` handle misuse → check handle types; do
  not mix, do arithmetic on, or cross-assign them.
- **[FIELD-002]** `Unknown field` → use the exact declared field name, or extend
  the type explicitly if the prompt really requires that field.
- **[FLOW-001]** a fallthrough path with no return value → add an explicit final
  `ret ...` on every reachable top-level path in the non-`Void` helper.
- **[ANN-001]** a misplaced annotation, or a `@pure` function calling an effect →
  move `@pre`/`@post`/`@pure`/`@cost` directly above the function; keep effects out of pure code.
- **[TUP-001]** tuple misuse → destructure a direct top-level call only.
- **[MUT-001]** `let mut` → drop `mut`; a plain `let` is already mutable.

The compiler cannot check *output correctness*. If the program compiles but the
result is wrong, no code is printed — these are authoring rules: integer result
where decimals are expected → introduce a `Real` operand (`100.0`); unstable
decimal output → `value:0:precision`; wrong receiver spelling → `self`, never
`Self`; iterating an object root → extract the array with `toon_key` first
(ROOT-001); a fallback that didn't save a nested lookup → guard the intermediate
node (NEST-001); exact-output mismatch → match spacing, casing, and precision
(FMT-001); Markdown fences in the answer → return raw Aether source only
(OUT-001); flattened or renamed JSON keys → copy them exactly (KEY-001).

## Validation Checklist

Before submitting Aether code, verify:

- all output, task helper, and `ai_chat` calls are inside `fx { ... }` (FX-001)
- every called helper appears in this document (BUILT-001)
- all `use "..."` imports reference real, verified modules; imported names are
  used exactly as exported (IMP-001, MOD-001)
- all function parameters have explicit types; uncertain types are annotated
  (TYPE-001)
- `ret` not `return`; `type` not `class` (SYN-001)
- no `let mut` in new code (MUT-001)
- no arithmetic on or cross-assignment of `ToonDoc` / `ToonNode`; every parsed
  doc is closed with `toon_close(doc)` (TOON-001)
- object-shaped payloads: named array extracted before iteration (ROOT-001);
  JSON keys copied exactly (KEY-001); intermediate nodes guarded before `_or`
  lookups (NEST-001)
- `toon_len` for TOON arrays, `length` for dynamic arrays (LEN-001)
- real arithmetic has a `Real` operand where decimals matter; stable decimal
  output uses `value:width:precision`
- tuple returns destructured directly (TUP-001)
- annotations sit above their functions (ANN-001)
- canonical forms used unless preserving accepted-but-non-canonical source
