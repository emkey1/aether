# Aether for Humans and LLMs

*Guide version: 2026-07-15-2*
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
8. **ORDER-001.** Define types and helper functions before `main` uses them.
9. **LEN-001.** `toon_len(node)` for TOON arrays; `length(arrayValue)` for
   dynamic arrays.
10. **TUP-001.** Tuples are narrow: destructure direct top-level helper
    returns (`let (a, b) = pair();`), or bind to one variable and read a
    single field with `.0`/`.1`/`.2` (`let t = pair(); t.0;`) -- but not
    `pair().0` directly. Assume nothing else. Recursive tuple-returning
    functions (direct or indirect) and concurrent `par`-branch calls to the
    same tuple-returning function are fully supported (tuple returns lower
    to a record returned by value, reentrant per call).
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
18. **FIELD-003.** A record/type field may declare a **constant** default:
    `field: Type = <const>` (e.g. `count: Int = 0`, `name: Text = ""`,
    `on: Bool = true`, `xs: Int[] = []`). Only compile-time constants are
    allowed — a default may not reference another field, `self`, or call a
    function. For a computed initial value, set it at construction with
    `new T { field: value }`.
19. **FLOW-001.** Every non-`Void` function must return a value on every
    reachable top-level path.
20. **FUNC-001.** Functions are not values. Aether has no anonymous functions,
    no inline `fn(...) -> T { ... }` literals, no lambdas, and no closures.
    Never pass a function as an argument. `task_spawn` / `task_queue` take a
    builtin *name* as `Text` (`task_spawn("delay", "worker", 5)`), not a
    function. There is no `map` / `filter` / `reduce`; transform with a `loop`.
    To run your own code concurrently, call your functions inside a
    `par { ... }` block (see **Concurrency**), not `task_spawn`.
21. **PAR-001.** Each `par` branch must own the record it writes. Passing the
    same record to two branches races — the concurrent writes corrupt the heap,
    so it is rejected at compile time. Give each branch its own record and
    combine the results after the block.
22. **MS-001.** `MStream` is the opaque memory-stream handle type.
    `mstreamcreate()` / `mstreamfromstring(text)` return `MStream` — never
    declare the binding `Int` or `Text`. Extract contents with
    `mstreambuffer(ms) -> Text`; release with `mstreamfree(ms)`. HTTP responses
    arrive in an `MStream` out-buffer (see **HTTP requests**).

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
- a field or method named after a reserved word — a type (`word`, `text`,
  `int`), a keyword (`new`, `for`, `match`), or an operator word (`mul`, `div`,
  `mod`, `xor`); rename the member (SYN-001)
- `fn new()` / `fn __init__` as a constructor method; Aether has none —
  construct with `new T { field: value }` (partial sets ok; unset fields keep
  their declared defaults), or `new T()` then assign fields, or a top-level
  factory `fn` (SYN-001)
- an untyped `new` binding or array literal: `let c = new C();` (a later
  `c.inc();` fails `POINTER but got VOID`) or `let xs = [1, 2, 3];` (cannot
  infer) — annotate both: `let c: C = new C();`, `let xs: Int[] = [1, 2, 3];`
  (TYPE-001)
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
- `let stream: Int = mstreamfromstring(...)` — memory streams are `MStream`
  handles, not integers (MS-001)
- annotations inside a function body (ANN-001)
- a tuple index chained directly onto a call: `pair().0` — bind it first,
  `let t = pair(); t.0;` (TUP-001)
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
| Loop keyword | `loop` (condition/range/infinite) | `while cond { }`, `for x in a..b { }` (Pascal heritage) | inventing other loop syntax |
| Return | `ret value;` (`ret;` for Void) | — | `return value;` |
| Output | variadic `println(a, b)` in `fx` | text-only `+` concatenation | `Text + Int` guessing |
| Text equality | `a == b` | `string_eq(a, b)` | inventing `.equals(...)` |
| Text length | `string_len(t)` | `t.len` | inventing `strlen(...)` |
| Text → Int | `parse_int(t)` | — | inventing `Int(t)` / `t.toInt()` |
| Text → Real | `parse_float(t)` | — | inventing `Real(t)` |
| Text → Bool | `parse_bool(t)` | — | comparing to `"true"` by hand |
| Int → Text | `int_to_text(n)` | `itoa(n)` | inventing `n.toString()` |
| Real → Text | `formatfloat(r, prec)` | `realtostr(r)` (always 6 dp) | `r:0:prec` as a value (it is `println`-only) |
| Char → Int code | `ord(ch)` | — | `int(ch)` (silently returns `0` for `Text`; it casts `Real`/`Bool`, not `Text`) |
| Int code → Char | `chr(code)` | — | inventing a lookup table |
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
| `MStream` | — | opaque memory-stream handle (MS-001) |

