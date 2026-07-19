# Aether for LLMs — Concise Guide (for small contexts)

*Guide version: 2026-07-15-2*
Aether is a compact PSCAL front end. It uses the existing backend, bytecode
compiler, and VM. It is not a separate runtime.

## Highest-Value Rules

1. **FX-001.** Every effectful builtin must be inside `fx { ... }`: output,
   task helpers, `ai_chat`, and all host interaction — filesystem (`mkdir`,
   `fileexists`), env (`getenv`), CLI (`paramstr`), `random`, clock (`gettime`),
   console input, `http*`/`socket*`, `sqlite*`. Pure math/string/conversion do
   not need `fx`, and `@pure` functions may call none of the effectful ones.
2. **SYN-001.** Aether syntax only: `fn`, `let`, `const`, `ret`, `if`, `loop`,
   `type`, `mod`, `use`. Never `return`, `class`, `for`, `while`, `var`,
   `def`, `func`, `=>`.
3. **BUILT-001.** The builtins named here are the supported surface. Do not
   invent helpers (`substring`, `to_upper`, `replace`, ...) or guess names. More
   exist (discover via `builtin_info(...)`), but never assume an unlisted name.
4. **IMP-001.** Never invent `use "..."` imports. Only import verified modules.
5. **MOD-001.** If a module exports `getFortyTwo`, call `getFortyTwo`. Do not
   rename exports to `get_forty_two`, `AetherName`, `APP_NAME`, or other guesses.
6. **TOON-001.** `ToonDoc` and `ToonNode` are opaque handles: no arithmetic,
   never assign one where the other is expected.
7. **TYPE-001.** If inference is not obviously safe, add the type explicitly.
8. **ANN-001.** `@pre`, `@post`, `@pure`, `@cost` go directly above the
   function, never inside it, never bare (`@pre` with no expression).
9. **MUT-001.** Plain `let` is already mutable. Never generate `let mut`.
10. **ORDER-001.** Define types, helpers, and modules before `main` uses them.
11. **LEN-001.** `toon_len(node)` for TOON arrays; `length(xs)` for dynamic
    arrays.
12. **TUP-001.** Tuples are narrow: `let (a, b) = pair();` on a direct
    top-level helper call only. Never `let value = pair();`. If the producer is
    a method, undefined, or an expression, return a record/object and read its
    fields instead. Tuple `@post` checks must use positional slots like
    `result.0`, `result.1`. Recursive tuple-returning functions (direct or
    indirect) and concurrent `par`-branch calls to the same tuple-returning
    function are fully supported (tuple returns lower to a record returned by
    value, reentrant per call).
13. **OUT-001.** Return raw Aether source only. No Markdown fences.
14. **ROOT-001.** If the JSON starts with `{`, extract the named array with
    `toon_key(root, "...")` before iterating. Only iterate `root` directly
    when the JSON starts with `[`.
15. **SCOPE-001.** A name must be declared before use and still be in scope at
    the use site. Do not rely on guessed globals or out-of-scope loop locals.
16. **METH-001.** Methods do not capture outer locals. If a method needs `i`,
    `name`, or another caller value, pass it as a parameter.
17. **FIELD-001.** Inside a method, a local may reuse a field name. Bare
    `valid` means the local; `self.valid` means the field.
18. **FIELD-002.** Record and type field names must exist exactly as declared.
    Do not invent fields.
19. **FIELD-003.** A record/type field may declare a **constant** default:
    `field: Type = <const>` (`count: Int = 0`, `on: Bool = true`,
    `xs: Int[] = []`). Only compile-time constants — a default cannot reference
    another field, `self`, or call a function. For a computed value, set it at
    construction with `new T { field: value }`.
20. **FLOW-001.** Every non-`Void` helper must return a value on every
    reachable top-level path.
21. **FMT-001.** If the prompt specifies exact output, match it exactly:
    spacing, casing, line order, and decimal precision.
