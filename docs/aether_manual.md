# The Aether Manual

*For people learning and using Aether 1.0.*

Aether is a small, statically typed programming language designed to be written
by language models, and read, reviewed and run by people. This manual teaches
it to a person: how to build it, how to write it, what each part of the
language does, and why it looks the way it does.

Other documents describe Aether for other readers:

- **The three LLM guides** ([full](aether_for_llms_and_others.md),
  [medium](aether_for_llms_medium_contexts.md),
  [small](aether_for_llms_with_small_contexts.md)) are written to be pasted
  into a model's prompt. They lead with rules, are ordered for a model whose
  copy may be cut short, and are measured by the benchmark. Give one to a model
  that will write Aether; read this manual to learn it yourself.
- **[The specification](aether_spec.md)** says which programs are Aether and
  what they print. Its grammar and leniency chapters are written; its semantics
  chapters are still stubs.
- **[Architecture and rationale](aether_architecture_and_rationale.md)**
  explains how the compiler is built.

**Every program in this manual runs.** The test `aether_manual_snippets`
compiles every code block here, runs every complete program, and fails when
what it prints differs from the output shown beneath it, error messages
included. If your build prints something else, the build and the manual come
from different commits.

Shell examples assume you are in the repository root after building, so the
compiler is `./build/aether`. If you installed it, write `aether` instead.

## Contents