For mixed output, prefer variadic `println(...)` / `print(...)` rather than
guessing conversion helpers or `Text + Int` coercions.

## Operators

| Category | Operators |
|---|---|
| Arithmetic | `+ - * /` on `Int`/`Real`; `Int / Int` is integer-style division |
| Modulo | `%` (example: `7 % 3` = `1`) |
| Comparison | `== != < <= > >=` |
| Logical | `!` for negation; `&&` and `||` for conjunction/disjunction (short-circuit) |
| Bitwise/shift | `& \| ^`/`xor` and `<< >>` on `Int` (example: `6 & 3` = `2`, `6 << 1` = `12`) |
| Text | `+` concatenation (text-compatible operands only); `==` equality |
| Array | `xs + [v]` appends (any literal length); `xs + ys` concatenates two array-valued expressions |

`&`/`|`/`^` are bitwise, not logical -- use `&&`/`||` for Bool conjunction/
disjunction. `xor` may be spelled as the word `xor` or the symbol `^`
(both lex to the same operator); there is no word form for `&`/`|`.
Precedence (loosest to tightest): `|| , && , | , ^ , & , == != , < <= > >= ,
<< >> , + - , * / %`.

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
    ret Pair { first: 3, second: 7 };
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
const Limit: Int = 42;
let count: Int = 0;
let label: Text = "Aether";
```

`let` bindings are mutable (MUT-001). `let mut` is accepted, redundant, and
ignored; never generate it.

### Inference policy (TYPE-001)

Omit the type only for these initializers:

- literals: `42`, `3.5`, `"text"`, `true`
- calls to functions with known declared return types, including methods on
  known typed bindings and the TOON/string helpers in this document

Everything else gets an explicit type — especially `new` instances and array
literals (an inferred `let c = new C();` breaks a later method call, and
`let xs = [1, 2, 3];` cannot be inferred at all — see **Constructing records
and typing bindings**), TOON extractions with non-trivial shape, branchy
results, and arithmetic such as `base + 1` where the operand types are not
visible at a glance. When in doubt, annotate.

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
- a field may carry a **constant** default: `name: Type = <const>`
  (`count: Int = 0`, `name: Text = ""`, `on: Bool = true`, `xs: Int[] = []`).
  The default must be a compile-time constant — it cannot reference another
  field, `self`, or call a function (FIELD-003). For a computed initial value,
  set it at construction with `new T { field: value }`. A field with no default
  falls back to the type zero (integers `0`, reals `0.0`, booleans `false`,
  text empty).
- **define methods inside the `type` block; `self` is implicit — never give a
  method a `self` parameter.** Inside a method use lowercase `self`, never `Self`.
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
- **field and method names must not be reserved words.** A member named after a
  type keyword (`word`, `text`, `int`, `byte`, `bool`, `void`), a language
  keyword (`new`, `for`, `if`, `match`, `type`), or an operator word (`mul`,
  `div`, `mod`, `xor`) is rejected at parse time — `'<name>' is a reserved … and
  cannot be used as a field/method name` (SYN-001). Rename the member: `word` →
  `wordText`/`token`, `mul` → `multiply`/`scale` (plain names like `count`,
  `value`, `add`, `push` are fine). There is **no constructor method**: do not
  write `fn new()` (nor `fn __init__` / `Type.new()`); allocate with `new T()`
  and assign fields, or add a top-level factory `fn makeT(...) -> T` with a
  non-reserved name.

A record method goes **inside** the `type` with an implicit `self`, and its
contract may then reference `self`. A free-standing `fn` with an explicit `self`
parameter plus a `self`-referencing contract does not compile:

```aether
// WRONG — free-standing method + explicit self → [SCOPE-001] 'self' not in scope
type C { v: Int; }
@pre self.v >= 0
fn get(self: C) -> Int { ret self.v; }

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