22. **NAME-001.** Do not redeclare a local name in the same scope. Pick fresh
    names such as `values`, `count`, `sum`, `maxValue`.
23. **MOD-002.** Canonical import form is `use "module_name";`. After import,
    call exported names directly. Never guess `export { ... }` syntax or the
    foreign object/JSON APIs listed under **TOON rules**.
24. **FUNC-001.** Functions are not values: no anonymous `fn(...) -> T { ... }`,
    no lambdas, no closures, never pass a function as an argument.
    `task_spawn`/`task_queue` take a builtin name as `Text`, not a function. No
    `map`/`filter`/`reduce`; use a `loop`. To run your own code concurrently,
    use `par { f(); g(); }`, not `task_spawn`.
25. **PAR-001.** Each `par` branch must own the record it writes. Passing the
    **same** record to two branches races — the parallel writes corrupt the heap.
    Give each branch its own record and combine the results after the block.
26. **MS-001.** `MStream` is the opaque memory-stream handle type (HTTP
    response bodies). `mstreamcreate()`/`mstreamfromstring(text)` return
    `MStream` — never `Int`, never `Text`. Body text comes from
    `mstreambuffer(ms) -> Text`; release with `mstreamfree(ms)`.

Default stance: single-file programs; variadic `println("label = ", v)` over
mixed-type `+`; explicit types for TOON values and non-trivial helper results;
real logic, never hard-coded expected output; preserve provided export names
exactly.

Not every rule above is a *compiler* check: some are emitted as a stable
`[CODE]` (with a `help:` line on newer builds), some are reported under a broader
code (mostly SCOPE-001), and the rest are authoring rules the compiler cannot
catch. The **Repair rules** section lists each emitted code with its fix and
flags the authoring-only ones.

## Core syntax

- Comments: prefer `// line comment`. Block comments are accepted, but models
  should still generate `//`. Text literals are double-quoted; escape `\"`.
- Types: `Int`, `Real`, `Text`, `Bool` (`true`/`false`), `Void`, plus opaque
  `ToonDoc`/`ToonNode`/`MStream`. `println(boolValue)` prints `true` or `false`. For mixed
  output use variadic `println` (never `+`); to build a `Text`, use the
  conversion helpers (`itoa`, `formatfloat`, `realtostr`, `parse_int/float/bool`).
- Operators: `+ - * / %`, `== != < <= > >=`, `!`, `&&`, `||`.
- Numeric literals allow `_` digit separators between digits: `1_000_000`, `0xFF_FF`.

```aether
fn add(a: Int, b: Int) -> Int {
    ret a + b;
}
```

Every `fn` declares `-> ReturnType`; procedures use `-> Void` and bare `ret;`.
`fn helper(x: Int) { ... }` is invalid.