1. [What Aether is, and why](#1-what-aether-is-and-why)
2. [Install, build and run](#2-install-build-and-run)
3. [A first program](#3-a-first-program)
4. [A short tour](#4-a-short-tour)
5. [Types and literals](#5-types-and-literals)
6. [Variables and constants](#6-variables-and-constants)
7. [Functions and tuples](#7-functions-and-tuples)
8. [Records and methods](#8-records-and-methods)
9. [Control flow and loops](#9-control-flow-and-loops)
10. [Effects and `fx`](#10-effects-and-fx)
11. [Text](#11-text)
12. [Arrays](#12-arrays)
13. [Maps](#13-maps)
14. [Contracts](#14-contracts)
15. [Errors and diagnostics](#15-errors-and-diagnostics)
16. [Modules](#16-modules)
17. [Files, environment and arguments](#17-files-environment-and-arguments)
18. [HTTP and sockets](#18-http-and-sockets)
19. [TOON and JSON](#19-toon-and-json)
20. [Concurrency](#20-concurrency)
21. [The `--deny` sandbox and exit status](#21-the---deny-sandbox-and-exit-status)
22. [Release labels and versions](#22-release-labels-and-versions)
23. [Where to go next](#23-where-to-go-next)

Appendix: [Coming from another language](#appendix-coming-from-another-language)

---

## 1. What Aether is, and why

Aether is:

- **Statically typed.** Every value has a type the compiler knows. Function
  parameters and return types are always written out; local types can often
  be inferred.
- **Small and regular.** `fn`, `let`, `const`, `ret`, `if`, `loop`, `type`,
  `mod`, `use`, `fx` and `par`, with a few dozen builtins, cover most programs.
- **Explicit about effects.** Anything that touches the world outside the
  program (printing, files, the network, the clock, randomness) is written
  inside an `fx { ... }` block, so you can see where a program acts.
- **Contract-carrying.** `@pre` and `@post` become checks at run time; `@pure`
  is checked at compile time.
- **Diagnosed with stable codes.** A compile error carries a code such as
  `[FX-001]`, a message, and usually a hint that names the fix.

### Written by language models

Aether's design goal is that a language model should write valid, correct
Aether with as little in its prompt as possible. The project measures that with
a benchmark of programming tasks run against open-weight models, and treats the
benchmark, not taste, as the instrument. When capable models keep reaching for
a construct that does not exist, or keep tripping over one that does, that
counts as a defect in the language, and the fix goes into the language rather
than into instructions that teach models to avoid it.

Much of what might look arbitrary to a person comes out of that loop:

- A foreign spelling that models write with one clear meaning is often
  accepted as an exact synonym: `while` and `for` beside `loop`, `and`, `or`
  and `not` beside `&&`, `||` and `!`, `Float` and `String` beside `Real` and
  `Text`.
- Other foreign spellings are rejected with a diagnostic that names the Aether
  form: `return` (write `ret`), `class` (write `type`), `elif`, `match`.
- A form that means something different in another language is refused,
  because accepting it would give a program that runs and prints the wrong
  answer. `//` always starts a comment and is never Python's floor division;
  `0 <= i < n` is an error, not a chained comparison; `println("{} items", n)`
  is an error, not a placeholder.
- Diagnostics name their fix, so a model can repair a program from the message
  alone, and so can you.

The file `tests/surface/registry.json` lists every such spelling, its class and
a test that proves it.

Some things are left out on purpose, each by a recorded decision in
[`docs/aether_decisions.md`](aether_decisions.md): function values (no
lambdas, closures or callbacks), generics, enums, Option and Result types,
exceptions, default and named arguments, string interpolation, and a `switch`
statement. Most of these decisions state a reopen condition in terms of
measurement: if models keep failing for want of the feature after the
diagnostic that explains its absence has shipped, it is reconsidered. Maps are
the precedent: left out at first (D47), they were added in October 2026
(D68).

### What it is for

Small-to-medium automation programs: read a JSON payload, pull out typed
fields, classify or transform them, and print or write a report, with the
effects visible and a few contracts checked along the way. It is not a dynamic
scripting language, and it is not a systems language.

### How it runs

Aether has no virtual machine of its own. The `aether` binary parses your
program and checks it (types, effects, purity, contracts), then builds the
abstract syntax tree of the shared PSCAL toolchain. The shared bytecode
compiler and the PSCAL VM do the rest. The same engine runs PSCAL's other front
ends, and Aether plugs into the Rea engine through a hook interface rather than
a copy of it.

```text
program.aether
  -> Aether parser and checks        (src/aether)
  -> shared PSCAL syntax tree
  -> Rea engine analysis              (external/rea)
  -> PSCAL bytecode compiler and VM   (external/pscal-core)
```

One backend is a hard rule of the project: there is no Aether VM, bytecode or
runtime, and there will not be one. A fix to the VM helps every front end, and
Aether never has a second runtime to keep in step. The compiled bytecode is
cached per source file, so running an unchanged program again skips the
compile (see [§2](#the-bytecode-cache)).

### A short history

- **June 2026.** Aether starts as a front end in the PSCAL project, first as a
  text rewriter over Rea. On 27 June its own parser, which builds the shared
  syntax tree directly, became the default, and on 1 July the rewriter was
  deleted.
- **Summer 2026.** The benchmark, the three guides and fine-tuned models grow
  up around it, and the language changes by measurement: a construct models
  need is added, a construct that fools them gets a diagnostic.
- **October 2026.** The benchmark and the fine-tuning work move into this
  repository. A review round makes names case-sensitive, renames every builtin
  to snake_case and settles how a `Real` prints, and the result is tagged
  `1.0-beta1`. From then on, changes follow the compatibility rules in
  [§22](#22-release-labels-and-versions).

`CHANGELOG.md` records every change to what programs compile or print, with
the date version it shipped in.

---

## 2. Install, build and run

### What you need

- A C compiler, CMake 3.18 or later, and git.
- The libcurl development headers, for networking (`http_*` and `socket_*`):
  `brew install curl` on macOS, `libcurl4-openssl-dev` or the equivalent on
  Linux. To build without networking, configure with
  `-DAETHER_ENABLE_CURL=OFF`.
- Python 3, only for the test gates.

### Build

The shared engine lives in two git submodules under `external/`: `rea` (the
Rea engine) and `pscal-core` (the bytecode compiler and VM). Clone
recursively, or the build fails on an empty `external/`:

```sh
git clone --recurse-submodules https://github.com/emkey1/aether.git
cd aether
cmake -S . -B build
cmake --build build -j
```

The build is Release by default. If you cloned without
`--recurse-submodules`, run `git submodule update --init --recursive` once.
`-DAETHER_ENABLE_SDL=ON` adds the optional SDL graphics builtins
(`examples/sdl/`).

### Run

```sh
./build/aether hello.aether
./build/aether greet.aether Ada Grace
```

Options go before the source file. Everything after it is passed to the
program, where `args()` returns it (§17). Programs conventionally end in
`.aether`; modules are the exception (§16).

| Option | Effect |
|---|---|
| `-v`, `--version` | print the version: `Aether Compiler Version: <date version> (<release label>)` |
| `--no-run` | compile and check only |
| `--no-cache` | compile fresh; neither read nor write the bytecode cache |
| `--verbose` | say whether the cache was used |
| `--diagnostics-json`, `--diagnostics-toon` | print compile errors as JSON or TOON records (§15) |
| `--verbose-compat` | also print quiet compatibility warnings, such as an ignored missing import (§16) |
| `--deny LIST` | refuse classes of effects at run time (§21) |
| `--prune-cache` | delete cache entries whose source file is gone, then exit |
| `--dump-ext-builtins` | list the extended builtins this build registers |

A `#!` line at the very top of a file is ignored. So with `aether` on your
`PATH`, a program whose first line is `#!/usr/bin/env aether` runs like any
script:

```sh
chmod +x hello.aether
./hello.aether
```

### The bytecode cache

The first run of a program compiles it and stores the bytecode in
`$HOME/.pscal/bc_cache/`, one entry per source file. Later runs reuse the
entry while the source (and every module it imports) is unchanged. You
normally never think about it. Two cases where you do:

- When you work on the compiler itself, or compare two compiler builds, pass
  `--no-cache` so that each run compiles fresh. Every test in the repository
  does.
- `--prune-cache` clears out entries for files you have deleted.

### Install

```sh
cmake --install build --prefix /usr/local
```

This installs the `aether` binary in `<prefix>/bin`, the examples in
`<prefix>/share/aether/examples`, and the docs in `<prefix>/share/doc/aether`.

### Test

```sh
ctest --test-dir build --output-on-failure
ctest --test-dir build -LE invariance     # skips the two-minute reformat lap
```

CTest runs the conformance corpus in `tests/`, the examples, the guide and
manual gates, and the replay of real model-written programs. `CLAUDE.md` at
the repository root describes each gate.

---

## 3. A first program

Save this as `hello.aether` and run `./build/aether hello.aether`:

```aether
fn main() -> Void {
    fx {
        println("Hello, Aether");
    }
}
```

```text
Hello, Aether
```

- `fn main() -> Void` declares the entry point. `Void` means it returns
  nothing. A `main` may instead be declared `-> Int`, and the value it returns
  becomes the program's exit status (§21).
- `fx { ... }` marks the statements that act on the world. `println` writes to
  standard output, so it must sit inside one; outside, it is the compile error
  `[FX-001]`.
- `println` writes its arguments and then a newline. Statements end with `;`,
  and blocks use braces.

A file with no `fn main` is a script: its top-level statements run in order.

```aether @ok file=script.aether
// No fn main: the statements run from top to bottom.
let name: Text = "script";
fx { println("Hello from a ", name); }
```

```text
Hello from a script
```

A file may not have both a `fn main` and top-level statements, because `main`
would then never run:

```aether @reject=ENTRY-001
fx { println("top level"); }

fn main() -> Void {
    fx { println("in main"); }
}
```

```text
prog.aether:1: [ENTRY-001] Aether parser error: fn main will not run: this file also has top-level statements (this line is the first).
hint: move the top-level statements into main, or end the file with `main();`.
help: see ENTRY-001 in the Aether guide
```

As the hint says, a script may also call `main();` itself at the end.

---

## 4. A short tour

Here is a slightly larger program that touches most of the language. It reads
orders from JSON, adds tax and a rush fee, totals them per customer, and prints
the totals in name order.

```aether
type Order {
    customer: Text;
    amount: Real;
    rush: Bool = false;

    fn fee() -> Real {
        if self.rush {
            ret 5.0;
        }
        ret 0.0;
    }
}

@pre amount >= 0.0
@post result >= amount
@pure
fn withTax(amount: Real) -> Real {
    ret amount * 1.08;
}

fn parseOrders(json: Text) -> Order[] {
    let doc: ToonDoc = toon_parse(json);
    let rows: ToonNode = toon_root(doc);
    let orders: Order[] = [];
    loop i in 0..toon_len(rows) {
        let row: ToonNode = toon_at(rows, i);
        orders = orders + [new Order {
            customer: toon_get_text(row, "customer"),
            amount: toon_get_real(row, "amount"),
            rush: toon_get_bool_or(row, "rush", false)
        }];
    }
    toon_close(doc);
    ret orders;
}

fn main() -> Void {
    let orders: Order[] = parseOrders(
        "[{\"customer\": \"ann\", \"amount\": 20.0},"
        + " {\"customer\": \"bob\", \"amount\": 12.5, \"rush\": true},"
        + " {\"customer\": \"ann\", \"amount\": 7.25}]");
    let totals: Map[Text, Real] = {};
    loop o in orders {
        totals[o.customer] = get(totals, o.customer, 0.0) + withTax(o.amount) + o.fee();
    }
    fx {
        loop name in sorted(keys(totals)) {
            println(name, ": ", format_float(totals[name], 2));
        }
        println(length(orders), " orders, ", length(totals), " customers");
    }
}
```

```text
ann: 29.43
bob: 18.50
3 orders, 2 customers
```

What it shows, and where each part is explained:

- `type Order` is a record with three fields, one with a default, and a method
  `fee` that reads its fields through `self` ([§8](#8-records-and-methods)).
- `withTax` is a pure function with a precondition and a postcondition; the
  compiler checks that it has no effects, and the program stops if a call
  breaks either contract ([§14](#14-contracts)).
- `parseOrders` reads JSON through the `toon_*` builtins
  ([§19](#19-toon-and-json)) and builds an array by appending
  ([§12](#12-arrays)). `new Order { ... }` constructs a record.
- `totals` is a map from `Text` to `Real` ([§13](#13-maps)); `get` reads it
  with a default.
- Every printed line sits in one `fx` block at the end
  ([§10](#10-effects-and-fx)). The calculation above it has no effects.
- `format_float(x, 2)` turns a `Real` into text with two decimals
  ([§11](#11-text)).

---

## 5. Types and literals

| Type | Values | Notes |
|---|---|---|
| `Int` | `42`, `-7`, `0xFF`, `1_000_000` | 64-bit signed; overflow stops the program |
| `Real` | `3.5`, `2.5e2`, `.5` | 64-bit floating point |
| `Text` | `"hello"`, `'hello'` | a string of Unicode characters |
| `Bool` | `true`, `false` | |
| `Void` | | the return type of a function that returns nothing |
| `T[]` | `[1, 2, 3]`, `[]` | a growable array of `T` ([§12](#12-arrays)) |
| `Map[Text, T]` | `{"a": 1}`, `{}` | Text keys, `T` values ([§13](#13-maps)) |
| record types | `new Point { x: 1 }` | declared with `type` ([§8](#8-records-and-methods)) |
| `(T1, T2, ...)` | `(1, "a")` | tuples, as a function's return type only ([§7](#7-functions-and-tuples)) |
| `ToonDoc`, `ToonNode` | | handles to parsed JSON ([§19](#19-toon-and-json)) |
| `MStream` | | a memory buffer, for HTTP and sockets ([§18](#18-http-and-sockets)) |

`Float` and `String` are accepted as other names for `Real` and `Text`. No
other foreign type name is: `Double`, `Integer`, `Boolean`, `List`, `Dict` and
lower-case `int` are each a `TYPE-002` error that names the Aether type.

### Literals

```aether
fn main() -> Void {
    fx {
        println(42, " ", -7, " ", 0xFF, " ", 1_000_000);
        println(3.5, " ", 2.5e2, " ", .5, " ", 100.0, " ", 0.1 + 0.2);
        println("say \"hi\"", " ", 'single', " ", "ab" "cd", " ", "café \x41");
        println("caf\u00e9 ", length("caf\u00e9"), " \U0001F600 \u{1F600}");
        println(true, " ", false, " ", !true);
    }
}
```

```text
42 -7 255 1000000
3.5 250.0 0.5 100.0 0.30000000000000004
say "hi" single abcd café A
café 4 😀 😀
true false false
```

- `_` between digits is ignored. Hexadecimal literals start with `0x`; there
  are no binary or octal literals.
- Text literals use double or single quotes and stay on one line. Source files
  are UTF-8, so a literal may hold any character. The escapes are `\n`, `\t`,
  `\r`, `\"`, `\\`, `\xNN` (one byte: `\x41` is `A`), and `\uXXXX` or
  `\UXXXXXXXX` (a Unicode code point: `\u00e9` is `é`, one character long).
  The braced form that Rust, JavaScript and Swift use, `\u{1F600}`, takes any
  number of hex digits and means the same as `\U0001F600`. A JSON-style
  surrogate pair, `\uD83D\uDE00`, joins into one character; half a pair is a
  `SYN-001` error, in braces too. An unknown escape keeps its backslash, and so
  does `\u` with fewer than four hex digits or with braces that hold no hex
  digits, so `"C:\users"` prints as written. Two literals written side by side
  join into one.
- A `Real` prints in the shortest form that reads back as the same value, as
  Python prints it: `100.0`, `250.0`, `0.30000000000000004`.

### Arithmetic

```aether
fn main() -> Void {
    let a: Int = 7;
    let b: Int = 2;
    fx {
        println(a / b, " ", a div b, " ", a % b);
        println(-a div b, " ", -a % b);
        println(a * 1.5, " ", real(a) / 4.0, " ", int(3.99), " ", int(-3.99));
        println(round(2.5), " ", round(-2.5), " ", floor(-3.2), " ", ceil(3.2));
        println(6 & 3, " ", 6 | 3, " ", 6 ^ 3, " ", 6 << 1, " ", -16 >> 2);
    }
}
```

```text
3.5 3 1
-3 -1
10.5 1.75 3 -3
3 -3 -4 4
2 7 5 12 -4
```

- `/` always gives a `Real`, even between two `Int`s. For an integer quotient
  write `div`, which truncates toward zero. This is Python 3's reading of `/`,
  which is what models write; a census of the project's program corpus found
  none that needed integer `/` and eight that needed the `Real` (decision D4).
- `%` takes the sign of the left operand, so `-7 % 2` is `-1`. Test oddness
  with `n % 2 != 0`, not `n % 2 == 1`.
- `int(x)` truncates a `Real` toward zero. `round` rounds half away from zero.
  `round`, `trunc`, `floor` and `ceil` return `Int`.
- `&`, `|`, `^` (also spelled `xor`), `<<` and `>>` work on `Int` bits.
  Shifts do not check overflow: `<<` drops bits shifted past bit 63, so
  `1 << 63` is the minimum `Int` and `1 << 64` is `0`. `>>` keeps the sign,
  and a negative shift amount stops the program.
- Integer overflow and division by zero (of `Int` or `Real`, with `div`, `/`
  or `%`) stop the program with a coded run-time error,
  `AETH-RUNTIME-INT-OVERFLOW` or `AETH-RUNTIME-DIV-ZERO`, rather than wrapping
  around or producing infinity:

```aether @trap=AETH-RUNTIME-INT-OVERFLOW
fn main() -> Void {
    let big: Int = 9223372036854775807;
    fx { println(big + 1); }
}
```

```text
prog.aether:3: [AETH-RUNTIME-INT-OVERFLOW] Integer overflow: the result is outside Int's 64-bit range.
hint: check the operands before the operation, or compute in Real when values get that large.
[Error Location] Offset: 33, Line: 3
```

A run-time error names the source line twice: in the `file:line:` prefix and
in the `[Error Location]` line. The offset is a position in the compiled
bytecode, useful only to someone debugging the compiler.

### Operators and precedence

| Kind | Operators |
|---|---|
| arithmetic | `+ - * / div %` |
| comparison | `== != < <= > >=` |
| logical | `&& \|\| !`, or the words `and`, `or`, `not` |
| bitwise | `& \| ^ xor << >>` |
| Text | `+` joins two Texts; `==` and `<` compare |
| arrays | `+` appends or concatenates; `==` compares element by element |

From loosest to tightest binding: `||`, `&&`, `|`, `^`, `&`, `== !=`,
`< <= > >=`, `<< >>`, `+ -`, `* / div %`, then the unary operators. `&&` and
`||` short-circuit.

Two precedence traps are diagnosed. `flags & mask != 0` groups as
`flags & (mask != 0)` because the bitwise operators bind looser than the
comparisons, so it compiles with the warning `PREC-001`; write
`(flags & mask) != 0`. And comparisons do not chain: `0 <= i < n` is an error;
write `0 <= i && i < n`.

### Conversions

| From | To | Write |
|---|---|---|
| `Real` | `Int` | `int(x)` (truncates), `round(x)`, `floor(x)`, `ceil(x)` |
| `Int` | `Real` | `real(n)`, or mix it with a `Real`: `n * 1.0` |
| `Int` | `Text` | `int_to_text(n)` |
| `Real` | `Text` | `format_float(x, digits)`, or `real_to_text(x)` for the printed form |
| `Text` | `Int`, `Real` | `try_parse_int(t)`, `try_parse_real(t)` (checked); `parse_int(t)`, `parse_float(t)` (unchecked) |
| `Text` | `Bool` | `parse_bool(t)` |
| one-character `Text` | `Int` code | `ord(c)`; back with `chr(n)` |

Joining text with a number converts the number, left to right, when the result
is declared `Text`: `let s: Text = "n=" + 3;` is `"n=3"`. [§11](#11-text)
covers parsing text safely.

Storing a `Real` in an `Int` drops the fraction. It compiles, because that is
sometimes what you want, but with a warning, because it is more often a bug:

```aether @ok warn=NARROW-001
fn main() -> Void {
    let n: Int = 3.7;
    fx { println(n); }
}
```

```text
prog.aether:2: warning: [NARROW-001] Aether narrowing warning: 'n' is Int, but the value assigned is a Real literal; the fractional part is discarded silently. Write int(...) if the truncation is intended.
help: see NARROW-001 in the Aether guide
3
```

The warning goes to standard error, before the program runs; the `3` is the
program's output.

Write `int(3.7)` when truncating is what you mean, and the warning goes away.

---

## 6. Variables and constants

```aether
const LIMIT: Int = 3;

fn main() -> Void {
    let count = 0;                  // inferred: Int
    let label: Text = "passing";    // declared
    let scores: Int[] = [70, 85, 92, 64];
    loop s in scores {
        if s >= 80 {
            count = count + 1;      // every let is mutable
        }
    }
    count += LIMIT;                 // the same as count = count + LIMIT
    fx { println(count, " ", label); }
}
```

```text
5 passing
```

- `let` declares a variable, and every variable can be reassigned. (`let mut`
  is accepted and means the same thing.) `const` declares a value that cannot
  change.
- `let` and `const` may also appear at the top level of a file, outside any
  function, where every function can see them.
- `+=`, `-=`, `*=`, `/=` and `%=` are shorthand for the assignment they spell.

### When to write the type

The compiler infers a variable's type from a literal (`42`, `3.5`, `"x"`,
`true`), from a call to a function whose return type is declared, and from
`new T { ... }`. It cannot infer the type of an array or map literal, so those
always need one:

```aether @reject=TYPE-001
fn main() -> Void {
    let xs = [1, 2, 3];
    fx { println(length(xs)); }
}
```

```text
prog.aether:2: [TYPE-001] Aether declaration parser error: cannot infer the type of 'xs' from its initializer.
hint: write the array type: `let xs: Int[] = [...];`.
help: see TYPE-001 in the Aether guide
```

When in doubt, write the type. It costs a few characters and makes the code
easier to read.

### Names

Names are case-sensitive: `count` and `Count` are different spellings, and
using one where the other was declared is an error (`SCOPE-001`) that names the
declared spelling. But two names may not differ only by case, because the
engine underneath folds case: `let total` beside `let Total` is `NAME-001`.
Names used to be case-insensitive, a Pascal inheritance; they became
case-sensitive in October 2026 (D70) because every language models learn
from is.

A name means one thing in a block. Within one block, a `let` may not
redeclare a variable; a `let` may not redeclare a parameter; a local may not
reuse the name of a top-level constant; and a loop variable may not reuse the
name of a variable already in scope, since the loop would get its own copy and
leave the outer one unchanged:

```aether @reject=NAME-001
fn main() -> Void {
    let i: Int = 5;
    loop i in 0..3 {
        fx { println(i); }
    }
}
```

```text
prog.aether:3: [NAME-001] Aether redeclaration error: loop variable 'i' reuses the name of 'i' (line 2); the loop gets its own 'i', so the outer one is left unchanged.
hint: pick a new loop variable name, and assign the outer variable inside the loop if it must change.
help: see NAME-001 in the Aether guide
```

A variable is visible from its declaration to the end of the block that holds
it. A `let` inside an inner block (the body of an `if` or a loop) makes a new
variable that hides an outer one of the same name until the block ends. That
is legal, and it is a classic slip: to change the outer variable, assign it
(`total = total + i;`) rather than writing `let` again.

---

## 7. Functions and tuples

```aether
fn add(a: Int, b: Int) -> Int {
    ret a + b;
}

fn greet(name: Text) -> Void {
    fx { println("hello, ", name); }
}

fn main() -> Void {
    greet("Ada");
    fx { println(add(2, 3), " ", fact(10)); }
}

fn fact(n: Int) -> Int {
    if n <= 1 {
        ret 1;
    }
    ret n * fact(n - 1);
}
```

```text
hello, Ada
5 3628800
```

- Every parameter has a type, and every function declares its return type.
- `ret value;` returns a value; a `Void` function may write `ret;` or simply
  end. A function that returns a value must do so on every path, or the
  compiler reports `FLOW-001`.
- Functions may be declared in any order; `main` above calls `fact`, declared
  after it. Recursion works as usual.
- There is no overloading: two functions with the same name are `NAME-001`.
  There are no default or named arguments either.

### Arguments are values, records are shared

A function receives copies of `Int`, `Real`, `Bool` and `Text` arguments, and
of arrays and maps too. A record argument is the record itself, so changes to
its fields are seen by the caller:

```aether
type Counter {
    hits: Int = 0;
}

fn bumpCopy(n: Int) -> Int {
    n = n + 1;
    ret n;
}

fn bumpRecord(c: Counter) -> Void {
    c.hits = c.hits + 1;
}

fn main() -> Void {
    let n: Int = 1;
    let c: Counter = new Counter();
    let m: Int = bumpCopy(n);
    bumpRecord(c);
    bumpRecord(c);
    fx { println(n, " ", m, " ", c.hits); }
}
```

```text
1 2 2
```

A function that changes an array or map argument must return it, and the
caller must store the result (`xs = sortDown(xs);`); [§12](#12-arrays) has the
details and the error that enforces it.

A scalar argument that the function changes is the subtler trap. A parser
helper that advances its `pos` parameter does not move the caller's `pos`, so
the caller keeps reading from the old place. The compiler warns when it can
see the mistake:

```aether @ok warn=PARAM-001
fn readWord(s: Text, pos: Int) -> Text {
    let word: Text = "";
    loop pos < length(s) && s[pos] != " " {
        word = word + s[pos];
        pos = pos + 1;
    }
    pos = pos + 1;              // step past the space: lost on return
    ret word;
}

fn main() -> Void {
    let line: Text = "hello world";
    let pos: Int = 0;
    let first: Text = readWord(line, pos);
    let second: Text = readWord(line, pos);
    fx { println(first, " ", second); }
}
```

```text
prog.aether:7: warning: [PARAM-001] Aether parameter warning: parameter 'pos' of 'readWord' is a copy: this update is never read again, and the caller never sees it. Return it with the result (`-> (Text, Int)`) and assign it at the call (`let (value, next) = readWord(...); pos = next;`), or make 'readWord' a nested fn that assigns the enclosing 'pos', or keep the cursor in a record field.
help: see PARAM-001 in the Aether guide
hello hello
```

The program compiles, warns, and prints `hello hello`: the second call starts
from the same `pos` as the first. The warning names three fixes. The first is
to return the new position with the result, as a tuple:

```aether
fn readWord(s: Text, pos: Int) -> (Text, Int) {
    let word: Text = "";
    loop pos < length(s) && s[pos] != " " {
        word = word + s[pos];
        pos = pos + 1;
    }
    ret (word, pos + 1);
}

fn main() -> Void {
    let line: Text = "hello world";
    let (first, next) = readWord(line, 0);
    let (second, after) = readWord(line, next);
    fx { println(first, " ", second, " ", after); }
}
```

```text
hello world 12
```

The second is to keep the cursor in a record field, since records are shared.
The third is to write the helpers as nested functions that share the cursor,
which the next section shows.

### Nested functions

A function declared inside another function's body can read and assign that
function's parameters and the locals declared before it:

```aether
fn sumOf(expr: Text) -> Int {
    let pos: Int = 0;
    fn number() -> Int {
        let value: Int = 0;
        loop pos < length(expr) && is_digit(expr[pos]) {
            value = value * 10 + ord(expr[pos]) - ord("0");
            pos = pos + 1;              // moves sumOf's pos
        }
        ret value;
    }
    let total: Int = number();
    loop pos < length(expr) && expr[pos] == "+" {
        pos = pos + 1;
        total = total + number();
    }
    ret total;
}

fn main() -> Void {
    fx { println(sumOf("12+30+0"), " ", sumOf("7")); }
}
```

```text
42 7
```

Nesting goes one level deep, and only inside free functions (not methods). A
nested function is not a value either: it cannot be stored, passed or
returned.

### Tuples

A function can return several values as a tuple, which the caller unpacks
straight away or binds to one name and reads by position:

```aether
fn divmod(a: Int, b: Int) -> (Int, Int) {
    ret (a div b, a % b);
}

fn main() -> Void {
    let (q, r) = divmod(17, 5);
    let t = divmod(9, 4);
    fx { println(q, " ", r, " ", t.0, " ", t.1); }
}
```

```text
3 2 2 1
```

Tuples are deliberately narrow: a tuple is a return type, unpacked or bound
directly from the call. It cannot be a parameter, a field or an array element,
and `divmod(9, 4).0` must be written as two steps:

```aether @reject=TUP-001
fn divmod(a: Int, b: Int) -> (Int, Int) {
    ret (a div b, a % b);
}

fn main() -> Void {
    fx { println(divmod(9, 4).0); }
}
```

```text
prog.aether:6: [TUP-001] Aether tuple parser error: tuple index .0 access is only supported on a variable, not directly on a call result.
hint: bind the call first, for example `let t = pair(); t.0;`.
help: see TUP-001 in the Aether guide
```

When you need more than that, return a record.

### No function values

Functions are not values. There are no lambdas, no closures and no callbacks,
and `let f = add;` is `FUNC-001`. There is no `map` or `filter` either; write
the loop. To choose between functions, branch with `if` and call each one. The
standard helpers that would otherwise take a function, sorting chief among
them, take a field name instead (`sorted_by`, [§8](#8-records-and-methods)).

---

## 8. Records and methods

A record type groups named fields and the methods that work on them:

```aether
type Account {
    owner: Text;
    balance: Int = 0;
    history: Int[] = [];

    fn deposit(amount: Int) -> Void {
        self.balance = self.balance + amount;
        self.history = self.history + [amount];
    }

    fn summary() -> Text {
        ret self.owner + ": " + int_to_text(self.balance) + " after "
            + int_to_text(length(self.history)) + " deposits";
    }
}

fn main() -> Void {
    let acct: Account = new Account { owner: "ann" };
    acct.deposit(50);
    acct.deposit(25);
    let empty: Account = new Account();
    fx {
        println(acct.summary());
        println("[", empty.owner, "] ", empty.balance);
    }
}
```

```text
ann: 75 after 2 deposits
[] 0
```

- Fields are `name: Type;`. A field may have a default, which must be a
  literal (`0`, `""`, `false`, `[]`). Not an expression, not a call, and not a
  named constant (`FIELD-003`). A field without a default starts at its type's
  zero: `0`, `0.0`, `""`, `false`, an empty array.
- `new T { field: value, ... }` constructs a record and sets the fields you
  list; the others keep their defaults. `new T()` sets none.
- Methods are declared inside the type. Inside a method, `self` is the record;
  fields and other methods are always reached through it (`self.balance`,
  `self.summary()`). Never declare `self` as a parameter.
- There are no constructor methods. For construction with logic, write an
  ordinary function such as `fn makeAccount(owner: Text) -> Account`.
- A method sees only its parameters and `self`, not the local variables of
  whoever calls it. Pass what it needs.
- A field or method may not be named after a keyword (`new`, `loop`, `type`,
  `div`, `and`, ...). Words that other languages reserve, such as `match`,
  `class` or `text`, are ordinary names here.

### Records are shared, compared by value

A record variable refers to the record, so assigning it or passing it shares
the same record. `==` compares two records field by field; `same(a, b)` asks
whether they are the same record:

```aether
type Point {
    x: Int = 0;
    y: Int = 0;
}

fn makePoint(x: Int, y: Int) -> Point {
    ret new Point { x: x, y: y };
}

fn main() -> Void {
    let a: Point = makePoint(1, 2);
    let b: Point = a;                   // the same record, not a copy
    b.x = 10;
    let c: Point = makePoint(10, 2);
    fx { println(a.x, " ", a == c, " ", same(a, b), " ", same(a, c)); }
}
```

```text
10 true true false
```

To copy a record, construct a new one from its fields. A record type that
contains itself (`next: Node`) cannot be compared with `==` (`REC-001`); use
`same`.

### Arrays of records

An array of records sorts by one of its fields. `sorted_by` and
`sorted_by_desc` take the field's name as a string literal, which the compiler
checks against the record type:

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
    fx {
        print("by score:");
        loop p in sorted_by_desc(ps, "score") {
            print(" ", p.name, "=", p.score);
        }
        println("");
        print("by team, then score:");
        loop p in sorted_by(sorted_by_desc(ps, "score"), "team") {
            print(" ", p.team, "/", p.name);
        }
        println("");
    }
}
```

```text
by score: bob=9 cy=9 ann=7
by team, then score: blue/bob red/cy red/ann
```

The sorts are stable, so to sort by two keys, sort by the minor key first and
the major key second, as the second loop does.

---

## 9. Control flow and loops

### Conditions

```aether
fn grade(score: Int) -> Text {
    if score >= 90 {
        ret "A";
    } else if score >= 80 {
        ret "B";
    }
    ret "C";
}

fn main() -> Void {
    let n: Int = 12;
    let size: Text = if n > 10 { "big" } else { "small" };
    fx { println(grade(95), grade(85), grade(50), " ", size); }
}
```

```text
ABC big
```

Parentheses around a condition are optional; braces around each body are the
normal form. `if` also works as an expression, wherever a value is expected,
`else if` chains included. There is no `match` or `switch`, and the diagnostic says what
to write instead:

```aether @reject=SYN-001
fn main() -> Void {
    let day: Int = 3;
    match day {
        1 => fx { println("Monday"); }
    }
}
```

```text
prog.aether:3: [SYN-001] Aether parser error: 'match' is not Aether syntax.
hint: there is no match or switch statement; use an `if ... else if ... else` chain.
help: see SYN-001 in the Aether guide
```

### Loops

`loop` is the one loop keyword, in four forms:

```aether
fn main() -> Void {
    let words: Text[] = ["red", "green", "blue"];
    let n: Int = 1;
    fx {
        print("range:");
        loop i in 0..5 { print(" ", i); }
        println("");
        print("step:");
        loop i in 10..0 step -3 { print(" ", i); }
        println("");
        print("each:");
        loop w in words { print(" ", w); }
        println("");
        loop n < 100 { n = n * 3; }
        println("condition: ", n);
        print("odd, below 10:");
        let k: Int = 0;
        loop {
            k = k + 1;
            if k >= 10 { break; }
            if k % 2 == 0 { continue; }
            print(" ", k);
        }
        println("");
    }
}
```

```text
range: 0 1 2 3 4
step: 10 7 4 1
each: red green blue
condition: 243
odd, below 10: 1 3 5 7 9
```

- **Range:** `loop i in a..b` counts from `a` up to `b - 1`; the range is
  half-open, so `1..n + 1` is 1 through `n`. `step k` changes the stride, and
  a negative step counts down. The loop declares `i` itself: do not `let` it
  first.
- **Each:** `loop x in collection` visits the elements of an array, the
  characters of a `Text`, the keys of a map, or the elements of a JSON array.
- **Condition:** `loop cond { ... }` runs while `cond` is true.
- **Forever:** `loop { ... }` runs until `break` (or `ret`).
- `break` leaves the innermost loop; `continue` skips to its next round.

`while cond { }` and `for i in a..b { }` are accepted as other spellings of the
condition and range forms, since models from other languages write them.

### Quantifiers

`all x in xs: cond` and `any x in xs: cond` are `Bool` expressions over the same
collections a loop takes, ranges included:

```aether
fn main() -> Void {
    let xs: Int[] = [3, 8, 1];
    let ascending: Bool = all i in 0..length(xs) - 1: xs[i] <= xs[i + 1];
    fx {
        println(ascending, " ", any x in xs: x > 5, " ", all x in xs: x > 0);
        if any c in "hello": c == "l" {
            println("has an l");
        }
    }
}
```

```text
false true true
has an l
```

The condition runs to the end of the expression, so put parentheses around a
quantifier inside a larger condition: `(all x in xs: x > 0) && n > 0`. Both
stop as soon as the answer is known; over an empty collection, `all` is true
and `any` is false. They are most useful in contracts ([§14](#14-contracts)).

---

## 10. Effects and `fx`

An effect is anything a program does to, or learns from, the world outside
itself. In Aether these builtins are effects, and a call to one must sit inside
an `fx { ... }` block:

- output: `print`, `println`
- files and directories: `read_file`, `write_file`, `file_exists`, `mkdir`, ...
- standard input: `read_line`, `read_all_stdin`
- the environment and command line: `get_env`, `args`
- the clock and sleeping: `clock_ms`, `now_text`, `sleep`
- randomness: `random`, `randomize`
- the network: `http_*`, `socket_*`, `dns_lookup`
- tasks and threads, `ai_chat`, and the database (`sqlite_*`)

Arithmetic, text, arrays, maps, conversions and the JSON accessors are not
effects, and work anywhere. The appendix of the full guide marks every
effectful builtin with *fx*.

```aether @reject=FX-001
fn main() -> Void {
    let total: Int = 3;
    println("total = ", total);
}
```

```text
prog.aether:3: [FX-001] Aether effect error: call to 'println' requires an fx block.
help: see FX-001 in the Aether guide
```

An `fx` block may hold any statements, loops and conditions included, so one
block can cover a whole loop that prints. A good shape for most programs is to
compute first, with no effects, and then act in one place at the end, as the
tour program in [§4](#4-a-short-tour) does.

`fx` is local. It marks the call sites in the function where they are written,
and nothing more: a function that contains its own `fx` block can be called
from anywhere, and the caller does not need an `fx` of its own:

```aether
fn report(label: Text, value: Int) -> Void {
    fx { println(label, ": ", value); }
}

fn main() -> Void {
    report("answer", 42);
}
```

```text
answer: 42
```

The guarantee that does travel through calls is `@pure`. A function marked
`@pure` may not contain an `fx` block, call an effectful builtin, or call any
function that is not itself `@pure`, and the compiler checks all three:

```aether @reject=ANN-001
fn logLine(msg: Text) -> Void {
    fx { println(msg); }
}

@pure
fn twice(n: Int) -> Int {
    logLine("doubling");
    ret n * 2;
}

fn main() -> Void {
    fx { println(twice(4)); }
}
```

```text
prog.aether:7: [ANN-001] Aether purity error: pure function 'twice' cannot call non-pure function 'logLine'.
help: see ANN-001 in the Aether guide
```

**Why mark effects at all?** A reader, or a reviewer of model-written code,
can find every place a program touches the outside world by searching for
`fx`, and can trust that a `@pure` function only computes. The marker costs a
few braces. At run time, the `--deny` sandbox ([§21](#21-the---deny-sandbox-and-exit-status))
can refuse whole classes of effects.

---

## 11. Text

A `Text` is a sequence of Unicode characters. Positions count characters, not
bytes, starting at 0, and slices are half-open like ranges:

```aether
fn main() -> Void {
    let s: Text = "Hello, World";
    fx {
        println(length(s), " ", s[0], " ", s[7..], " ", s[..5], " ", s[7..9]);
        println(upper(s), " ", lower(s), " ", reversed("abc"), " ", length("héllo"));
        println(contains(s, "World"), " ", starts_with(s, "He"), " ", ends_with(s, "!"));
        println(index_of(s, "o"), " ", index_of(s, "xyz"), " ", count(s, "o"));
        println(replace(s, "World", "Aether"), " | ", trim("  padded  "), " | ", repeat("-", 5));
        println(join(split("a,b,,c", ","), "/"), " ", length(split("a,b,,c", ",")));
        println(ord("A"), " ", chr(97), " ", is_digit("123"), " ", is_digit(""), " ", is_alpha("x1"));
        println("apple" < "banana", " ", "Zebra" < "apple", " ", "a" == "a");
    }
}
```

```text
12 H World Hello Wo
HELLO, WORLD hello, world cba 5
true true false
4 -1 2
Hello, Aether | padded | -----
a/b//c 4
65 a true false false
true true true
```

| Builtin | Result |
|---|---|
| `length(s)` | the number of characters |
| `s[i]` | the character at `i`, as a one-character `Text`; out of range stops the program |
| `s[a..b]`, `s[a..]`, `s[..b]` | the characters from `a` up to `b - 1`; a slice past the end is cut short, never an error |
| `loop c in s` | each character in turn |
| `contains(s, x)`, `starts_with(s, x)`, `ends_with(s, x)` | `Bool` |
| `index_of(s, x)` | the first position of `x`, or `-1` |
| `count(s, x)` | how many times `x` occurs, without overlaps |
| `upper(s)`, `lower(s)` | ASCII letters mapped; other characters unchanged |
| `trim(s)` | leading and trailing whitespace removed |
| `replace(s, old, new)` | every `old` replaced, left to right |
| `split(s, sep)`, `join(parts, sep)` | `Text` to `Text[]` and back |
| `repeat(s, n)`, `reversed(s)` | `s` written `n` times; `s` backwards |
| `is_digit`, `is_alpha`, `is_alnum`, `is_space`, `is_upper`, `is_lower` | `true` when the text is non-empty and every character is in the ASCII class |
| `ord(c)`, `chr(n)` | a character's code, and back |

Text compares with `==`, `!=` and `<` (by character code, so capitals sort
before lower case). Every text helper is a function: `upper(s)`, not
`s.upper()`. A method-style call on a builtin type is a `SCOPE-001` error whose
hint names the function to call.

### Building text

`+` joins two `Text` values. When the result is declared `Text`, a number
joined to text is converted, left to right. `format_float` fixes the decimals,
and `repeat` pads:

```aether
fn padLeft(s: Text, width: Int) -> Text {
    ret repeat(" ", width - length(s)) + s;
}

fn main() -> Void {
    let price: Real = 3.14159;
    let qty: Int = 12;
    let line: Text = "qty=" + qty + " price=" + format_float(price, 2);
    fx {
        println(line);
        println("[", padLeft(int_to_text(qty), 5), "]");
        println(price:0:3, " [", price:8:2, "]");
        println(real_to_text(2.5), " ", int_to_text(-4) + "!");
    }
}
```

```text
qty=12 price=3.14
[   12]
3.142 [    3.14]
2.5 -4!
```

Inside `print` and `println` only, `value:width:precision` formats a number:
width `0` means no padding. `println` has no placeholders. `"{}"`, `%d` and
`f"..."` are `FMT-001` errors, because in other languages they mean
substitution and here they would print literally. Pass the values as separate
arguments instead.

`println` puts nothing between its arguments: `println(1, 2)` prints `12`.

### Turning text into numbers

`try_parse_int` and `try_parse_real` return the value and whether the *whole*
text was a number. Use them for anything that arrives from outside the
program. `parse_int` and `parse_float` never fail: they read a leading number
and return `0` when there is none, so a `0` cannot tell you whether the input
was valid. `parse_bool` is `true` for `true`, `1`, `yes` or `t`, and `false`
for anything else:

```aether
fn main() -> Void {
    let inputs: Text[] = ["42", " -7 ", "12x", "", "3.5"];
    fx {
        loop raw in inputs {
            let (n, ok) = try_parse_int(raw);
            println("[", raw, "] try_parse_int: ", n, " ", ok, ", parse_int: ", parse_int(raw));
        }
        let (r, rok) = try_parse_real("2.5e3");
        println(r, " ", rok, " ", parse_bool("yes"), " ", parse_float("2.5kg"));
    }
}
```

```text
[42] try_parse_int: 42 true, parse_int: 42
[ -7 ] try_parse_int: -7 true, parse_int: -7
[12x] try_parse_int: 0 false, parse_int: 12
[] try_parse_int: 0 false, parse_int: 0
[3.5] try_parse_int: 0 false, parse_int: 3
2500.0 true true 2.5
```

---

## 12. Arrays

An array `T[]` holds values of one type and grows by appending:

```aether
fn main() -> Void {
    let xs: Int[] = [5, 3, 9];
    xs = xs + [1];                  // append one
    xs = xs + [4, 6];               // append several
    let ys: Int[] = sorted(xs);     // a sorted copy; xs is unchanged
    let firstTwo: Int[] = ys[..2];  // a slice is a copy
    let counts: Int[] = [0; 3];     // three zeros
    counts[1] = 7;
    fx {
        println(length(xs), " ", xs[0], " ", xs[length(xs) - 1]);
        print("sorted:");
        loop y in ys { print(" ", y); }
        println("");
        println(sum(xs), " ", min_of(xs), " ", max_of(xs), " ", index_of(xs, 9), " ", contains(xs, 2));
        println(length(firstTwo), " ", firstTwo[1], " ", counts[0], counts[1], counts[2], " ", xs == ys);
    }
}
```

```text
6 5 6
sorted: 1 3 4 5 6 9
28 1 9 2 false
2 3 070 false
```

- Declare the type: `let xs: Int[] = [];`. `[]` is the empty array; `[v; n]`
  is `n` copies of `v`.
- `xs = xs + [v];` appends; `xs + ys` concatenates. There is no resize call:
  grow by appending and shrink by slicing (`xs = xs[..n];`).
- `xs[i]` reads and `xs[i] = v;` writes. An index outside the array stops the
  program with `ARR-003` (below).
- `xs[a..b]` is a copy of elements `a` to `b - 1`; either bound may be left
  out. A negative start, a start after the end, or an end past `length(xs)`
  stops the program with an `ARR-003` that names the slice bound; a `Text`
  slice is cut short instead.
- `==` compares two arrays element by element.
- The helpers `sum`, `min_of`, `max_of`, `sorted`, `sorted_desc`,
  `reversed`, `contains`, `index_of` and `count` work on `Int[]`, `Real[]` and
  `Text[]`, and return new values. `sorted_by` sorts records
  ([§8](#8-records-and-methods)).
- `println` cannot print a whole array (`TYPE-001`); loop over it, or `join` a
  `Text[]`.

### Arrays of arrays

A table is an array of arrays. Rows are independent, so they may have
different lengths:

```aether
fn main() -> Void {
    let grid: Int[][] = [[0; 3]; 2];
    grid[1][2] = 5;
    let snapshot: Int[][] = grid;       // a full copy, rows included
    snapshot[0][0] = 9;
    let jagged: Int[][] = [[1], [2, 3], []];
    fx {
        loop row in grid {
            loop v in row { print(v); }
            println("");
        }
        println(grid[0][0], " ", snapshot[0][0], " ", length(jagged[1]), " ", length(jagged[2]));
    }
}
```

```text
000
005
0 9 2 0
```

When you loop over a table, bound the inner loop with the row's own length,
`length(table[r])`, not the first row's.

### Arrays are values

Assigning an array, or passing it to a function, copies it. That is the
opposite of records, and it has a consequence: a function that changes an array
it was given changes only its own copy. The compiler rejects that, rather than
let the change silently vanish:

```aether @reject=ARR-001
fn addBonus(scores: Int[]) -> Void {
    loop i in 0..length(scores) {
        scores[i] = scores[i] + 5;
    }
}

fn main() -> Void {
    let scores: Int[] = [70, 80];
    addBonus(scores);
    fx { println(scores[0]); }
}
```

```text
prog.aether:3: [ARR-001] Aether array-mutation error: array parameter 'scores' has an element written here, but the function never returns it. Arrays are values: the caller keeps its own copy and never sees this change. Return it and reassign: `xs = f(xs)`. For scratch work, copy it to a local first (`let work: Int[] = scores;`).
help: see ARR-001 in the Aether guide
```

Why not pass arrays by reference, like records? Because then `let ys = xs;`
would copy while a parameter would alias: two rules for one type. And the
error is there because the silent version bit: fine-tuned models wrote in-place
heap sorts whose writes vanished, and printed wrong answers without a word.
As an error, one repair round fixes it (decision D8).

Return the array and store the result:

```aether
fn addBonus(scores: Int[]) -> Int[] {
    loop i in 0..length(scores) {
        scores[i] = scores[i] + 5;
    }
    ret scores;
}

fn main() -> Void {
    let scores: Int[] = [70, 80];
    scores = addBonus(scores);
    fx { println(scores[0], " ", scores[1]); }
}
```

```text
75 85
```

Throwing away an array that a call returns, such as a bare `sorted(xs);`
statement, is `ARR-001` too: the call changes nothing you can see.

### Out of range

```aether @trap=ARR-003
fn main() -> Void {
    let xs: Int[] = [10, 20, 30];
    fx { println(xs[3]); }
}
```

```text
prog.aether:3: [ARR-003] array index 3 is out of bounds in dimension 1 of 1: valid indices are 0..2.
hint: the index is past the end of what has been appended so far -- check the loop bound, and whether you meant the row currently being built rather than one already stored.
help: see ARR-003 in the Aether guide
[Error Location] Offset: 36, Line: 3
```

The message gives the index, the valid range and a hint. `valid indices are
0..2` means the array has three elements. When you build a table row by row,
the usual cause is reading `table[i]` while row `i` is still the local you are
building and has not been appended yet.

---

## 13. Maps

`Map[Text, T]` maps text keys to values of one type `T`, which may be any type,
records and arrays included:

```aether
fn wordCounts(words: Text[]) -> Map[Text, Int] {
    let counts: Map[Text, Int] = {};
    loop w in words {
        counts[w] = get(counts, w, 0) + 1;
    }
    ret counts;
}

fn main() -> Void {
    let counts: Map[Text, Int] = wordCounts(split("the cat and the hat", " "));
    let ages: Map[Text, Int] = {"ann": 31, "bob": 27};
    ages["cid"] = 40;
    remove(ages, "bob");
    fx {
        println(counts["the"], " ", length(counts), " ", has(counts, "dog"), " ", "cat" in counts);
        loop k in ages {
            println(k, " is ", ages[k]);
        }
        println(join(keys(counts), ","), " ", ages);
    }
}
```

```text
2 4 false true
ann is 31
cid is 40
the,cat,and,hat {"ann": 31, "cid": 40}
```

- `{}` is an empty map and needs a declared type; `{"a": 1}` is a literal.
- `m[k] = v` inserts or replaces. `m[k]` reads a key that must be present.
  `get(m, k, default)` reads with a fallback, and `has(m, k)` (or `k in m`,
  `k not in m`) tests.
- `remove(m, k)` deletes a key, and does nothing if it is absent.
- `keys(m)` and `values(m)` are arrays in insertion order, and `loop k in m`
  visits the keys in that order. Replacing a value keeps its key's place. For
  sorted output, loop over `sorted(keys(m))`.
- `println(m)` prints a map of plain values in insertion order.
- Like arrays, maps are values: assigning or passing one copies it, so a
  function that changes a map returns it.
- There is no set type. A set of names is a `Map[Text, Bool]`.

Reading a missing key stops the program and says how to avoid it:

```aether @trap
fn main() -> Void {
    let ages: Map[Text, Int] = {"ann": 31};
    fx { println(ages["bob"]); }
}
```

```text
prog.aether:3: Runtime Error: key "bob" is not in the map; use has(ages, "bob") or get(ages, "bob", default).
[Error Location] Offset: 44, Line: 3
```

A map can group values under a key. Each group is an array, created the first
time its key appears:

```aether
let groups: Map[Text, Text[]] = {};
loop w in split("apple avocado bean", " ") {
    let initial: Text = w[..1];
    if !has(groups, initial) {
        groups[initial] = [];
    }
    groups[initial] = groups[initial] + [w];
}
```

---

## 14. Contracts

Three annotations go directly above a function:

- `@pre condition`: checked when the function is called.
- `@post condition`: checked when it returns, with the return value available
  as `result`.
- `@pure`: checked at compile time. The function has no effects and calls only
  pure functions ([§10](#10-effects-and-fx)).

A function may stack several `@pre` and `@post` lines. A broken one stops the
program with `CON-001` and exit status 1:

```aether @trap=CON-001
@pre n >= 0
@post result * 2 <= n
fn half(n: Int) -> Int {
    ret n div 2;
}

fn main() -> Void {
    fx {
        println(half(10));
        println(half(-4));
    }
}
```

```text
5
[CON-001] Aether @pre failed in half
```

The message names the function whose contract broke. A `@pre` failure means
the caller passed a bad argument; a `@post` failure means the function computed
the wrong result. Fix the code, unless it is the contract that is wrong.

Contracts over arrays state a property, such as a length or, with a
quantifier, something true of every element:

```aether
@pre length(xs) > 0
@pre all x in xs: x >= 0
@post result >= 0
@cost 5ms
@pure
fn mean(xs: Int[]) -> Int {
    ret sum(xs) div length(xs);
}

@post result.0 <= result.1
@pure
fn ordered(a: Int, b: Int) -> (Int, Int) {
    if a <= b {
        ret (a, b);
    }
    ret (b, a);
}

type Stock {
    count: Int = 0;

    @pre amount > 0
    @post self.count >= 0
    fn take(amount: Int) -> Void {
        self.count = self.count - amount;
    }
}
```

- A tuple-returning function names its result's slots `result.0`,
  `result.1`, and so on.
- A method declared inside its type may use `self` in its contracts.
- `@cost 5ms` (units `ns`, `us`, `ms`, `s`, `op`, `ops`, `step`, `steps`)
  records an intended budget. Its form is checked, but nothing measures it:
  treat it as documentation.

Annotations sit directly above the function they describe (inside a body, only
above a nested function); one anywhere else is `ANN-001`.

---

## 15. Errors and diagnostics

### Reading a diagnostic

```aether @reject=SYN-001 file=report.aether
fn twice(n: Int) -> Int {
    return n * 2;
}

fn main() -> Void {
    fx { println(twice(21)); }
}
```

```text
report.aether:2: [SYN-001] Aether parser error: 'return' is not Aether syntax.
hint: Aether returns with `ret`: write `ret value;`, or bare `ret;` in a Void function.
help: see SYN-001 in the Aether guide
```

A compile error has up to four parts:

1. `report.aether:2:` is the file and the line, in your source.
2. `[SYN-001]` is the code. Each code names one kind of mistake, and codes are
   stable: a code you learn keeps its meaning.
3. The message says what is wrong at this site, naming the words involved.
4. `hint:` (when present) says what to write instead, and `help:` points to the
   code's entry in the repair rules of the full guide.

Some messages carry the fix in the message itself:

```aether @reject=SCOPE-001 file=report.aether
fn main() -> Void {
    let count: Int = 3;
    fx { println("items: ", Count); }
}
```

```text
report.aether:3: [SCOPE-001] Aether scope error: identifier 'Count' not in scope: names are case-sensitive, and the one declared is 'count'. Did you mean `count`?
help: see SCOPE-001 in the Aether guide
```

The compiler reports as many errors as it can find in one run. A warning has
the same shape, with `warning:` before the code, and does not stop the program
from compiling and running.

The codes you will meet most often:

| Code | Means | Usual fix |
|---|---|---|
| `SYN-001` | not Aether syntax (`return`, `class`, `match`, a keyword as a field name) | the form the hint names |
| `SCOPE-001` | a name that is not declared, not visible here, or spelled with different case | declare it, pass it in, or fix the spelling |
| `NAME-001` | a name declared twice, or two names that differ only by case | pick a new name, or assign instead of `let` |
| `TYPE-001` | a type that cannot be inferred, or a value of the wrong kind | write the type, or convert |
| `TYPE-002` | a type name that does not exist (`Double`, `List`) | the Aether type the message names |
| `FX-001` | an effect outside an `fx` block | wrap it in `fx { }` |
| `ANN-001` | a misplaced annotation, or a `@pure` function with effects | move it; keep effects out of pure code |
| `FLOW-001` | a function that can end without returning a value | add the missing `ret` |
| `ARR-001` | an array or map argument changed but not returned | return it and reassign |
| `TUP-001` | a tuple used outside its narrow forms | unpack the call, or return a record |
| `FIELD-002`, `FIELD-003` | an unknown field; a field default that is not a literal | the declared name; set it at construction |
| `PAR-001`, `PAR-002` | one record shared by two `par` branches; a non-call in `par` | a record per branch; call a function |
| `FMT-001` | a `{}`, `%d` or `f"..."` placeholder | pass values as `println` arguments |
| `DIV-002` | `x // 2` | `x div 2`; `//` starts a comment |
| `ENTRY-001` | no runnable entry point, or `main` beside top-level statements | the structure the hint names |
| `NARROW-001` (warning) | a `Real` stored in an `Int` | `int(...)` if you mean it |
| `PREC-001` (warning) | `flags & mask != 0` | `(flags & mask) != 0` |
| `PARAM-001` (warning) | a changed scalar parameter the caller will not see | return it, or share it |

The full list, with every rule each code covers, is the "Repair rules"
section of the [full guide](aether_for_llms_and_others.md#repair-rules).

A warning in action:

```aether @ok warn=PREC-001
fn main() -> Void {
    let flags: Int = 6;
    let mask: Int = 1;
    fx {
        println(flags & mask != 0);
        println((flags & mask) != 0);
    }
}
```

```text
prog.aether:5: warning: [PREC-001] Aether precedence warning: '&' binds looser than '!=', so this parses as `a & (b != c)` and produces an Int, not a Bool.
hint: parenthesize the mask: `(a & b) != c`. As written the comparison runs first and its result is combined bitwise, which compiles and prints a number.
help: see PREC-001 in the Aether guide
0
false
```

The first line prints `0`: it computed `6 & (1 != 0)`, which is `6 & 1`.

### Run-time errors

A program that compiles can still stop at run time. Some run-time errors carry
a code, and often a hint, like compile errors: `ARR-003` for an array index out
of range, `CON-001` for a broken contract, `AETH-RUNTIME-INT-OVERFLOW` and
`AETH-RUNTIME-DIV-ZERO` for arithmetic, and `AETH-RUNTIME-...` codes for
builtins given bad input, such as unparsable JSON ([§19](#19-toon-and-json)).
Others come from the shared VM and have only a message so far: a missing map
key, a `Text` index out of range.
Either way the program stops with exit status 1, and the message names the
source line (for a contract, the function).

What the compiler cannot catch is a program that runs and prints the wrong
answer. Contracts ([§14](#14-contracts)) are the tool for that: state what a
function promises, and a wrong result stops the program instead of flowing on.

### Diagnostics for tools

`--diagnostics-json` prints compile errors as a JSON array of records, and
`--diagnostics-toon` as TOON, for an editor, a script, or the repair loop of a
model that writes Aether:

```aether @reject=FX-001 flags=--diagnostics-json
fn main() -> Void {
    println("hi");
}
```

```text
[
  {"severity":"error","phase":"semantic","kind":"effect","code":"FX-001","file":"prog.aether","line":2,"column":null,"message":"Aether effect error: call to 'println' requires an fx block.","hint":"see FX-001 in the Aether guide","raw":"prog.aether:2: [FX-001] Aether effect error: call to 'println' requires an fx block."}
]
```

Feeding a diagnostic back to the model that wrote the program is how the
benchmark's repair turns work, and the reason every message tries to name its
fix.

---

## 16. Modules

A module is a file holding one `mod` block. Names marked `export` are visible
to programs that `use` it; the rest are private. Here is a module named
`Geometry`, saved as a file named `geometry`:

```aether
mod Geometry {
    export const Sides: Int = 4;

    @pure
    export fn area(width: Int, height: Int) -> Int {
        ret width * height;
    }

    @pure
    export fn perimeter(width: Int, height: Int) -> Int {
        ret 2 * (width + height);
    }
}
```

A program in the same directory imports it with `use` and calls its exports
by name, or qualified with the module's name:

```aether
use "geometry";

fn main() -> Void {
    let a: Int = area(3, 4);
    fx { println(a, " ", Geometry.perimeter(3, 4), " ", Sides); }
}
```

```text
12 14 4
```

How a `use` finds its file:

- `use "geometry";` loads the file named `geometry`, **with no extension**. It
  does not find a file called `geometry.aether`.
- The file is found next to the file that imports it, or in one of the
  directories listed in the `REA_IMPORT_PATH` environment variable (separated
  by `:`).
- By convention the file is named after the module in snake_case: `mod
  Geometry` in `geometry`, `mod ScoreUtils` in `score_utils`.
- Names are used exactly as exported; `use` does not rename.

A `use` whose file cannot be found is ignored without a word, and its names
then fail as undeclared:

```aether @reject=SCOPE-001
use "geometri";

fn main() -> Void {
    fx { println(area(3, 4)); }
}
```

```text
prog.aether:4: [SCOPE-001] identifier 'area' not in scope.
```

When you see `SCOPE-001` for a name you expected a module to provide, run
again with `--verbose-compat`, which adds a line such as
`prog.aether:1: warning: [IMP-001] Aether ignored missing import 'geometri'.`

A module follows the same rules as the file that uses it: its effects need
`fx`, its functions may not change an array argument they do not return, and a
`@pure` function may not call an export that has effects.

---

## 17. Files, environment and arguments

Every builtin in this chapter is an effect, so it goes inside `fx`. There are
no file handles to open or close: each operation is one call on a path.

```aether
fn main() -> Void {
    fx {
        write_file("notes.txt", "first line\n");
        append_file("notes.txt", "second line\n");
        loop line in read_lines("notes.txt") {
            println("> ", line);
        }
        println(length(read_file("notes.txt")), " bytes, exists: ", file_exists("notes.txt"));
        let (text, ok) = try_read_file("missing.txt");
        println("missing.txt: ok=", ok, " text=[", text, "]");
        let removed: Bool = delete_file("notes.txt");
        println("deleted: ", removed, ", exists: ", file_exists("notes.txt"));
    }
}
```

```text
> first line
> second line
23 bytes, exists: true
missing.txt: ok=false text=[]
deleted: true, exists: false
```

| Builtin | Does |
|---|---|
| `read_file(path) -> Text` | the whole file; a file that cannot be read stops the program |
| `read_lines(path) -> Text[]` | its lines, without line endings |
| `try_read_file(path) -> (Text, Bool)`, `try_read_lines(path) -> (Text[], Bool)` | the same, with `false` instead of stopping |
| `write_file(path, text)`, `append_file(path, text)` | create or replace; add to the end. Write the `"\n"` yourself |
| `file_exists(path) -> Bool` | |
| `delete_file(path) -> Bool`, `rename_file(from, to) -> Bool` | `true` on success |
| `mkdir(path) -> Int`, `rmdir(path) -> Int` | `0` on success; `mkdir` of an existing directory is `0` |
| `get_current_dir() -> Text` | |
| `read_line() -> Text`, `read_all_stdin() -> Text` | one line of standard input (`""` at the end), or all that remains |
| `args() -> Text[]` | the program's command-line arguments |
| `get_env(name) -> Text`, `get_env_int(name, fallback) -> Int` | an environment variable (`""` or the fallback when unset) |
| `clock_ms() -> Int`, `now_text() -> Text` | milliseconds since 1970 (for timing); the local time as `YYYY-MM-DD HH:MM:SS` |
| `sleep(ms)` | pause |
| `random(n) -> Int`, `random() -> Real`, `randomize()` | `0` to `n - 1`; `[0, 1)`; seed from the clock |

Paths are relative to the directory you run the program from.

### Arguments

`args()` returns what follows the source file on the command line. This
program, saved as `greet.aether` and run as
`./build/aether greet.aether Ada Grace`, prints:

```aether @ok file=greet.aether args=Ada,Grace
fn main() -> Int {
    let names: Text[] = [];
    fx { names = args(); }
    if length(names) == 0 {
        fx { println("usage: greet NAME..."); }
        ret 2;
    }
    fx {
        loop n in names {
            println("hello, ", n);
        }
    }
    ret 0;
}
```

```text
hello, Ada
hello, Grace
```

Run with no names, it prints the usage line and exits with status 2.

### Standard input

To process all of standard input, read it at once and split it. Saved as
`sum.aether` and run as `printf '3\n4\nx\n5\n' | ./build/aether sum.aether`:

```aether @ok file=sum.aether stdin=3\n4\nx\n5\n
fn main() -> Void {
    let total: Int = 0;
    let skipped: Int = 0;
    fx {
        loop line in split(read_all_stdin(), "\n") {
            if line == "" {
                continue;
            }
            let (n, ok) = try_parse_int(line);
            if ok {
                total = total + n;
            } else {
                skipped = skipped + 1;
            }
        }
        println("total ", total, ", skipped ", skipped);
    }
}
```

```text
total 12, skipped 1
```

The input ends with a line break, so `split` returns an empty last element,
which the loop skips.

### Environment, time and randomness

```aether
fn main() -> Void {
    fx {
        println("[", get_env("AETHER_MANUAL_UNSET"), "] ", get_env_int("AETHER_MANUAL_UNSET", 8));
        let start: Int = clock_ms();
        sleep(20);
        println(clock_ms() - start >= 20, " ", length(now_text()));
        println(random(100), " ", random(100), " ", random(6));
    }
}
```

```text
[] 8
true 19
42 49 5
```

Without a call to `randomize()`, the random numbers are the same on every run,
which makes a program reproducible while you test it. Call `randomize()` once
to seed from the clock.

---

## 18. HTTP and sockets

Networking needs a build with libcurl, which is the default. Every `http_*` and
`socket_*` call is an effect.

### HTTP

An HTTP request goes through a session, and the response body arrives in an
`MStream`, a memory buffer you create first and read as text afterwards. The
program below starts a one-request web server on a local socket and fetches
from it in parallel, so it runs without a network connection:

```aether
type Reply {
    status: Int = 0;
    body: Text = "";
}

// Answer one request on a socket that is already listening.
fn serveOnce(listener: Int) -> Void {
    fx {
        let conn: Int = socket_accept(listener);
        let request: MStream = socket_receive(conn, 4096);
        let firstLine: Text = split(mstream_text(request), "\r\n")[0];
        let body: Text = "{\"path\": \"" + split(firstLine, " ")[1] + "\", \"ok\": true}";
        socket_send(conn, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
            + "Content-Length: " + length(body) + "\r\nConnection: close\r\n\r\n" + body);
        mstream_free(request);
        socket_close(conn);
    }
}

fn fetch(url: Text, out: Reply) -> Void {
    fx {
        let session: Int = http_session();
        let buffer: MStream = mstream_new();
        out.status = http_request(session, "GET", url, "", buffer);
        out.body = mstream_text(buffer);
        mstream_free(buffer);
        http_close(session);
    }
}

fn main() -> Void {
    let port: Int = 23561;
    let listener: Int = 0;
    fx {
        listener = socket_create(0);
        socket_bind(listener, port);
        socket_listen(listener, 1);
    }
    let reply: Reply = new Reply();
    par {
        serveOnce(listener);
        fetch("http://127.0.0.1:" + port + "/status", reply);
    }
    fx {
        socket_close(listener);
        println("status ", reply.status);
        println(reply.body);
    }
}
```

```text
status 200
{"path": "/status", "ok": true}
```

For a real service, `fetch` is the whole client: `http_session()`,
`http_request(session, method, url, body, out)`, `mstream_text(out)`,
`http_close(session)`. Pass `""` as the body of a `GET`, and the request body
for a `POST` or `PUT`. Other useful calls are `http_set_header(session, name,
value)`, `http_error_code(session)` and `http_last_error(session)`.

- `http_request` returns the HTTP status. A 404 or a 503 is still a status,
  with the server's body in `out`.
- When no response arrives at all, it returns a negative number: `-5` when the
  host name cannot be resolved, `-6` when the connection is refused, `-3` on a
  timeout. `http_last_error(session)` holds the message.
- A JSON body pairs with [§19](#19-toon-and-json):
  `toon_parse(mstream_text(out))`.
- Free each `MStream` with `mstream_free` once you have read it.

### Sockets

The socket builtins are a thin layer over BSD sockets, with integer handles:

| Builtin | Does |
|---|---|
| `socket_create(type) -> Int` | `0` for TCP, `1` for UDP; `-1` on failure. A second argument `6` asks for IPv6 |
| `socket_bind(s, port)`, `socket_listen(s, backlog)` | prepare a server socket; `0` on success |
| `socket_accept(s) -> Int` | wait for a client and return its socket |
| `socket_connect(s, host, port)` | connect a client socket; `0` on success |
| `socket_send(s, data) -> Int` | send `Text` or an `MStream`; returns the bytes sent |
| `socket_receive(s, maxlen) -> MStream` | wait for data; an empty result means the peer closed |
| `socket_set_blocking(s, flag)`, `socket_poll(s, ms, flags)` | non-blocking use: poll for readable (`1`) or writable (`2`) |
| `socket_close(s)` | |
| `dns_lookup(host) -> Text` | an address for a host name, `""` on failure |

`socket_accept` and `socket_receive` block until the other side acts. A
program that plays both sides, like the one above, must run them in a `par`
block ([§20](#20-concurrency)), and must create the listening socket before
the `par` block, so the client cannot connect before the server listens.

---

## 19. TOON and JSON

Aether reads structured data through the `toon_*` builtins, and what they read
is JSON text. (TOON is also the name of a compact notation for JSON-shaped
data, which `--diagnostics-toon` writes; the builtins themselves parse JSON.)
Parsing gives a `ToonDoc`, a handle to the whole document, and every position
inside it is a `ToonNode`:

```aether
fn main() -> Void {
    let doc: ToonDoc = toon_parse("{\"service\": {\"name\": \"api\", \"port\": 8080},"
        + " \"replicas\": [{\"zone\": \"a\", \"healthy\": true},"
        + " {\"zone\": \"b\", \"healthy\": false}, {\"zone\": \"c\"}]}");
    let root: ToonNode = toon_root(doc);
    let name: Text = toon_get_text(toon_key(root, "service"), "name");
    let port: Int = toon_get_int_or(root, "service.port", 80);
    let replicas: ToonNode = toon_key(root, "replicas");
    fx {
        println(name, ":", port, " timeout=", toon_get_int_or(root, "service.timeout", 30));
        loop i in 0..toon_len(replicas) {
            let r: ToonNode = toon_at(replicas, i);
            println(toon_get_text(r, "zone"), " healthy=", toon_get_bool_or(r, "healthy", false),
                " has_flag=", toon_has_key(r, "healthy"));
        }
        println(toon_is_obj(root), " ", toon_is_arr(replicas), " ", toon_type(replicas));
    }
    toon_close(doc);
}
```

```text
api:8080 timeout=30
a healthy=true has_flag=true
b healthy=false has_flag=true
c healthy=false has_flag=false
true true array
```

- `toon_root(doc)` is the top-level value. If the JSON starts with `{`, take
  the named array out with `toon_key` before looping; if it starts with `[`,
  loop over the root itself.
- `toon_key(node, key)` and `toon_at(node, i)` move to a field or an element;
  `toon_len(node)` counts an array. (`length` is for Aether arrays and `Text`,
  `toon_len` for JSON arrays; crossing them stops the program at run time.)
- `toon_get_text`, `toon_get_int`, `toon_get_real` and `toon_get_bool` read a
  field. The `_or` forms take a fallback, used when the key is missing, null,
  or of another type. A key may be a dotted path through objects:
  `"service.port"`.
- A missing key never crashes: it gives a missing node, on which every `_or`
  getter returns its fallback and `toon_len` is 0. `toon_has_key` tells a
  missing key from a present one.
- `toon_text_value(node)` and its `_int`, `_real`, `_bool` siblings read a
  node's own value; `toon_is_obj`, `toon_is_arr`, `toon_is_text`, ... test its
  kind.
- `toon_close(doc)` releases the document and every node taken from it.

`toon_parse` is not an effect; `toon_parse_file(path)` reads a file, so it is.
Text that is not valid JSON stops the program:

```aether @trap=AETH-RUNTIME-TOON-PARSE
fn main() -> Void {
    let doc: ToonDoc = toon_parse("{name: 'unquoted'}");
    toon_close(doc);
}
```

```text
prog.aether:2: [AETH-RUNTIME-TOON-PARSE] toon_parse could not parse the input at position 1: unexpected character. hint: the text must be valid JSON; check for unquoted keys, trailing commas, or single quotes.
[Error Location] Offset: 27, Line: 2
```

For a file that might be missing or malformed, `try_toon_parse_file(path)`
returns `(doc, ok)` instead of stopping.

### Writing JSON

There is no JSON writer; build the text. Escape the backslashes and quotes in
any text value you embed:

```aether
@pure
fn jsonText(s: Text) -> Text {
    ret "\"" + replace(replace(s, "\\", "\\\\"), "\"", "\\\"") + "\"";
}

fn main() -> Void {
    let name: Text = "say \"hi\"";
    let count: Int = 3;
    let json: Text = "{\"name\": " + jsonText(name) + ", \"count\": " + count + "}";
    let doc: ToonDoc = toon_parse(json);
    let back: Text = toon_get_text(toon_root(doc), "name");
    toon_close(doc);
    fx { println(json); println(back == name); }
}
```

```text
{"name": "say \"hi\"", "count": 3}
true
```

Reading your output back with `toon_parse`, as the last lines do, is a cheap
check that it is valid JSON.

---

## 20. Concurrency

### `par` blocks

A `par { ... }` block runs the function calls inside it at the same time, and
waits for all of them to finish before the program goes on:

```aether
type Tally {
    total: Int = 0;
}

fn sumRange(from: Int, upTo: Int, out: Tally) -> Void {
    let acc: Int = 0;
    loop i in from..upTo {
        acc = acc + i;
    }
    out.total = acc;
}

fn main() -> Void {
    let low: Tally = new Tally();
    let high: Tally = new Tally();
    par {
        sumRange(0, 500000, low);
        sumRange(500000, 1000000, high);
    }
    fx { println(low.total, " + ", high.total, " = ", low.total + high.total); }
}
```

```text
124999750000 + 374999750000 = 499999500000
```

- A `par` block holds only calls. An assignment, a loop or an `fx` block inside
  it is `PAR-002`; put that work in a function and call it.
- A branch hands back its result through a record argument, since records are
  shared. Each branch must have its own record. Two branches writing one
  record would race, so the compiler refuses it:

```aether @reject=PAR-001
type Tally {
    total: Int = 0;
}

fn addTo(out: Tally, n: Int) -> Void {
    out.total = out.total + n;
}

fn main() -> Void {
    let shared: Tally = new Tally();
    par {
        addTo(shared, 1);
        addTo(shared, 2);
    }
    fx { println(shared.total); }
}
```

```text
prog.aether:13: [PAR-001] Aether par error: record 'shared' is shared by more than one par branch; the spawned threads write it concurrently, which races (heap corruption).
hint: give each par branch its own record, then combine the results after the `par { ... }` block.
help: see PAR-001 in the Aether guide
```

- The branches may print, but their lines interleave in no fixed order. When
  order matters, have each branch store its result and print after the block.
- Each thread draws its own random numbers, so branches that call `random` do
  not repeat each other.

`par` is the one way to run your own code concurrently. There is no `spawn`
that takes a function, since functions are not values.

### Tasks

The task builtins run one of the runtime's own builtins in the background,
named as text, and give back an integer handle:

```aether
fn main() -> Void {
    fx {
        let h: Int = task_spawn("sleep", "nap", 20);
        println("working while the task sleeps");
        println("task finished: ", task_wait(h));
    }
}
```

```text
working while the task sleeps
task finished: 0
```

`task_spawn(builtin, name, argument)` starts a task, `task_wait(handle)` waits
for it and returns `0` when it succeeded, and `task_result(handle)` reads the
builtin's own result. Only an allow-listed set of builtins can run this way;
for your own functions, use `par`.

---

## 21. The `--deny` sandbox and exit status

### Denying effects

`--deny` refuses whole classes of effects at run time. It is meant for running
code you have not read yet, such as a program a model just wrote: the program
compiles as usual, and stops with an error at the first call the list denies.

```aether @trap=AETH-RUNTIME-DENIED flags=--deny,net
fn main() -> Void {
    fx {
        println("starting");
        let session: Int = http_session();
        println("session ", session);
    }
}
```

```text
starting
prog.aether:4: [AETH-RUNTIME-DENIED] http_session is denied by --deny net in this run: network access is not allowed.
[Error Location] Offset: 34, Line: 4
```

The message names the builtin as the program wrote it and the class that
refused it. A refused `proc` call adds a hint: `par` runs your own functions in
parallel and is not affected by any class. The classes,
separated by commas (`--deny net,proc`), are:

| Class | Refuses |
|---|---|
| `io` | output, files and directories, standard input, environment variables, `args()` |
| `net` | `http_*`, `socket_*`, `dns_lookup` |
| `proc` | starting tasks (`task_spawn`, `task_queue`) and threads |
| `clock` | `clock_ms`, `now_text`, `sleep` |
| `random` | `random`, `randomize` |
| `ext` | loading native plugin libraries into the VM |
| `all` | every class above |

`--deny io` refuses printing too, so a program under it can report only through
its exit status. `par` blocks are not affected by any class. The environment
variable `PSCAL_VM_DENY` takes the same list, and the two are combined.

Ending the program is never denied: `exit`, `halt`, a `main` that returns a
status, and the checks behind contracts and slices all work under every
`--deny` list.

`has_builtin` takes the `--deny` list into account. It answers false for a
builtin this run refuses, as does `has_ai()` under `--deny net`, so a program
can check before it calls instead of stopping:

```aether @ok flags=--deny,net
fn main() -> Void {
    fx {
        if has_builtin("network", "dns_lookup") {
            println(dns_lookup("localhost"));
        } else {
            println("no network in this run");
        }
    }
}
```

```text
no network in this run
```

### Exit status

| Status | When |
|---|---|
| `0` | `main` returned normally, or a script ran to the end |
| `n` | `fn main() -> Int` returned `n`, or the program called `exit(n)` or `halt(n)` |
| `1` | a compile error, a run-time error, a broken contract, or a denied effect |

```aether @ok rc=3
fn check(v: Int) -> Int {
    if v < 0 {
        fx {
            println("error: negative value ", v);
            exit(3);
        }
    }
    ret v;
}

fn main() -> Int {
    let values: Int[] = [4, -1, 9];
    loop v in values {
        fx { println("ok ", check(v)); }
    }
    ret 0;
}
```

```text
ok 4
error: negative value -1
```

`exit(n)` and `halt(n)` are effects, so they go inside `fx`; both stop the
program at once, from any function.

---

## 22. Release labels and versions

Aether carries two version numbers, and `--version` prints both, for example:

```text
Aether Compiler Version: 2026-10-21-5 (1.0-beta4dev)
```

- **The date version** (`2026-10-21-5`, from the file `VERSION`) changes with
  every change to what programs compile or print: the date, and a counter for
  that day. Each one has an entry in `CHANGELOG.md`. Tools pin to it; it is the
  first word after `Version:`.
- **The release label** (`1.0-beta4dev`, from the file `RELEASE`) names
  releases: `1.0-beta1`, `1.0-beta2`, `1.0-beta3`, then `1.0`, `1.01`, `1.02`,
  and so on in hundredths, always with two decimals, so that `1.09` sorts
  before `1.10`. Between releases the label is the next release plus `dev`.
  Each release is a git tag of the same name; dev labels are never tagged.

A program can read both with `aether_version()`, which returns the text inside
the version line, such as `"2026-10-21-5 (1.0-beta4dev)"`.

### The compatibility freeze

Decision D75 freezes the language at 1.0. Once the project's owner declares
`1.0`, a program that is valid under 1.0 keeps compiling and printing the same
output in every later version. There are no renames, no removed forms and no
changes of meaning. What may still change:

- additions: new builtins, new types, and syntax that used to be an error;
- better diagnostics, better performance, and bug fixes;
- a single exception: a program whose 1.0 behaviour is silently wrong under
  the specification may become a coded error, with its own decision row, the
  evidence for it, and a hint that names the fix.

Until then the language is in beta: `1.0-beta1` and `1.0-beta2` were tagged on
2026-10-20 and `1.0-beta3` on 2026-10-21. The freeze already applies in intent,
but a breaking change found through use may still go in, with its own decision
row: D76, which made `task_status` return `Bool`, is one. What this manual
teaches is meant to stay true; later versions add to the language rather than
change it.

---

## 23. Where to go next

- **Every builtin.** The [appendix of the full
  guide](aether_for_llms_and_others.md#appendix-complete-builtin-inventory) lists
  every builtin this compiler exposes, with signatures, and marks the effectful
  ones. It is generated from the compiler and checked by a test, so it cannot
  drift. A running program can ask too: `builtin_info("name")` returns one
  builtin's details as JSON, and `builtins_json()` all of them.
- **Examples.** `examples/base/` holds small programs, one topic each, and
  `examples/showcase/` larger ones. Run any of them with
  `./build/aether examples/base/strings`.
- **The exact rules.** [The specification](aether_spec.md) gives the grammar
  and the leniencies, each with a test; `tests/surface/registry.json` lists
  every accepted and rejected spelling.
- **Why things are the way they are.** [The decision
  register](aether_decisions.md) records each design decision with its
  evidence; [`CHANGELOG.md`](../CHANGELOG.md) records each change to the
  language; [architecture and rationale](aether_architecture_and_rationale.md)
  explains the compiler.
- **Having a model write Aether.** Paste one of the LLM guides into its prompt:
  the [medium guide](aether_for_llms_medium_contexts.md) for a typical model,
  the [small guide](aether_for_llms_with_small_contexts.md) when context is
  tight, the [full guide](aether_for_llms_and_others.md) for a frontier model.
  Run what it writes under `--deny`, and feed any diagnostic back to it.
- **Working on Aether itself.** Start with `CLAUDE.md` at the repository root.

---

## Appendix: Coming from another language

| You might write | In Aether |
|---|---|
| `return x` | `ret x;` |
| `def f(x):`, `function f(x)`, `func f(x int) int` | `fn f(x: Int) -> Int { ... }` |
| `var x = 1`, `let mut x = 1`, `x := 1` | `let x: Int = 1;` |
| `class Point:` | `type Point { x: Int; y: Int; }` |
| `Point(1, 2)`, `new Point(1, 2)` | `new Point { x: 1, y: 2 }` |
| `this.x`, bare `x` in a method | `self.x` |
| `for i in range(n)`, `for (i = 0; i < n; i++)` | `loop i in 0..n { }` |
| `for x in xs`, `xs.forEach(...)` | `loop x in xs { }` |
| `while cond` | `loop cond { }` (`while` is accepted) |
| `elif`, `else if` | `else if` |
| `match`, `switch` | an `if ... else if ... else` chain |
| `print(f"{n} items")`, `printf("%d items", n)` | `fx { println(n, " items"); }` |
| `len(xs)`, `xs.length`, `xs.size()` | `length(xs)` |
| `xs.append(v)`, `xs.push(v)` | `xs = xs + [v];` |
| `s.upper()`, `s.split(",")` | `upper(s)`, `split(s, ",")` |
| `str(n)`, `n.toString()` | `int_to_text(n)` |
| `int(s)`, `parseInt(s)` | `let (n, ok) = try_parse_int(s);` |
| `a // b` | `a div b` |
| `dict`, `HashMap<String, V>` | `Map[Text, V]` |
| `xs.map(f)`, `filter`, a lambda | a `loop` that builds a new array |
| `try` / `catch`, `Option`, `Result` | a `(value, ok)` tuple return |
| `import x`, `#include` | `use "x";` |
| `async`, `Thread`, `go f()` | `par { f(); g(); }` |