**Always annotate a binding that holds a `new` instance or an array literal**
(TYPE-001) — inference is not enough for either:

- `let c = new C();` then `c.inc();` fails with `argument 1 to 'c.inc' expects
  type POINTER but got VOID`; a method call on an inferred `new` binding is
  unsafe. Write `let c: C = new C();`.
- `let xs = [1, 2, 3];` fails with `cannot infer the type of 'xs'`. Write
  `let xs: Int[] = [1, 2, 3];`.

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
`println(...)`:

```aether
let label: Text = if ready { "ready" } else { "blocked" };
fx {
    println(label);
    println("status: ", if ready { "ready" } else { "blocked" });
}
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

The integer-style result applies through every `Int`-typed sink — a
`let m: Int = ...` binding, record fields, array elements, `Int` parameters,
and (since 2026-07-15-2) a `-> Int` function's `ret`, so
`fn mean(...) -> Int { ret sum / n; }` returns a truncated `Int`, not a
`Real`. Only a division printed directly with no typed sink anywhere (e.g.
`println(7 / 2)`) shows the underlying real result (`3.500000`).

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
- a method's `@pre`/`@post` may reference `self.field`, but only when the method
  is declared inside its `type` block (where `self` is implicit); the same
  contract on a top-level `fn` with an explicit `self` parameter fails
  `[SCOPE-001] identifier 'self' not in scope`
- on a collection return (`-> T[]`), a contract must compare a *property* of the
  collection, not the collection itself: `@post length(result) > 0`, never
  `@post result > 0` (an array is not comparable to a scalar, so the whole-array
  form is rejected at compile time with ANN-001). The same applies to a `@pre`
  over an array parameter: `@pre length(xs) > 0`
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

### File content: read/write

The table above only covers existence/metadata. To read or write a file's
*contents*, declare a variable of type `File` (a Pascal-style line-oriented
file handle) and use the classic `assign`/`reset`/`rewrite`/`readln`/
`writeln`/`eof`/`close` sequence — same effect rules as above, all inside
`fx`:

| Builtin | Signature |
|---|---|
| `assign(f, path)` | `(File, Text) -> Void` — binds `f` to `path`, no I/O yet |
| `rewrite(f)` | `(File) -> Void` — truncate/create for writing |
| `reset(f)` | `(File) -> Void` — open for reading |
| `append(f)` | `(File) -> Void` — open for writing at end-of-file |
| `readln(f, line)` | `(File, Text) -> Void` — reads one line into `line` |
| `writeln(f, ...)` | `(File, ...) -> Void` — writes args plus a newline |
| `eof(f)` | `(File) -> Bool` — true once no more lines remain |
| `close(f)` | `(File) -> Void` |
| `erase(f)` | `(File) -> Void` — deletes the file named by a prior `assign` |
| `rename(f, newPath)` | `(File, Text) -> Void` |

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
            readln(f, line);
            println(line);
        }
        close(f);
    }
    ret;
}
```

`readln(f, line)` writing its result into `line` is a special case built into
this one builtin — Aether does not have general by-reference parameters, so
this pattern only works with the file builtins above, not with user-defined
functions.

Sockets (`socket*`), database (`sqlite*`), `random`/`randomize`, and the
clock are also effectful but are not part of the curated surface; reach for
them via `builtin_info(...)` discovery.

## HTTP requests and memory streams (MS-001)

HTTP support needs a build with libcurl networking (`AETHER_ENABLE_CURL=ON`);
without it the `http*` builtins fail at runtime. The response body arrives in
an `MStream` — an opaque memory-stream handle (never `Int`, never `Text`).