```aether
const Name: Text = "Aether";
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

Statement-level `else` is supported.

Inline `if ... else ...` expressions are allowed anywhere a value is expected
— declarations, assignments, `ret`, and call arguments including
`println(...)`:

```aether
let label: Text = if ready { "ready" } else { "blocked" };
println("status: ", if ready { "ready" } else { "blocked" });
```

Loops (`break` exits; range is half-open):

```aether
loop index < total { index = index + 1; }
loop i in 0..count { fx { println(i); } }
loop { break; }
```

`continue` is supported.

## Records: `type`

```aether
type Counter {
    value: Int;            // fields end with ';'

    fn bump() -> Int {
        self.value = self.value + 1;   // lowercase self, never Self
        ret self.value;
    }
}
```

- methods live **inside** the `type` with an **implicit `self`** — never give one
  a `self` parameter; a method's `@pre`/`@post` may reference `self.field`
  (`@pre self.value >= 0` above `fn bump`). A free-standing `fn bump(self: Counter)`
  with a `self`-referencing contract fails `[SCOPE-001] identifier 'self' not in
  scope`.
- a field may declare a **constant** default: `value: Int = 0` (FIELD-003).
  Only compile-time constants — no other field, `self`, or call; for a computed
  value use `new T { field: value }`.
- `new Counter()` gives each field its declared default, else the type zero
  (Int `0`, Real `0.0`, Bool `false`, Text empty)
- construct with values: `new Point { x: 3, y: 4 }` (partial sets ok; an unset
  field keeps its default); bare `Point { x: 3, y: 4 }` / `Point(x: 3, y: 4)`
  also accepted
- records are pointer-backed: mutations through a callee are visible to the
  caller
- a top-level `fn bump(self: Counter) -> Int` is an extension method, called as
  `counter.bump()` — but it cannot carry a `@pre`/`@post` that names `self` (put
  the method and its contract inside the `type`)
- **field & method names must not be reserved words** — a member named after a
  type (`word`, `text`, `int`, `byte`, `bool`), a keyword (`new`, `for`, `if`,
  `match`), or an operator word (`mul`, `div`, `mod`, `xor`) fails SYN-001
  (`'<name>' is a reserved … and cannot be used as a field/method name`); rename
  it (`wordCount`, `multiply`). `new` in particular is reserved — there is no
  constructor method (see **Constructing records and typing bindings**).

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

Always annotate a binding that holds a `new` instance or an array literal
(TYPE-001): `let c = new C();` then `c.inc();` fails (`argument 1 to 'c.inc'
expects type POINTER but got VOID`), and `let xs = [1, 2, 3];` fails (`cannot
infer the type of 'xs'`). Write `let c: C = new C();` and
`let xs: Int[] = [1, 2, 3];`. Record literals may nest directly in an array
literal: `let ps: Point[] = [new Point { x: 1, y: 2 }, new Point { x: 3, y: 4 }];`.
Appending in a loop (`ps = ps + [p];`) also still works.

## Safe inference

Omit the type only for: literals (`42`, `3.5`, `"text"`, `true`) and calls to
functions/methods with known declared return types. Annotate everything else —
`new` instances and array literals (see **Constructing records and typing
bindings**), TOON extractions, branchy results, and arithmetic where operand
types aren't visible at a glance.

```aether
loop i in 0..5 {
    let square: Int = i * i;
    fx { println(i, " => ", square); }
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
output matters. `Int / Int` is integer-style division; force a `Real` operand
for decimals:

```aether
let pct: Real = ok * 100.0 / total;
```

Exact-output discipline:

- print exactly the labels requested, for example `avg0=0`, not `avg0 = 0`
- for percentages and averages that must show decimals, format them explicitly
- do not add extra headings, blank lines, or explanatory text

## Text

```aether
if name == "Aether" { ... }            // canonical; string_eq(a,b) accepted
let nameLen: Int = string_len(name);   // canonical; name.len accepted

// turning text into scalars — these exist, call them (do not inline):
let parts: Text[] = split("12,7,5", ",");  // Text -> Text[]  ["12","7","5"]
let n: Int = parse_int(parts[0]);          // Text -> Int
let r: Real = parse_float("3.5");          // Text -> Real
let ok: Bool = parse_bool("true");         // Text -> Bool
let s: Text = int_to_text(99);             // Int  -> Text  (itoa(n) also accepted)
let f: Text = formatfloat(3.14159, 2);     // Real -> Text  "3.14" (realtostr(r) = 6 dp)
let code: Int = ord("A");                  // Text(1 char) -> Int  65
let ch: Text = chr(65);                    // Int -> Text(1 char)  "A"
```

The safe Text surface: `string_eq`, `string_len`, `split`, `parse_int`,
`parse_float`, `parse_bool`, `itoa`/`int_to_text`, `formatfloat`/`realtostr`,
`ord`/`chr` (character <-> code point), plus `copy(s, start, count)`
(substring, 1-based), `pos(needle, s)` (1-based, 0 if absent), `trim(s)`,
`stringofchar(ch, n)`. Do not invent richer helpers (no `replace`; no
whole-string `to_upper`). Note: the `value:width:precision` spec only works
inside `println`; use `formatfloat` to build a `Text`. `int(x)` is a numeric
cast (`Real`/`Bool` -> `Int`, truncating) -- it does not parse or read the
code point of `Text`; passing it a `Text` silently returns `0`. Use
`parse_int` for numeric strings and `ord` for character codes.

## Math

Pascal naming: **`arctan`** not `atan`, **`ln`** not `log`. Trig in radians.
Args/results are `Real` unless noted.

- rounding (→ `Int`): `round`, `trunc`, `floor`, `ceil`; sign: `abs`
- powers: `sqrt`, `sqr`, `pow(base, exp)` (`power` alias), `exp`, `ln`, `log10`
- trig: `sin`, `cos`, `tan`, `arcsin`, `arccos`, `arctan`, `atan2(y, x)`, `cotan`
- hyperbolic: `sinh`, `cosh`, `tanh`
- helpers: `min(a, b)`, `max(a, b)`, `clamp(x, lo, hi)`, `odd(n) -> Bool`,
  `factorial(n)`, `fibonacci(n)`, `random()` `[0,1)`, `random(n)` `[0,n)`, `randomize()`

```aether
let pi: Real = 16.0 * arctan(1.0 / 5.0) - 4.0 * arctan(1.0 / 239.0);
```

## Files and environment

Host access for automation. **All effectful — call inside `fx`** (FX-001), and
banned in `@pure` functions:

- `fileexists(path) -> Bool`, `getcurrentdir() -> Text`
- `getenv(name) -> Text`, `getenvint(name, fallback) -> Int`
- `mkdir(path)`, `rmdir(path)`
- `paramcount() -> Int`, `paramstr(i) -> Text` (`paramstr(0)` = program)

```aether
fx { if fileexists("/etc/hosts") { println(getenv("HOME")); } }
```

To read/write file *contents*, declare a `File` variable (Pascal-style handle;
distinct from `Text`) and use `assign`/`reset`/`rewrite`/`readln`/`writeln`/
`eof`/`close`/`erase`/`rename`, all inside `fx`:

```aether
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
        readln(f, line);   // writes into `line` -- a special case for this builtin only
        println(line);
    }
    close(f);
}
```

Sockets (`socket*`), `sqlite*`, `random`, and the clock are also effectful;
discover them via `builtin_info(...)`.

## HTTP requests (MS-001)

Needs a libcurl build (`AETHER_ENABLE_CURL=ON`); the `http*` calls are
effectful (FX-001). The response body lands in an `MStream` out-buffer:

- `httpsession() -> Int`, `httpclose(session)`,
  `httpsetheader(session, name, value)`
- `httprequest(session, method, url, body, out) -> Int` (HTTP status; `body`
  is the request payload, `""` for GET; `out` is an initialized `MStream`)
- `mstreamcreate() -> MStream`, `mstreambuffer(ms) -> Text`,
  `mstreamfree(ms)` (all pure, no `fx`)

```aether
fx {
    let session: Int = httpsession();
    let out: MStream = mstreamcreate();
    let status: Int = httprequest(session, "GET", url, "", out);
    if status == 200 { println(mstreambuffer(out)); }
    mstreamfree(out);
    httpclose(session);
}
```

`mstreambuffer(out)` is the only way to read the body — never `print(out)`,
never `let stream: Int = ...`. JSON responses pair with TOON:
`toon_parse(mstreambuffer(out))`.

## Sockets

Real BSD-socket-style API (`socket*`) and `dnslookup` — not `tcpsocket*`/
`udpsocket*`/`resolve`, which do not exist. All effectful (FX-001).
`socketreceive` returns an `MStream` (MS-001), never `Text`/`Int` directly.

- `socketcreate(type[, family]) -> Int`: `type` `0`=TCP, `1`=UDP; `family`
  `4`=IPv4 (default), `6`=IPv6
- `socketconnect(socket, host, port) -> Int`,
  `socketbind(socket, port) -> Int` (all interfaces),
  `socketbindaddr(socket, host, port) -> Int` (one address),
  `socketlisten(socket, backlog) -> Int`, `socketclose(socket) -> Int`
  (all `0`/`-1`)
- `socketaccept(socket) -> Int` (new socket handle) **blocks** until a peer
  connects; `socketreceive(socket, maxlen) -> MStream` **blocks** until data
  arrives (`0`-length result = peer closed, not failure)
- `socketsend(socket, data) -> Int` (`data`: `Text` or `MStream`, bytes sent)
- `socketpeeraddr(socket) -> Text`, `socketlasterror() -> Int`
- `socketsetblocking(socket, blocking: Bool) -> Int`,
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
        out.payload = mstreambuffer(ms);
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
    ret;
}
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
```

Never `toon_len(xs)` on a dynamic array. Indexed reads/writes and
multi-element literals are supported. `xs = xs + [a, b, ...]` appends every
element of a literal of any length. `xs = xs + ys` / `let zs: T[] = xs + ys`
(`+` between two array-valued expressions, not necessarily literals)
concatenates them -- the RHS is evaluated once, then every element is
appended in order.

Use distinct local names inside one scope. Do not redeclare `xs`, `count`, or
other loop variables later in the same function.

`println`/`print` do not stringify arrays -- `println("data: ", xs)` silently
prints the array's internal representation (`ARRAY(dims:1, base_type:...,
elements_at:0x...)`), not its elements, and this is not an error. Loop over
the array and print each element yourself:

```aether
loop i in 0..length(xs) {
    print(xs[i], " ");
}
println("");
```

## TOON rules

Helpers (complete surface): `has_toon()`, `toon_parse(text)`,
`toon_parse_file(path)`, `toon_root(doc)`, `toon_close(doc)`,
`toon_key(node, key)`, `toon_at(node, i)`, `toon_len(node)`,
`toon_get_text/int/real/bool(node, key)` plus `_or(node, key, fallback)`
variants, `toon_text/int/real/bool/null_value(node)`, `toon_type(node)`,
`toon_has_key(node, key)`, `toon_has_at(node, i)`,
`toon_is_text/int/real/bool/null/arr/obj(node)`.

Keys are `Text`, indexes `Int`. Always `toon_close(doc)`.

Handle ownership:

- `toon_parse_file(path)` is **effectful** (file I/O — call inside `fx`);
  `toon_parse(text)` and the node ops below are pure (call outside `fx`)
- `ToonDoc` owns all `ToonNode` handles derived from it
- `toon_root(...)`, `toon_key(...)`, and `toon_at(...)` create node handles
- `toon_free(node)` releases one node handle early
- `toon_close(doc)` releases the document and any remaining child handles
- for short-lived documents, `toon_close(doc)` at the end is usually enough
- in large loops, prefer `toon_free(...)` for temporary nodes to avoid
  unnecessary handle buildup before document close

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

Key fidelity (KEY-001): copy JSON keys exactly. Never flatten nested objects
into guessed keys (`"appName"`, `"logLevel"`) or dotted keys (`"server.port"`):

```aether
let server: ToonNode = toon_key(root, "server");
let port: Int = toon_get_int_or(server, "port", 0);
```

Nested lookups (NEST-001): `_or` protects only the final lookup, not the path.
Guard intermediates:

```aether
let code: Text = "EMPTY";
if toon_has_key(row, "meta") {
let meta: ToonNode = toon_key(row, "meta");
    code = toon_get_text_or(meta, "code", "EMPTY");
}
```

Never: `toon_get_text_or(toon_key(toon_at(root, i), "meta"), "code", "EMPTY");`

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

This is the capture-free way to parallelize user code (FUNC-001). The task
helpers below are a lower-level handle API over runtime builtins, not user
functions.

## Tasks and AI

`sleep(ms: Int) -> Void`, `task_spawn(target: Text, name: Text, arg) -> Int`,
`task_queue(target: Text, name: Text, arg) -> Int`,
`task_wait(handle: Int) -> Int`, `task_lookup(name: Text) -> Int`,
`task_status(handle: Int) -> Int`, `task_result(handle: Int) -> Int`,
`task_stats() -> Array`, `task_stats_json() -> Text`,
`ai_chat(model: Text, messages: Text, system: Text = "", apiKey: Text = "", endpoint: Text = "") -> Text`,
probes `has_ai() -> Bool`, `has_builtin(category: Text, function: Text) -> Bool`.
All are effectful and must stay inside `fx`. `sleep(ms)` is a blocking
millisecond pause. `task_wait` waits on a task handle, not a duration.
`task_spawn`/`task_queue` dispatch an allow-listed runtime builtin by name (for
example `"delay"`), not a user-defined function; for user code use `par`.

Discovery (for builtins beyond this guide — deeper I/O, networking, DB, and
host-registered **custom** builtins). Agentic only: you must run the compiler.
- `builtins_json()` -> JSON list of all Aether-visible builtins
- `builtins_json(true)` -> richer metadata (`signature`, `effectful`, ...)
- `builtin_info(name)` -> metadata for one builtin
- `aether --dump-ext-builtins` -> registered custom/extended builtins

Discover the exact name, signature, and `effectful` flag, then call it (inside
`fx` if effectful). Cannot run the compiler? Stay within this guide; never guess
an unlisted name or its signature.

## Imports (IMP-001, MOD-001)

Imports are advanced; most generated Aether should be single-file.

- only `use "..."` for real, verified modules; a missing import's symbols do
  not exist even if the line is silently ignored
- **MOD-001**: imported names match exported names exactly — `use` does not
  rename. Module exports `classifySupport` → call `classifySupport(...)`,
  never a guessed `classify(...)`. Module exports `Answer` → use `Answer`,
  not `APP_NAME`, `AetherName`, or another invented spelling.
- if a task provides exports named `clampSupport`, `classifySupport`, or
  `PassMark`, call those exact names
- file naming: `mod ModuleConsts` → `use "module_consts";`
- when combining purity with module export, write `@pure` above `export fn`
- for generated code, assume modules export `const` and `fn`; do not generate
  exported `type` blocks

```aether
use "bench_support";
let score: Int = clampSupport(raw);
let status: Text = classifySupport(score);
```

Minimal import recipe:

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
`@pre` over an array parameter: `@pre length(xs) > 0`.

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
        fx { println("yyjson unavailable"); }
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

The compiler prints a stable code in brackets, and on newer builds a
`help: see <CODE> ...` line. Read the code, then apply the fix:

- **[FX-001]** an output, task, or `ai_chat` call outside an effect block → wrap
  it in `fx { ... }`.
- **[SYN-001]** non-Aether syntax → `ret` not `return`, `type` not `class`,
  `loop` not `for`/`while`; drop `var`, `def`, `=>`. Also a **field or method
  named after a reserved word** (`word`, `mul`, `new`, `for`, ...) → rename the
  member (see Records: `type`).
- **[SCOPE-001]** a name/scope problem — the catch-all. It is one of:
  - a helper not listed in this document → it does not exist; inline the logic (BUILT-001)
  - an export called by a guessed name → use the exact exported name (MOD-001)
  - a type or helper used before it is defined → define it earlier (ORDER-001)
  - a method reaching an outer local → pass it in as a parameter (METH-001)
  - `method '<m>' is not defined on type '<T>'` → define `fn <m>(...)` inside
    `type <T> { ... }`, or fix the call name (methods do not capture; METH-001)
  - a genuinely undeclared / out-of-scope name → declare it earlier or pass it in (SCOPE-001)
- **[NAME-001]** a local redeclared in the same scope (`'...' is already
  declared in this scope`) → pick a fresh name.
- **[IMP-001]** an invented or malformed import → remove the `use`, or write
  `use "module_name";` and call the exports directly (MOD-002).
- **[TYPE-001]** a type cannot be inferred → annotate it (including an untyped
  array literal, `let xs: Int[] = [...]`). Also covers `toon_len(node)` for TOON
  arrays vs `length(xs)` for dynamic arrays (LEN-001).
- **`expects type POINTER but got VOID`** on a method call (no code) → the
  receiver is an inferred `new` binding; annotate it (`let c: C = new C();`).
- **[TOON-001]** a `ToonDoc`/`ToonNode` handle misuse → e.g. add
  `let root: ToonNode = toon_root(doc);`; never do arithmetic on, or cross-assign, handles.
- **[MS-001]** an `MStream` handle misuse → declare stream bindings `MStream`
  (`let out: MStream = mstreamcreate();`), read contents with
  `mstreambuffer(ms) -> Text`; never `Int`/`Text` bindings, arithmetic, or
  cross-assignment with TOON handles (see HTTP requests).
- **[FIELD-002]** `Unknown field` → use the exact declared field name (or add it
  to the type if the prompt truly requires it).
- **[FIELD-003]** a field default that is not a compile-time constant (names
  another field, `self`, or calls a function), or a populated array default →
  use a literal/constant (`count: Int = 0`), or set the value at construction
  with `new T { field: value }`. A type-mismatched default (`value: Int = "x"`)
  is `[TYPE-001]` instead — match the field's type.
- **[FLOW-001]** a fallthrough path with no return value → add a final `ret ...`
  on every reachable top-level path.
- **[FLOW-002]** an empty `ret;` in a non-`Void` function → give the return a
  value (`ret <expr>;`), or declare the function `-> Void`.
- **[ANN-001]** a misplaced annotation, or a `@pure` function calling an effect →
  move `@pre`/`@post`/`@pure`/`@cost` directly above the function and keep effects out of pure code.
- **[TUP-001]** tuple misuse → destructure a direct top-level call only, `let (a, b) = pair();`; otherwise return a record/object and read its fields. Recursion (direct or indirect) and concurrent `par`-branch calls through a tuple-returning function are fine — tuple returns are reentrant (lowered to a record returned by value).
- **[MUT-001]** `let mut` → drop `mut`; a plain `let` is already mutable.
- **[PAR-001]** the same record passed to more than one `par` branch (concurrent
  writes race) → give each branch its own record and combine after the block.
- **[PAR-002]** a non-call statement inside `par { ... }` → `par` bodies allow
  only direct call statements; wrap the work in a `fn` and call that inside `par`.

If the program *compiles* but the output is wrong — extra headings, wrong
spacing or precision (an integer where decimals are expected → add a `Real`
operand such as `100.0`), wrong JSON keys, an unguarded nested lookup, or
iterating an object root — no code is printed: these are authoring rules the
compiler cannot check (FMT-001, OUT-001, KEY-001, NEST-001, ROOT-001,
FIELD-001). Re-read the prompt and match it exactly.

## Validation checklist

- all output, task, and `ai_chat` calls inside `fx { ... }` (FX-001)
- every called helper appears in this document (BUILT-001)
- imports verified; exported names used exactly (IMP-001, MOD-001)
- all parameters typed; uncertain types annotated; `new` instances and array
  literals typed (`let c: C = new C();`, `let xs: Int[] = [...]`) (TYPE-001)
- `ret` not `return`; `type` not `class`; no `let mut` (SYN-001, MUT-001)
- field & method names are not reserved words (`word`/`mul`/`new`/`for`); no
  `fn new()`/`__init__` constructor method (SYN-001)
- no arithmetic on / cross-assignment of TOON handles; docs closed (TOON-001)
- stream bindings declared `MStream`; bodies read via `mstreambuffer` (MS-001)
- object roots: named array extracted (ROOT-001); keys copied exactly
  (KEY-001); intermediates guarded before `_or` (NEST-001)
- `toon_len` vs `length` used correctly (LEN-001)
- `Real` operand where decimals matter; `value:width:precision` for stable output
- tuples destructured directly (TUP-001); annotations above functions (ANN-001)