| Builtin | Signature | fx? |
|---|---|---|
| `httpsession()` | `() -> Int` (session handle, `-1` on failure) | yes |
| `httprequest(session, method, url, body, out)` | `(Int, Text, Text, Text, MStream) -> Int` (HTTP status, `-1` on transport error) | yes |
| `httpsetheader(session, name, value)` | `(Int, Text, Text) -> Void` | yes |
| `httpclose(session)` | `(Int) -> Void` | yes |
| `mstreamcreate()` | `() -> MStream` | no (pure memory) |
| `mstreamfromstring(text)` | `(Text) -> MStream` | no |
| `mstreambuffer(ms)` | `(MStream) -> Text` (stream contents) | no |
| `mstreamfree(ms)` | `(MStream) -> Void` | no |
| `mstreamloadfromfile(ms, path)` / `mstreamsavetofile(ms, path)` | `(MStream, Text) -> Bool` | yes (file I/O) |

Golden shape — a GET request end to end (all `http*` calls inside `fx`):

```aether
fn main() -> Void {
    fx {
        let session: Int = httpsession();
        let out: MStream = mstreamcreate();
        let status: Int = httprequest(session, "GET", "https://example.com/", "", out);
        if status == 200 {
            let body: Text = mstreambuffer(out);
            println(body);
        }
        mstreamfree(out);
        httpclose(session);
    }
    ret;
}
```

- the `out` argument must be an initialized `MStream` (`mstreamcreate()`);
  `httprequest` clears it before writing the response into it
- `mstreambuffer(out)` is the ONLY way to get the body as `Text` — never
  `print(out)` and never `let body: Int = ...`
- `body` (arg 4) is the request payload — pass `""` for GET
- a JSON response pairs naturally with TOON: `toon_parse(mstreambuffer(out))`
- `MStream` handles are opaque: no arithmetic, no cross-assignment with
  `ToonDoc`/`ToonNode`/`Int` (MS-001)

## Sockets

A real, working BSD-socket-style API — not the `tcpsocket*`/`udpsocket*`/
`resolve` names a model might guess by analogy with other languages; those do
not exist. All `socket*` calls and `dnslookup` are effectful (FX-001, inside
`fx`). `socketreceive` returns its data in an `MStream` (MS-001), same as
`http*` — never `Int`, never `Text` directly.

| Builtin | Signature | Notes |
|---|---|---|
| `socketcreate(type[, family])` | `(Int, Int?) -> Int` (socket handle, `-1` on failure) | `type`: `0` = TCP (`SOCK_STREAM`), `1` = UDP (`SOCK_DGRAM`); `family`: `4` = IPv4 (default), `6` = IPv6 |
| `socketconnect(socket, host, port)` | `(Int, Text, Int) -> Int` (`0`/`-1`) | resolves `host` (hostname or literal address) and connects |
| `socketbind(socket, port)` | `(Int, Int) -> Int` (`0`/`-1`) | binds on all local interfaces (`INADDR_ANY`) |
| `socketbindaddr(socket, host, port)` | `(Int, Text, Int) -> Int` (`0`/`-1`) | binds to one specific local address |
| `socketlisten(socket, backlog)` | `(Int, Int) -> Int` (`0`/`-1`) | |
| `socketaccept(socket)` | `(Int) -> Int` (new connected-socket handle, `-1` on failure) | **blocks** until a peer connects |
| `socketpeeraddr(socket)` | `(Int) -> Text` | remote IP of a connected socket |
| `socketsend(socket, data)` | `(Int, Text\|MStream) -> Int` (bytes sent, `-1` on failure) | `data` may be `Text` or an `MStream` |
| `socketreceive(socket, maxlen)` | `(Int, Int) -> MStream` | **blocks** until data arrives (or the peer closes — a `0`-length result then means EOF, not failure); use `mstreambuffer(...)` to read it |
| `socketsetblocking(socket, blocking)` | `(Int, Bool) -> Int` (`0`/`-1`) | flips a socket to non-blocking mode |
| `socketpoll(socket, timeoutMs, flags)` | `(Int, Int, Int) -> Int` | `flags`: `1` = check readable, `2` = check writable (bitwise-or both); returns `0` on timeout, else a bitmask of what's ready |
| `socketclose(socket)` | `(Int) -> Int` (`0`/`-1`) | |
| `socketlasterror()` | `() -> Int` | last socket error code (errno/WSA), `0` if the last call succeeded |
| `dnslookup(hostname)` | `(Text) -> Text` | forward lookup only (no reverse lookup); returns `""` on failure |

The gotcha every generated socket example must handle: `socketaccept` and
`socketreceive` **block the calling task indefinitely** until a peer shows up
— call either one with no counterpart running concurrently and the program
hangs forever. Two ways to avoid that:

1. **Same-process client + server via `par`** (recommended for a
   self-contained demo). Create, bind, and put the listening socket into
   listen state *before* the `par` block — not inside one of its branches —
   so the client branch can never race ahead of a not-yet-listening server;
   `par` already joins (waits for both) before continuing (see
   **Concurrency**).
2. **Non-blocking + `socketpoll`.** Call `socketsetblocking(socket, false)`,
   then loop on `socketpoll(socket, timeoutMs, 1)` until it reports readable
   (or a deadline passes) before calling `socketaccept`/`socketreceive`.

Golden shape — a same-process TCP echo, listener set up before `par`, verified
to run to completion in well under a second:

```aether
type Message {
    payload: Text;
}

fn server(listenSock: Int, out: Message) -> Void {
    fx {
        let conn: Int = socketaccept(listenSock);
        let ms: MStream = socketreceive(conn, 256);
        out.payload = mstreambuffer(ms);
        println("server got: ", out.payload);
        socketsend(conn, "pong");
        mstreamfree(ms);
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
        out.payload = mstreambuffer(ms);
        println("client got: ", out.payload);
        mstreamfree(ms);
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
    fx { println("done"); }
    ret;
}
```

## Dynamic arrays

```aether
let xs: Int[] = [];
xs = xs + [7];          // append pattern (any literal length: [7], [7, 8], even [])
let n: Int = length(xs);
let first: Int = xs[0]; // indexed read
xs[0] = 9;              // indexed write
let ys: Int[] = [1, 2, 3];
let zs: Int[] = xs + ys; // concatenation: `arr1 + arr2`, both already-built arrays
xs = xs + ys;            // same idiom, self-reassignment form
```

- `Type[]` declares; `[]` is the empty literal
- indexed reads/writes such as `xs[0]` and `xs[0] = v;` are supported
- multi-element literals such as `[1, 2, 3]` are supported
- `xs = xs + [a, b, ...];` appends every element of a literal, in order,
  regardless of the literal's length (one element, many, or even `[]`, a no-op)
- `xs = xs + ys;` / `let zs: T[] = xs + ys;` (`+` between two array-valued
  expressions, neither necessarily a literal) concatenates: the RHS is
  evaluated once and every one of its elements is appended, in order
- `length(xs)` canonical; `len(xs)` and `xs.len` accepted
- never `toon_len(xs)` on a dynamic array (LEN-001)
- `println`/`print` do not stringify arrays. `println("data: ", xs)` compiles
  and runs, but silently prints the array's internal representation
  (`ARRAY(dims:1, base_type:INT64, elements_at:0x...)`) instead of its
  elements -- there is no error to catch this. Loop and print each element:
  `loop i in 0..length(xs) { print(xs[i], " "); } println("");`

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
SCOPE-001, TOON-001, TYPE-001, TUP-001, FLOW-001, FLOW-002, MUT-001, FIELD-002,
FIELD-003, PAR-001, PAR-002, and NAME-001; the finer rule names below map onto them.

- **[FX-001]** an output, task helper, or `ai_chat` call outside an effect block
  → wrap it in `fx { ... }`.
- **[SYN-001]** non-Aether syntax → `ret` not `return`, `type` (with Aether field
  syntax) not `class`; drop `var`, `def`, `=>`. Also a
  **field or method named after a reserved word** (`word`, `mul`, `new`, `for`,
  ...) → rename the member (see Records: `type`).
- **[SCOPE-001]** a name/scope problem — the catch-all. It is one of:
  - a helper not listed in this document → it does not exist; inline the logic (BUILT-001)
  - an export called by a guessed name → use the exact exported name (MOD-001)
  - a type or helper used before it is defined → define it earlier (ORDER-001)
  - a method reaching an outer local → pass it in as a parameter (METH-001)
  - `method '<m>' is not defined on type '<T>'` → define `fn <m>(...)` inside
    `type <T> { ... }`, or fix the call name (METH-001)
  - a genuinely undeclared / out-of-scope name → declare it earlier, pass it in,
    or rename the local you actually meant (SCOPE-001)
- **[NAME-001]** a local redeclared in the same scope (`'...' is already
  declared in this scope`) → pick a fresh name.
- **[IMP-001]** an invented or malformed import → remove the `use` unless
  verified, or write `use "module_name";` and call exports directly (MOD-002).
- **[TYPE-001]** a type cannot be inferred → annotate it (including an untyped
  array literal such as `let xs: Int[] = [1, 2, 3];`). Also covers
  `toon_len(node)` for TOON arrays vs `length(xs)` for dynamic arrays (LEN-001).
- **`expects type POINTER but got VOID`** on a method call (no bracketed code) →
  the receiver is an inferred `new` binding; annotate it: `let c: C = new C();`.
- **[TOON-001]** a `ToonDoc` / `ToonNode` handle misuse → check handle types; do
  not mix, do arithmetic on, or cross-assign them.
- **[FIELD-002]** `Unknown field` → use the exact declared field name, or extend
  the type explicitly if the prompt really requires that field.
- **[FIELD-003]** a field default that is not a compile-time constant (references
  another field, `self`, or calls a function), or a populated array default →
  use a literal/constant (`count: Int = 0`), or drop the default and set the
  value at construction with `new T { field: value }`. A type-mismatched default
  (`value: Int = "x"`) is a `[TYPE-001]` instead — make the default's type match
  the field.
- **[FLOW-001]** a fallthrough path with no return value → add an explicit final
  `ret ...` on every reachable top-level path in the non-`Void` helper.
- **[FLOW-002]** an empty `ret;` in a non-`Void` function (a `ret` with no value)
  → give the return a value (`ret <expr>;`), or declare the function `-> Void`.
- **[ANN-001]** a misplaced annotation, or a `@pure` function calling an effect →
  move `@pre`/`@post`/`@pure`/`@cost` directly above the function; keep effects out of pure code.
- **[TUP-001]** tuple misuse → destructure a direct top-level call
  (`let (a, b) = pair();`), or bind it and read one field (`let t = pair();
  t.0;`); an index past the arity or chained directly onto the call
  (`pair().0`) both fail here too. If the callee isn't a known top-level
  tuple-returning function, return a record instead and read its fields.
- **[MUT-001]** `let mut` → drop `mut`; a plain `let` is already mutable.
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
- all function parameters have explicit types; uncertain types are annotated;
  `new` instances and array literals carry an explicit type (`let c: C = new
  C();`, `let xs: Int[] = [1, 2, 3];`) (TYPE-001)
- `ret` not `return`; `type` not `class` (SYN-001)
- no field or method named after a reserved word (`word`, `mul`, `new`, `for`);
  no `fn new()` constructor method (SYN-001)
- no `let mut` in new code (MUT-001)
- no arithmetic on or cross-assignment of `ToonDoc` / `ToonNode`; every parsed
  doc is closed with `toon_close(doc)` (TOON-001)
- object-shaped payloads: named array extracted before iteration (ROOT-001);
  JSON keys copied exactly (KEY-001); intermediate nodes guarded before `_or`
  lookups (NEST-001)
- `toon_len` for TOON arrays, `length` for dynamic arrays (LEN-001)
- real arithmetic has a `Real` operand where decimals matter; stable decimal
  output uses `value:width:precision`
- tuple returns destructured directly, or bound to a variable and read with
  `.0`/`.1`/`.2` — never chained directly onto the call (TUP-001)
- annotations sit above their functions (ANN-001)
- canonical forms used unless preserving accepted-but-non-canonical source
