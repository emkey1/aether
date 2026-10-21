# Aether language changelog

Aether language versions use the date-stamp scheme `YYYY-MM-DD-N` (the same scheme
as the guides): the date of the last language-affecting change, with `N`
distinguishing multiple such changes on a single day. The value lives in
[`VERSION`](VERSION), is reported by `aether --version`, and is stamped into every
benchmark result for traceability.

A release label (D75: `1.0-beta1`, `1.0`, `1.01-dev`, ...) lives in [`RELEASE`](RELEASE)
and follows the date in `aether --version`; a release gets its own line here.

Bump it with `tools/bump_version.py` **only** when a change alters what programs
compile or how they run — a syntax, semantic, or builtin change — and never on a
plain rebuild. Because the stamp is checked in, every node that builds a given
commit reports the same version, so a real mismatch between nodes means one is
genuinely behind. Each bump should add an entry below.

## 2026-10-21-2

**Release `1.0-beta3`** (D75): the third 1.0 beta, tagged `1.0-beta3`;
`--version` prints `Aether Compiler Version: 2026-10-21-2 (1.0-beta3)`. It is
the changes from 2026-10-20-13 to 2026-10-21-2 on top of `1.0-beta2`: the VM
audit's silent-wrong compiler fixes, `aether_version()`, out-of-range Int
literals, and the standard-library fixes below (with D76, `task_status ->
Bool`). The next dev label is `1.0-beta4dev`.

**Standard-library defects the stdlib training pack found** (1.0-beta3dev):
engine fixes in pscal-core and front-end fixes here, each with a fixture in
`tests/fx/`. Every one of the 84 `stdlib_*` seed pairs keeps its output.

- **Async HTTP** (engine). Progress read after `http_await` raced the
  worker's slot reset: with three requests in flight on one session,
  `http_get_async_total` read 0 while the progress read 7, or both read 0. A
  request's final counters now stay readable after it is awaited or
  cancelled, and a `file://` request reports the file's size as its total (it
  read 0). `http_is_done` on a cancelled request is `true` from the cancel on;
  by Int id it read `false` forever once the slot was released. Every builtin
  that takes a session now holds that session's lock: since pscal-core d65d981
  async workers no longer write the session, which fixed the 1.0-beta2 crash
  (SIGTRAP in `httpAsyncFinish`, 4 in 400 runs here), and the lock covers what
  is left, an await or a request on another thread
  (`http_async_progress_pass`).
- **Builtin names given at run time take the Aether spellings.**
  `has_builtin("core", "file_exists")`, `("sqlite", "sqlite_open")`,
  `("core", "println")` and the other documented names answered `false`
  while the engine spellings D72 removed answered `true`; `task_spawn` and
  `task_queue` stopped with "unknown builtin identifier" on `"dns_lookup"` or
  `"sleep"`. The engine spellings still work (`builtin_names_runtime_pass`).
- **`task_status` returns `Bool`** (D76): `let ok: Bool = task_status(h);`
  compiles, and an `Int` binding is TYPE-001 with a hint naming the Bool. It
  was typed `Int` although the engine returned a Bool.
- **`ai_chat`'s third parameter is a system prompt**, as documented: it is sent
  as a system message ahead of `messages`. It was spliced into the request as
  JSON fields, so a prompt made the request invalid JSON. Text that is JSON
  object fields (`"temperature": 0`) is still merged, so every request that
  worked before is sent unchanged (`ai_chat_system_prompt_pass`).
- `socket_create` of a type other than 0 (TCP) or 1 (UDP) is `-1`; it made a
  TCP socket. Unseeded `random` gives the same numbers on every platform: the
  engine used libc `rand_r`, so glibc hosts drew differently from macOS; the
  macOS sequence is kept (`runtime_edges_pass`).
- Diagnostics: `elif` is SYN-001 once (it was reported again by each
  enclosing block, three times inside a loop in `fx`), and `loop b in [0, 255]`
  names the typed binding to write (`let items: Int[] = [...];`) instead of
  "cannot infer the type of the collection".
- Docs: the full guide corrects `http_request`'s transport-error value and
  lists the error classes and 5xx behaviour, `builtins_json()`'s shape (an
  array of objects), `mkdir` of an existing directory (`0`), SQL errors in
  `sqlite_exec`/`sqlite_prepare` (they stop the program; `sqlite_errmsg`
  serves a failed `sqlite_step`; turning them into return codes would break
  the beta freeze and is left to an owner decision) and a missing `toon_key`
  (a missing node, not a null one).

## 2026-10-21-1

**Two defects found during the 2026-10-20 VM-audit compiler fixes** (1.0-beta3dev),
both bugs under the beta freeze.

- **An Int literal that does not fit 64 bits is SYN-001** (spec §1.3, `L.lit.7`
  to `L.lit.9`). It used to clamp with no word: `18446744073709551615` and
  `9223372036854775808` both printed 9223372036854775807, a whole-number
  literal past that bound in a `Real` binding became 9.223372036854776e+18, and
  a hex literal with more than 16 significant digits read as all ones (-1). The
  message gives the bound, and for a decimal literal the hint names the Real
  spelling (`9223372036854775808.0`). `-9223372036854775808`, the smallest Int,
  now reads as itself; it used to give -9223372036854775807. A hex literal of up to
  16 digits keeps its 64-bit pattern (`0xffffffffffffffff` is -1), and leading
  zeros do not count. Pascal's qword literals are unchanged (pscal-core), and so
  is Rea.
- **An inferred `let` over `+` now follows the typed pass.** The parser's
  inference typed `+` from the operands' parse-time types, which are unknown for a
  name, a field or a call, so with no literal in the expression it fell back to
  Int:
  - `let x = a + b;` with two Text bindings, and `p.first + p.last`, `s + f()`,
    `s[0] + s[1]`, failed in the backend with the uncoded "Type mismatch: Cannot
    assign UNICODESTRING to integer". These now infer Text (S.textplus.5).
  - `s + t + s` was reported as Text plus a number. It now infers Text.
  - `let x = s[0] + 1;` failed with that same uncoded line, while `s[0] + 2`
    printed "a2". `s[i]` is a one-character Text, so this is the inferred
    Text-plus-number form: TYPE-001 like `let x = "a" + 1;` (S.textplus.4, D7
    still open). The hint names `ord(s[i]) + 1` for the character code and
    `let x: Text` for the join. A Text field plus a number (`it.name + 1`) gets
    the same TYPE-001.
  - **Silent-wrong:** a Real field was not named either, so `let v = n + p.r;`
    was typed Int and truncated (5.5 printed 5, `p.r - n` printed 0 for -0.5).
    It is now Real, as it already was with a Real local.

  `-`, `*`, `div` and `mod` with a Text operand now reach the typed pass's
  TYPE-001 ("`-` needs numbers ...", naming `ord` and `parse_int`) instead of
  "cannot infer". An inferred `let c = s[0];` or `let x = o.f;` is unchanged.

Fixtures: `basic/int_literal_edges_pass`, `diag/int_literal_out_of_range_fail`,
`diag/int_literal_hex_out_of_range_fail`, `diag/int_literal_for_real_fail`,
`basic/infer_let_text_join_pass`, `basic/infer_real_field_mix_pass`,
`diag/infer_let_text_index_plus_int_fail`, `diag/infer_let_text_field_plus_int_fail`.

## 2026-10-20-14

**`aether_version()`** (D75, a 1.0-beta addition; release label still
`1.0-beta3dev`). A program can now ask which compiler runs it:
`aether_version()` returns the text `aether --version` prints after
"Version:", the language version and then the release label, e.g.
`"2026-10-20-14 (1.0-beta3)"` (a PBuild umbrella build adds its `+commit`).
It is pure and fixed for a given compiler, so the front end turns the call into
a string literal; a user's own `fn aether_version` still wins, and an argument
is BUILT-002. It is in the builtin listing (pscal-core), so `builtin_info` and
the full guide's appendix document it. An agent had looked for a version in
vain: no builtin, no environment variable, and the guide's stamp
(`2026-10-20-10`) is the guide's own revision, not the compiler's.
`tests/fx/builtins/aether_version_pass` and a `tests/run.sh` check against
`--version`; the full guide's probes list it (guide stamp `2026-10-20-11`).

The engine pin also fixes a crash: an async HTTP request's worker thread wrote
the session's last headers and error, so a cancelled request that finished
after `http_close` freed them twice (`http_loopback_pass` aborted in 4 of 180
runs at 12 in parallel). The awaiting thread now applies them in `http_await` and
`http_try_await`, and what those calls leave on the session is unchanged.

## 2026-10-20-13

**Silent-wrong fixes from the 2026-10-20 VM audit's compiler list** (1.0-beta3dev).
Each compiled a program that then ran the wrong way.

Fixed:
- **A function declared twice, or twice up to case, is NAME-001** (D70; W4-23,
  whose backlog probe is now `tests/fx/names/duplicate_fn_fail`). The engine
  keeps one routine per name, ignoring case, so the later body replaced the
  earlier one for every call: `fn foo` beside `fn Foo` printed `2 2`, and a
  helper written twice ran its second body. Two methods of one type are
  checked the same way; a function and a method, or methods of different
  types, may share a name, and a function and a value still may (D17).
- **Arithmetic other than `+` on Text is TYPE-001.** `-`, `*`, `/`, `div`,
  `%`, `shl`, `shr` and unary minus took a Text operand: `s[0] - 1` was 96 for
  "abc" (the character code), `-s[0]` was -97, and `t - 1` on a whole Text
  stopped only at run time ("Operands must be numbers"). The message names
  `ord(c)` and `parse_int(t)`, the forms the guides teach; `+` still joins
  Text. `"abc" << 1` is now this TYPE-001 at compile time instead of the VM's
  uncoded shift error (`tests/run.sh` updated).
- **A print width or precision above 255** (`println(x:300)`) used its low
  byte (width 44): pscal-core's wide FORMAT_VALUE16.
- Engine (pscal-core, no Aether program reaches it today, since nested
  functions are lifted and nothing captures): a closure kept past its
  defining routine, or a thread running a nested routine past its spawner's
  return, no longer reads and writes the dead frame's stack slots.

## 2026-10-20-12

**Release `1.0-beta2`** (D75): the second 1.0 beta, tagged `1.0-beta2`;
`--version` prints `Aether Compiler Version: 2026-10-20-12 (1.0-beta2)`. It is
the builtin-surface work below on top of `1.0-beta1`. The next dev label is
`1.0-beta3dev`.

**The whole builtin surface is checked, and what the check found is fixed**
(1.0-beta2 work; the release label is `1.0-beta2dev`). Of the 60 builtins D72
renamed, 45 were called by no test or example, so nothing showed that a
documented name reached a working engine function. `tools/check_builtin_surface.py`
(ctest `aether_builtin_surface`) now generates its programs from the tables:
every documented signature in the builtin listing, each overload with and
without its defaults, must compile with its documented arity, argument types and
return type (185 shapes); every other name the appendix lists must resolve
(197); every removed spelling (D55, D61, D66, D72; 102) must fail with
SCOPE-001 and a hint naming its replacement. `tests/fx/builtins` runs the
families offline: SQLite on `:memory:`, sockets and DNS over loopback, a real
HTTP GET through libcurl against an in-process loopback server plus the
file and async forms, and `get_env`, `get_env_int` (from a new `.env` fixture
sidecar), `get_current_dir` and `file_exists`.

Fixed:
- **SQLite and `ai_chat` were in no shipped build.** The standalone build used
  pscal-core's minimal configuration, so `sqlite_*` (21 builtins in the full
  guide's appendix) and `ai_chat` (in all three guides) were SCOPE-001 on every
  host and grader; only the PBuild umbrella compiled them. Both are now on by
  default (`AETHER_ENABLE_SQLITE`; `ai_chat` with curl). A Linux build needs the
  libsqlite3 development package (CI installs it). A build configured without
  one says so: "identifier 'sqlite_open' not in scope: SQLite support is not
  built into this compiler.", with the configure flag as the hint.
- **The async HTTP builtins were unusable.** `http_request_async` and
  `http_request_async_to_file` returned an engine task value that no Aether type
  holds, so the documented `let id: Int = http_request_async(...)` stopped with
  "Cannot assign TASK to integer". Under Aether they return the task id as an
  Int, and `http_await`, `http_try_await`, `http_is_done`, `http_cancel`,
  `http_get_async_progress` and `http_get_async_total` take it; `http_is_done`
  returns the documented Bool.
- **Wrong documented types.** `sqlite_column_type` returns the storage class as
  Text (`"INTEGER"`, `"FLOAT"`, `"TEXT"`, `"BLOB"`, `"NULL"`), not Int;
  `http_set_option` takes an Int or a Text value by key. `task_stats()` returns
  records with no Aether type: the guide now says to count them
  (`length(task_stats())`) and read fields through `task_stats_json()`.
- **`sqlite_finalize` stopped the program** after a failed step: it reported
  the statement's last error (a UNIQUE violation, say) as its own failure. It
  now returns that code like `sqlite_step` and `sqlite_reset`.
- **Compile-time checks for 57 builtins.** The environment, working-directory,
  DNS, HTTP, socket and SQLite builtins had no arity check and no argument
  types, so `get_env()` or `socket_connect(s, 80, "host")` compiled and failed
  at run time. Each now has BUILT-002 arity (from the engine's own argument
  guards) and TYPE-001 for an argument of the wrong type ("this parameter is
  Text, but this value is Int"); their return types are in the type oracle, so
  `let ct: Int = sqlite_column_type(q, 0);` is TYPE-001.
- **A cold cached run lost the file and line of a run-time error.** The first
  run of a program with the cache on (the default) printed run-time errors
  without the `file:line:` prefix that `--no-cache` and a warm run print: a
  cache miss reset the compiled chunk and dropped its source path (rea). The
  newly built-in `ai_chat` made the run-time line-mapping test run under the
  pin gate's cold-cache lap, which found it.
- **The listing and the appendix.** The listing (and so `builtin_info` and the
  appendix) had no entry for the 13 `socket_*` builtins or `dns_lookup`; it has
  them now. The appendix no longer lists names this build does not register
  (the SDL family in a build without SDL), and lists the camelCase engine names
  (`apiSend`, `dosExec`, ...) in the lowercase spelling D70 requires.

## 2026-10-20-11

**Release `1.0-beta1`** (D75): this version closes the 2026-10-20 review round
(D4, D8, D24, D68, D69, D70, D72, D73, D74) and is the first 1.0 beta, tagged
`1.0-beta1`. The label lives in `RELEASE`; `--version` prints it after the date
version, which stays the first token after `Version:` for tools: `Aether
Compiler Version: 2026-10-20-11 (1.0-beta1)`. Fixes during the beta are
`1.0-beta2`, and so on.

**An imported module follows the importer's effect rules (D69).** The walk that
enforces FX-001 and ANN-001 ran before `use`d modules were loaded, so it never
saw a module's body or its functions: a module function could call `println`
outside `fx`, a module's `@pure` export could call an effectful helper, and a
`@pure` function could call a module export that performs effects, all with no
diagnostic (an in-file `mod` block was always checked). Each loaded module is
now walked once modules are in, its diagnostics naming the module's own file,
and a `@pure` function's call to a name not declared in its file is judged
against the module functions. ARR-001 (D8) reaches module functions the same
way, and the type-oracle rules that already walked modules (REC-001, the
discarded array result, the Real-index rule) now name the module's file
instead of the importer's. `fx` stays local (D69): a function that holds its own
`fx` block may still be called from anywhere but a `@pure` function, across a
module boundary too.

Fixtures: `tests/module_effects_fail` (FX-001 and ANN-001 in the module, ANN-001
in the importer), `tests/module_effects_pass` and `tests/module_arr_param_fail`,
checked in `tests/run.sh`. The full guide's Modules rules and the spec's
modules section say so (full guide stamp `2026-10-20-9`).

## 2026-10-20-10

**A Real prints in its shortest round-trip form (D74).** The default text of a
Real -- `println(r)`, `"x=" + r`, `real_to_text(r)` -- was six fixed decimals
(`2.500000`, `0.300000` for `0.1 + 0.2`, `1e20` as 21 digits). It is now
Python's `repr`: the shortest digits that read back as the same value, with a
whole value keeping `.0` and scientific form when the point sits more than 16
places right or 4 left of the first digit: `2.5`, `100.0`,
`0.30000000000000004`, `1e+20`, `1e-05`. Fixed decimals stay with explicit
formatting, which is unchanged: `format_float(x, 2)` and `println(x:0:2)`.
`real_to_text` follows the default (it was always six decimals). Engine:
pscal-core prints Reals with `pscalFormatRealRepr` when the front end is
Aether; other front ends keep `%f`.

**Recaptured** (every one checked to differ only in the formatting of a Real):
13 fixture `.out` files, 12 expected outputs in `tests/run.sh`, four in
`tests/experiments/run.sh`, the guide
program goldens (`docs/guide_goldens.json`), two example goldens, the spec's
example outputs and six training-corpus goldens (`recapture_expected.py
--accept`; 731 ok, 0 drifted after). No benchmark task's `expected_stdout`
prints a default-format Real (all 234 checked), so none is recaptured; archived
attempts that printed one with `println` can now match. The guides and the
card describe the new form (guide stamps `2026-10-20-8`, card `2026-10-20-2`).
The `repair_float_formatting` drill printed `19.99`, which the default form now
gets right; it is repointed to `20.5` (prints `20.5`, wants `20.50`).

## 2026-10-20-9

**Names are case-sensitive (D70, superseding D17's case folding).** The shared
engine resolves names ignoring case, so `total = 5` reached a `let Total`,
`new point()` built a `Point`, `area(3)` called `fn Area` and `Println(x)`
printed. A reference must now be spelled exactly as its declaration: a name that
matches a declared one only up to case is SCOPE-001, "identifier 'Total' not in
scope: names are case-sensitive, and the one declared is 'total'. Did you mean
`total`?", and a builtin written in another case (`Println`, `SQRT`) is
SCOPE-001 naming the lowercase form. Types and values are separate namespaces
(`let board: Board` is fine), and the cast spellings `Int(x)`, `Real(x)`,
`Bool(x)`, `Text(x)` stay. Two names that differ only by case still cannot both
be declared (NAME-001, now worded "differs from 'Total' only by case"), because
the engine cannot keep them apart. Fields were already exact (FIELD-002).

Census before the switch, over `tests/` (the 218 replay programs included),
`examples/` and the training corpus, 1,234 programs: none relied on
case-insensitive lookup, so nothing is migrated. The surface registry's
`case_underscore_fold` (`intToText(n)`, tolerated under D7, pending) is class 2.
The spec's identifier rule, its L.ident.1 example and D17 note follow, with a new
L.ident.7; the guides' NAME-001 rule and repair line say names are
case-sensitive (guide stamps `2026-10-20-7`). The `repair_name_case_const`
drill's stored diagnostic is re-pinned to the new NAME-001 wording.

## 2026-10-20-8

**Builtin names are snake_case (D72).** The run-together legacy names are
renamed with an underscore between the words, and the old spellings are class 2:
SCOPE-001 whose hint names the new one (`write format_float(...)`), as D55, D61
and D66 did for earlier families. A program's own `fn` of an old name still
works.

- Files, environment and formatting: `fileexists` -> `file_exists` (it was a
  tolerated spelling, D60), `formatfloat` -> `format_float`, `realtostr` ->
  `real_to_text`, `getcurrentdir` -> `get_current_dir`, `getenv` -> `get_env`,
  `getenvint` -> `get_env_int`, `dnslookup` -> `dns_lookup`.
- HTTP: `http_session`, `http_close`, `http_set_header`, `http_request`,
  `http_request_to_file`, `http_request_async`, `http_request_async_to_file`,
  `http_await`, `http_try_await`, `http_is_done`, `http_cancel`,
  `http_get_async_progress`, `http_get_async_total`, `http_get_header`,
  `http_get_last_headers`, `http_clear_headers`, `http_set_option`,
  `http_error_code`, `http_last_error`.
- Sockets: `socket_create`, `socket_connect`, `socket_bind`, `socket_bind_addr`,
  `socket_listen`, `socket_accept`, `socket_send`, `socket_receive`,
  `socket_close`, `socket_peer_addr`, `socket_last_error`, `socket_set_blocking`,
  `socket_poll`.
- SQLite: `sqlite_open`, `sqlite_close`, `sqlite_exec`, `sqlite_prepare`,
  `sqlite_step`, `sqlite_finalize`, `sqlite_reset`, `sqlite_clear_bindings`,
  `sqlite_bind_int`, `sqlite_bind_double`, `sqlite_bind_text`, `sqlite_bind_null`,
  `sqlite_column_count`, `sqlite_column_type`, `sqlite_column_name`,
  `sqlite_column_int`, `sqlite_column_double`, `sqlite_column_text`,
  `sqlite_changes`, `sqlite_last_insert_rowid`, `sqlite_errmsg`.

The engine names are unchanged (the new names alias them), so effects, arity
and types carry over; diagnostics quote the name as written (`call to
'http_session' requires an fx block`), while a VM `--deny` message still names
the engine builtin. Migrated: 15 tests, 9 examples, 18 training-corpus programs (manifest
shas updated; the 13 that record a deterministic stdout print it unchanged),
55 calls in the seed instruction solutions, 249 in the four distilled
training batches (`training/distilled`), the spice-market repair drill, 7
bench reference solutions in 5 suites (not a grading field; `tasks_version`
unchanged; 204/204 references pass), the oracle check's sandbox probe (which
now also requires the refusal to be the sandbox's: the old `socketcreate` was
rejected as SCOPE-001 and the probe read that as a pass), the guides, the card
and the spec. An unknown callee's SCOPE-001 in an inferred `let` quotes the
name as written and says when a builtin is missing from this build
(`sqlite_open` without SQLite named the engine's `sqliteopen`), and
`unknown_callee_inferred_fail` calls `open_database`, since `sqlite_open` is
now real. The builtin listing uses the new names
(pscal-core), the surface registry gains four `removed_d72_*` class-2 entries
so the appendix does not list the old spellings, and `alias_fileexists` is
class 2. Archived benchmark attempts that call an old name now fail to compile:
those are declared breaks in `bench/replay_waivers.json`.

## 2026-10-20-7

**Checked file reads (D73).** Three new helpers return `(value, ok)`, the shape
`try_parse_int` already taught, instead of stopping the program when a file is
missing or unreadable:

- `try_read_file(path) -> (Text, Bool)` -- `("", false)` on failure;
- `try_read_lines(path) -> (Text[], Bool)` -- `([], false)` on failure;
- `try_toon_parse_file(path) -> (ToonDoc, Bool)` -- `ok` is false for a missing
  file or bad JSON, and the document is then JSON `null`, so `toon_root`,
  every accessor and `toon_close` keep working.

All three are effectful: a call needs `fx` (FX-001) and is barred from `@pure`
(ANN-001), exactly as `read_file` is. The unchecked forms are unchanged and
still stop with a run-time error. Each is one line of Aether over hidden engine
builtins (pscal-core: `aether_try_read_file_text`, `aether_try_read_lines_list`,
`aether_try_toon_read_file` and a per-thread `aether_last_io_ok`), injected only
when a program calls it, like the `try_parse_*` pair; the file is read once.
The builtin listing documents the three as `fx` (appendix 156 -> 159), and the guides'
file sections list them (guide stamps `2026-10-20-5`).

## 2026-10-20-6

**Int / Int is Real, and a Real in an Int-only place is an error (D4 B+, D18).**
`/` on two Ints keeps giving a Real (Python 3's reading: `7 / 2` is `3.5`), and
`a div b` is the integer quotient. A quotient that reaches a place only an Int
can fill is now TYPE-001 naming `a div b`, where it used to truncate silently:
an **array or Text index** (`xs[n / 2]` indexed with 1 for 3.5 / 2) and **every
part of a loop range**, both bounds and the step (`loop i in 0..7 / 2` ran four
times, to 3.5). `%`, `div`, shifts and `int_to_text` were already coded. A typed
sink keeps its D1 behaviour (`let q: Int = a / b;` truncates).

Also fixed: a postfix on a Text literal was dropped, so `"abcd"[i]` and
`"abcd"[1..3]` gave the whole literal; they index and slice now.

Migrated: one harvested training-corpus program (not canonical, no oracle, so
excluded from datasets) looped `2..sqrt(n)`; it now loops
`2..int(sqrt(n)) + 1`, which also stops it listing 4, 9, 25 and 49 as primes
(its golden is corrected and its manifest sha updated).

Backlog probe div_range_bound (W4-42) flips to TYPE-001 and moves to
`tests/fx/diag`; div_compare (`7 / 2 == 3` is false) is the decided behaviour
under B+ and moves to `tests/fx/basic/div_int_int_is_real_pass`. The spec's
G.MulExpr.3 is an `@ok` example; G.MulExpr.4 and G.Range.5 pin the new errors.
The guides' TYPE-001 repair lines name the index and the range bound (guide
stamps `2026-10-20-4`).

## 2026-10-20-5

**Arrays are values, strictly (D8 V-strict).** A change to an array parameter
that the function never returns is now ARR-001, an **error** -- an element
(`xs[i] = v`), a nested element (`grid[r][c] = v`), an append or resize
(`xs = xs + [v]`), or a reassignment (`xs = sorted(xs)`) -- in any function, not
just a `Void` one. It was a warning for an index write in a `Void` function
only, so a non-Void helper that sorted its parameter in place (the fine-tunes'
heap-sort miss, runs 6 and 7) compiled and printed the unsorted input. The
message says "Return it and reassign: `xs = f(xs)`", and for scratch work
"copy it to a local first". A call statement that throws away the array a call
returns after taking one (`sortDesc(xs);`, `sorted(xs);`) is ARR-001 too. A
nested function's by-reference capture (D64) is exempt: its writes are seen.
A record field named after the parameter (`new LineUp { heights: heights }`)
is not a write to it (`tests/fx/basic/arr_param_record_field_init_pass`).
A `Map[Text, T]` parameter (D68) is a value the same way: an un-returned write
to one (`m[k] = v`, `remove(m, k)`), D68's warning, is now this error, worded
for maps (`Return it and reassign: m = f(m)`), and so is a discarded returned
map; `tests/fx/map/map_param_write_warn_pass` is now `map_param_write_fail`.

Programs that wrote an array parameter as scratch space no longer compile; copy
the parameter to a local (`let work: Int[] = xs;`). Migrated: the training corpus's
`67_bitboard_twelve_queens` (its manifest sha updated; same output), three
`tests/fx/lower` copy fixtures (they now return the written parameter and bind
the result; same output), the old warning test (now
`tests/fx/diag/arr001_void_index_write_fail`) and `arr001_nested_write_warn_pass`
(now `_fail`), and five repair drills in `training/seed_repair_pairs*.json`
that taught the silent version (`repair_array_inplace_mutation`,
`repair_sort_helper_mutates_param`, `repair_arrvalue_heap_sort_void`,
`repair_arrvalue_reverse_void`, `repair_arrvalue_grid_mark_void`): they were
`behavioral` (runs, prints the unsorted input) and are now `repair` drills on
the ARR-001 error. The replay case `cs_nqueens__36a6031b` (backtracking that marks
and unmarks its parameter arrays) is a declared break in `tests/replay/WAIVED`.
The guides say ARR-001 is an error (guide stamps `2026-10-20-3`); the full
guide's list of emitted codes gains REC-001 (missed in `2026-10-20-3`) and
FUNC-001. The guide-snippet gate now gives a fragment its arrays (`xs`, `table`,
...) as locals rather than parameters.

## 2026-10-20-4

**A map type: `Map[Text, T]`** (decision D68, superseding D47; pscal-core
c0abe3e). A language-level container like arrays: Text keys, values of any one
type (`Int`, `Real`, `Text`, `Bool`, a record, an array, another map).
Nothing that compiled before changes meaning, except the names below that
now resolve.

- **Surface.** The type `Map[Text, T]` anywhere a type goes (`Map[Text, Int[]]`,
  `Map[Text, Map[Text, Int]]`, `Map[Text, P][]`); the literal
  `{"k": v, ...}` and the empty `{}`, which needs a declared type; `m[k]`
  (a missing key is a run-time error: `key "dee" is not in the map; use
  has(ages, "dee") or get(ages, "dee", default)`); `m[k] = v` and the
  compound forms; writes into a stored value (`m[k][i] = v`, `m[a][b] = v`,
  `m[k].f = v`, `remove(m[a], k)`), which need the key; `has(m, k)`,
  `get(m, k, default)`, `remove(m, k)` (a no-op when absent), `keys(m)`,
  `values(m)`, `length(m)` and `m.length`; `loop k in m` (a snapshot of the
  keys); `k in m` and `k not in m` as `has(m, k)` / `!has(m, k)`.
- **Semantics.** Insertion order everywhere (keys, values, loop, printing):
  overwriting keeps a key's place, remove then re-insert moves it to the end.
  A map is a value like an array (D8): `let b = m;` and a call copy it
  (copy-on-write in the engine, so a copy costs nothing until written), arrays
  stored in a map are copied with it, and a function that changes a map
  returns it; a nested fn that writes an enclosing map changes it (D64).
  An un-returned write to a map parameter is ARR-001, as for an
  array (a warning in a `Void` fn today; in the V-strict arm,
  `AETHER_EXPERIMENT=arrays=vstrict`, for any un-returned write). Records stored
  in a map are shared, as in an array of records. `==`/`!=` compare key sets
  and values, ignoring order, by D24's structural path (record values field by
  field). `println(m)` prints `{"ann": 31, "bob":
  27}`: Text quoted and escaped, a Real in its shortest round-trip form (D74:
  `0.5`, `1.0`, `1e+20`), arrays and nested maps inside. Every map operation is
  pure (`@pure` may build and change its own maps).
- **Judgment calls the proposal left open** (recorded in the proposal and the
  D68 row). `k in m` is accepted as sugar for `has(m, k)`, with `k not in m`:
  `in` has no other infix use in an expression, and the operand is parsed
  tentatively, so any non-map right operand parses exactly as before. A `{`
  that starts a statement opens a block; one directly after `if`, `loop`,
  `while` or `elif` is that statement's body (`if { }` stays SYN-001);
  elsewhere `{` is a map literal when it is followed by `}` or by an
  expression and `:`, so `if x == { ... }` keeps its SYN-001.
- **Diagnostics.** TYPE-001: a non-Text key (`m[k]` with an Int `k`, a literal
  key, `Map[Int, T]`), a value of the wrong type (`m[k] = v`, a literal value,
  `get`'s default), `{}` with no declared type, `==` against a non-map,
  ordering or arithmetic on a map, a slice of a map, printing a map of
  records. NAME-001: a key written twice in one literal. REC-001: `==` on maps
  whose values are a self-referential record type. SCOPE-001 with a
  hint naming the function: `m.get(k)`, `m.keys()`, `m.items()`, `m.pop(k)`
  and every other method spelling (D71), and `dict()`, `has_key`, `get_or`,
  `map_get`. TYPE-002 naming `Map[Text, T]`: `Dict[..]`, `HashMap<..>`,
  `Map<..>`; `Set[..]` names `Map[Text, Bool]`. BUILT-002: `get(m, k)`.
- **Names.** `has`, `get`, `remove`, `keys` and `values` are Aether aliases of
  the engine-private `aether_map_*` builtins; a program's own `fn` of the same
  name still wins. No other front end sees them.
- **Performance.** A 20,000-word tally over 2,000 keys runs in about 0.2 s
  (D47 measured 9.9 s for the record-of-parallel-arrays idiom).

Fixtures: `tests/fx/map/` (every operation and error, nested maps, records as
values, value semantics, insertion order, `==`, printing). Surface registry
entries `map_type`, `map_in`, `map_length_property`, `method_map`,
`alias_dict`, `type_dict`, `map_int_key`. No archived benchmark attempt or
corpus program used a map, so no replay waiver.

## 2026-10-20-3

**Records and tuples compare by value (D24).** `a == b` on two records was a
pointer comparison and printed `false` for equal fields with no warning; it now
compares field by field (a tuple slot by slot), recursing into nested records
and arrays, so an array of records compares element by element too. `!=` is its
negation and `== nil` / `!= nil` work as before.

- **`same(a, b)`** (new builtin, pure) answers whether two values are the same
  record: the identity test `==` used to be. Arguments that are not records are
  TYPE-001.
- **REC-001** for `==` / `!=` on a record type that reaches itself through its
  fields (`type Node { next: Node; }`), where a field-wise comparison could
  recurse without end: use `same(a, b)` or compare the fields.
- **TYPE-001** for comparing two different record types, which can never be
  equal.
- Engine: pscal-core compares two record pointers field by field when the front
  end is Aether (other front ends keep pointer identity), with a depth cap as a
  backstop, and registers `aether_same`.

Programs that compared records with `==` change output where the compared
records were distinct but equal (they printed `false`, now `true`). Backlog
probes record_eq and tuple_eq (W4-20) flip and move to `tests/fx/basic`; the
spec's D.receq.1 is an `@ok` example, the surface registry's record_equality
entry is class 1, and the three guides say `==` compares records by value
(guide stamps `2026-10-20-1`).

## 2026-10-20-2

**Engine fixes from the 2026-10-20 language review** (pscal-core
26fd454..5ac9b55). No new surface and no compile-time change. A program that
got a silently wrong number now gets the right one or stops with a runtime
error, and a program whose `par` branch failed now exits non-zero. Corpus A/B
(734 programs) and the replay lap are unchanged.

- **Real → Int conversions are 64-bit.** `int`, `trunc`, `round`, `floor` and
  `ceil` tagged their result INT32, so `int(5000000000.0)` and
  `trunc(5000000000.7)` printed 705032704 and `round(-3000000000.0)`
  1294967296, while the implicit Real → Int store was right. That made
  NARROW-001's "write `int(...)`" steer to the wrong answer. A Real with no
  64-bit Int value (NaN, an infinity, |x| >= 2^63) used to come back as garbage
  (`int(1e20)` was -1). It is now a runtime error:
  `int(1e+20) has no 64-bit integer value`.
- **Shifts are 64-bit on literals too.** `1 << 32` was 0 and `3 << 30`
  negative (the VM's fast path for two small operands), while the same shift
  on a variable was right. A count of 64 or more is now defined: `<<` gives 0
  and `>>` the sign fill (`one << 64` was 1).
- **Int builtins trap on overflow like `+` and `*`.** `sum([MAX, 1])`,
  `abs(MIN)` and `-MIN` returned MIN, `sqr(3037000500)` went negative, and
  `factorial(21..)` and `fibonacci(93..)` wrapped (`factorial(25)` =
  7034535277573963776). All raise `Runtime Error: Integer overflow.`.
- **A failing `par` branch fails the program** (W4-37). The branch's error was
  printed and the program went on and exited 0. Now, at the join, the program
  stops with a non-zero exit, and only the branch's own error is on stderr.
  This is in the engine (THREAD_JOIN under `frontendIsAether()`), so other
  front ends' threads are unchanged. D37d had placed this fix in the front end.
  The engine route needed no front-end change, because the worker already
  records a failed job's status and only the join discarded it.
- **A branch that fails after running its own `par` no longer hangs.** A
  nested worker's failed job was escalated to a program-wide interrupt, and
  the joiner deadlocked on its own lock: it hung in 4 of 4 runs at pscal-core
  26fd454, and a variant stopped early with exit 0.

Backlog probes flipped and moved to `tests/fx/`: real_to_int_32bit,
trunc_32bit, round_32bit, shift_literal_32bit, factorial_wrap, sum_wrap,
abs_min_wrap, par_branch_failure. Backend conformance `int64_paths` (W5-04)
passes. New fixtures cover the boundaries (`int_edges_64bit_pass`), the range
error, negate/sqr/fibonacci overflow and two nested-`par` failures.

## 2026-10-20-1

**Soundness fixes from the 2026-10-20 language review.** No new surface. Every
change turns a program that compiled and then ran wrong, returned nil or crashed
into a compile-time error with a code, except the inferred-Real fix, which
makes such programs print the right value. Paired replay over the archived
benchmark attempts: no unwaived pass→fail; the 218 frozen replay cases are
unchanged (185 pass).

- **TYPE-001 for a value of the wrong type** (W4-17). The type oracle already
  classified every typed sink but no rule reported a mismatch, so
  `fn g() -> Text { ret 5; }`, `xs[0] = "z"` on an `Int[]`, `a.x = "hello"`
  into an `Int` field, `greet(42)` into a `Text` parameter and
  `const N: Int = "ten"` compiled, as did record confusion
  (`let a: P = new Q()`, a `Q` passed for a `P`, a `Q` in a `P[]` literal),
  which then read the wrong type's data by slot. All are TYPE-001 now
  ("type mismatch: the function returns Text, but this value is Int").
  Int into Real still widens, Real into Int keeps its truncation and
  NARROW-001 warning, and anything the oracle cannot type passes.
- **A condition must be Bool.** `if x = 2 { }` assigned 2 and took the
  branch, and `if n { }` on an Int ran as `n != 0`. Both are TYPE-001.
- **An inferred `let` over Int and Real is Real.** `let c = n * y;` (Int `n`,
  Real `y`) followed its left operand, was typed Int and printed 7 for 7.5.
- **FLOW-001 checks every path** (W4-04). It fired only when a non-Void
  function had no value `ret` anywhere, so an `if`-only body returned nil. A
  function now passes when every path ends in `ret`, `halt`/`exit`, an `if`
  whose branches all do, or a `loop { }` nothing breaks out of.
- **PAR-001 sees globals, aliases and shared fields.** A global one branch's
  call graph writes and another uses, one record under two names
  (`let b: S = a; par { f(a); f(b); }`), and the same field expression in two
  branches raced: lost updates, or a VM double-release crash (exit 133). They
  are rejected at compile time; distinct array elements, separate records and
  read-only globals stay legal.
- **A method named like its own type is NAME-001.** `fn c` inside `type C`
  (names ignore case) became the engine's constructor and died with
  "VM Error: Local slot index 1 out of range".
- **Backend errors now coded:** FIELD-002 for an unknown field, a case slip
  (`t.Count` on `count`, naming the declared spelling) and an unknown
  `new T { y: ... }` initializer; SYN-001 for `break`/`continue` outside a
  loop, on its line (was "L0: Compiler error"); FUNC-001 for a top-level
  function used as a value (`let f = sq;`, `apply(sq)`), as D49 specifies.
- **ENTRY-001's hint** for `fn main(args: Text[])` names `args()` instead of
  the removed `paramcount()`/`paramstr(i)`.

Backlog probes flipped and moved to `tests/fx/`: flow001_tail_ret,
flow001_int_fallthrough, ret_text_from_int, bool_from_int, bool_from_int_ret,
ret_int_from_text, elem_store_wrong_type, field_store_wrong_type,
record_nominal_let, record_nominal_param, infer_int_times_real,
assign_in_condition, par_alias_append, method_named_like_type,
entry_hint_paramstr.

## 2026-10-19-3

**A method spelling that cannot reach its function is SCOPE-001, not a run-time
"Undefined global"** (W6-05, D56). No new surface: programs that ran before run
the same; programs that used to compile and then die at run time now fail to
compile, with a hint naming the call form.

- **Builtins known only by an alias.** `x.round_to(1)`, `s.index_of(",")`,
  `n.int_to_text()`, `xs.sorted_desc()`, `xs.min_of()`, `path.read_file()`, ...
  compiled as UFCS calls and failed at run time with "Undefined global variable
  'round_to'": the method check took the name as callable because its alias
  (`aether_round_to`) is a builtin, but only a direct `name(` call is aliased,
  so the UFCS call reached the backend under the written name. Every
  modern-surface helper (D56-D67) not already in the method-hint table was
  affected. Now SCOPE-001: `Real has no method 'round_to'`, hint
  ``write `round_to(x, ...)`: builtins are functions, the value goes first``
  (just `round_to(x)` for an empty argument list; the receiver as written when it
  is a variable or `a.b` field, else a placeholder). The hand-written hints
  (`s.upper()`, `s.is_digit()`, `xs.sort()`, ...) keep their own wording.
- **Nested functions (D62).** `xs.total()`, where `total` is a nested fn,
  compiled and failed the same way (a nested fn is lifted under another name and
  only direct calls are rewritten). Now SCOPE-001 with hint
  ``write `total(xs)`: a nested fn is called as a function, ...``.
- **More receivers checked.** The check also types a receiver reached through a
  record field (`r.line.index_of(",")`, `self.label.upper()`), which escaped it
  before; array elements, variables and call results were already covered.
- **Unchanged.** A builtin that answers to the written name stays a UFCS call,
  as the full guide documents (`s.trim()`, `s.split(sep)`, `n.abs()`,
  `x.sqrt()`; fixture `diag/ufcs_builtin_pass`), as do `.len()` / `.length()`
  (D7 class 1). A program's own top-level fn, extension method
  (`fn index_of(self: Text, ...)`) or record method of the same name still wins.
- **Tests.** Fixtures `diag/method_alias_builtin_fail`,
  `diag/method_alias_builtin_field_fail`, `diag/method_alias_builtin_noargs_fail`,
  `diag/method_nested_fn_fail` and `diag/method_alias_builtin_shadow_pass`.

## 2026-10-19-2

**`[]` on an opaque handle is a compile-time error.** Indexing a `ToonNode`,
a `ToonDoc` or an `MStream` with `[]` compiled: a handle is an Int slot on the
VM, so `toon_get_int_or(items[i], "n", 0)` on a TOON array node ran until the
index and stopped with an uncoded `VM Error: Pointer does not point to an
array for element access.` (a call result such as `toon_key(root, "items")[0]`
drew the runtime `AETH-RUNTIME-INDEX-NOT-ARRAY` line, whose hint is about 2-D
arrays). Each shape is now rejected where it is written. Front end only: the
pins stay at pscal-core `26fd454` and rea `94ea5f5`.

- **TOON-001 for `ToonNode` and `ToonDoc`, MS-001 for `MStream`**, kind
  `type`: ``cannot index 'items' with `[]`: it is a ToonNode, an opaque TOON
  handle, not an array.`` The hint names the taught form: `toon_at(items, i)`
  and `toon_len(items)` with a `loop i in 0..toon_len(items)` skeleton for a
  node; `toon_root(doc)` first, then `toon_at(root, i)` or `toon_key(root,
  "name")` for a document; `mstream_text(ms)`, then index the `Text`, for a
  stream. Judged on any base whose type the front end can name: a local, a
  parameter, a call result, a record field (`h.node[i]`), an indexed write
  (`items[0] = x`). A `ToonNode[]` is a real array and indexes as before.
- **The slice form moves from TYPE-001 to TOON-001 / MS-001.** `items[0..1]`
  on a `ToonNode` was already rejected, but its hint ("index one element with
  `[i]`") led straight into the run-time crash; it now carries the same hint
  as the index form.
- **Where:** `aetherCheckHandleIndex` in `src/aether/ast_checks.c`, called
  from `parsePostfix` before both the slice and the index lowering; the base
  type comes from `aetherInferSliceBaseTypeName`, which resolves fields.

Census: the paired replay (317 archived attempts) gives identical outcomes on
`2026-10-19-1` and this change, so no archived benchmark program indexed a
handle and there is no replay waiver; every `.aether` program in this repo
(1,203: tests, examples, the training corpus candidates) run on both binaries
differs only in the new fixtures and in ten programs whose stdout also varies
between two runs of the old binary (random, wall-clock, `par` order). The
dataset gates (`--check-gates`) pass: no training record uses the form.

Docs: the full guide's TOON-001 repair bullet names `node[i]` and `ms[i]`
(full guide `2026-10-19-2`; medium and small unchanged: they already teach
`toon_at`/`toon_len` and carry no repair bullet for the index form). Surface
registry: `toon_node_index`, `toon_doc_index`, `mstream_index` (class 2).

Fixtures: `diag/toon_node_index_fail`, `diag/toon_node_param_index_fail`,
`diag/toon_call_result_index_fail`, `diag/toon_field_index_fail`,
`diag/toon_node_slice_fail`, `diag/toon_node_index_assign_fail`,
`diag/toon_doc_index_fail`, `diag/mstream_index_fail`,
`diag/toon_handle_array_index_pass`.


## 2026-10-19-1

**Character-class builtins** (decision D67, accepted by the owner on 2026-10-19).
`is_digit(t)`, `is_alpha(t)`, `is_alnum(t)`, `is_space(t)`, `is_upper(t)` and
`is_lower(t)`, each `Text -> Bool`.

- **Meaning, Python-like.** True iff `t` is non-empty and every character of
  `t` is in the class, so `is_digit(s[i])` tests one character and
  `is_digit("123")` a whole field; `""` is false. ASCII classes only: digit
  `0-9`; alpha `A-Z a-z`; alnum = alpha or digit; space = space, tab,
  newline, carriage return, form feed, vertical tab; upper `A-Z`; lower
  `a-z`. A non-ASCII character is in no class. Decided byte by byte in the
  engine, never by the host's `<ctype.h>` locale, so Linux and macOS agree.
- **Pure.** Usable in `@pure` functions, in `@pre`/`@post`, and outside `fx`.
  A wrong argument count is BUILT-002; a non-`Text` argument is a run-time
  error, as for `upper`.
- **Why.** Aether had no character-class builtins; the only way to ask "is
  this a digit" was an `ord` range comparison. The run-6 fine-tune
  (`qwen38-27b-r6`) invented `is_digit` on a main benchmark suite, and these
  are the names models and people reach for.
- **Names a program already uses keep working.** A program's own top-level or
  nested `fn is_digit(...)` (any signature) still wins over the builtin, as
  for every aliased builtin; a local, parameter or field named `is_upper` is
  untouched (fixture `tests/fx/names/char_class_named_fn_pass`). An
  unqualified call of a module's exported `is_digit` reaches the builtin, as
  it does for `upper`; call it qualified (`Mod.is_digit(c)`).
- **Did-you-mean hints** (SCOPE-001): `isdigit`, `isalpha`, `isalnum`,
  `isspace`, `isupper`, `islower`, `is_letter`, `is_whitespace` name the
  builtin; the method spellings `s.is_digit()`, `s.isdigit()`,
  `s.isnumeric()`, `s.isalpha()`, `s.isspace()`, ... are SCOPE-001 with a
  hint naming the function, as for `s.upper()`.
- **Engine.** pscal-core registers them under Aether-private names
  (`aether_is_digit` .. `aether_is_lower`, `ext_builtins/strings/stdlib.c`)
  and lists them in `builtins_json` with their signatures.
- **Tests.** Surface registry `helper_char_class` (class 1; each builtin on
  single characters, whole strings and `""`, and inside a `@pure` fn, against
  an `ord`-range twin), `method_char_class`, `alias_isdigit` (class 2);
  fixtures `names/char_class_named_fn_pass`, `diag/char_class_arity_fail`.
  No archived benchmark attempt or corpus program changes meaning, so there
  is no replay waiver.
- **Docs.** Full guide (Strings table and method-spelling note, the
  "Validating before parsing" contract now uses `is_digit`, the regenerated
  builtin appendix), medium guide (one line in the Text helpers), decision
  register (D67 row).

## 2026-10-18-2

**MStream write verbs** (decision D66, accepted by the owner on 2026-10-18).
`mstream_append_text(ms, t)` is now `mstream_write_text(ms, t)` and
`mstream_append_byte(ms, b)` is now `mstream_write_byte(ms, b)`; the meaning
is unchanged (the bytes go at the end of the stream). Reverses D61's two
append names. The engine-private builtin keeps its name; pscal-core changes
only the run-time messages and the builtin listing.

- **The old names are removed, not aliased** (class 2, as the D55 removals):
  a call to `mstream_append_text` or `mstream_append_byte` is SCOPE-001 with
  the hint ``write `mstream_write_text(ms, t)` (it adds t's bytes at the
  end)`` or ``write `mstream_write_byte(ms, b)` (it adds one byte at the
  end)``, unless the program declares a function of that name itself. The
  engine spelling `mstreamappendbyte` now hints `mstream_write_byte`.
- **Why.** "append" carried a Pascal-era feel (it descends from
  `mstreamappendbyte`); the run-5 fine-tune independently invented
  `mstream_write_text` twice; in modern languages `write` on a stream already
  means add at the end (Python `.write()`, Go's `Writer`). One canonical name
  per operation (D55), so no alias.
- **Did-you-mean hints** (SCOPE-001) for invented spellings now name the
  write verbs: `mstream_append`, `mstream_write`, `mstream_put`,
  `mstreamwrite` (both verbs); `mstream_write_bytes`, `mstream_append_bytes`,
  `mstream_write_string`/`_str`, `mstream_append_string`/`_str`,
  `mstreamappendtext`, `mstreamappendbytes`, `mstreamwritetext`
  (`mstream_write_text`); `mstreamwritebyte` (`mstream_write_byte`);
  `mstream_write_char`, `mstream_append_char` (`mstream_write_byte(ms,
  ord(ch))`).
- **Migration.** `tools/modernize_surface.py` rewrites the two old names (and
  `mstreamappendbyte`) to the write verbs. Migrated in this release: the
  surface registry (`helper_mstream`; new `removed_mstream_append_text`,
  `removed_mstream_append_byte`), the seed instruction solutions (15 pairs,
  one prompt), the repair drills' fixed sources, one corpus program (manifest
  sha256 refreshed), and the reference solutions of three sealed TOON tasks
  (not a grading field; prompts and expected output unchanged). No archived
  benchmark attempt used the old names, so there is no replay waiver.
- **Docs.** Full guide (HTTP/MStream table, the removed-spellings table and
  bullet, the regenerated builtin appendix), medium guide (the MStream
  bullet), spec section 6.1, decision register (D66 row; D61 row notes the
  rename).

## 2026-10-18-1

**Nested functions assign their enclosing locals; annotations above a nested fn**
(decisions D64, option (c), and D65, accepted by the owner on 2026-10-18; the
proposal [`docs/proposals/2026-10-18-nested-fn-assignment.md`](docs/proposals/2026-10-18-nested-fn-assignment.md)).
One engine fix in pscal-core (`spawn` passes by-reference arguments); rea
unchanged.

- **Assignable capture (D64).** A nested fn may assign the enclosing
  parameters and locals it can see, write their elements and append to them:
  `count = count + 1;`, `cols[r] = c;`, `adj[u] = adj[u] + [v];` change the
  enclosing variable, and a later read, in the host or any nested fn, sees the
  new value. Each activation of the host has its own variables, so a recursive
  host is correct. Lowering (`aetherLiftNestedFns`): the D62 capture walk also
  collects the captures each nested fn writes (an assignment, an element
  write, `setlength`/`inc`/`dec`, which covers the append and slice lowerings),
  closed transitively through sibling calls like the reads; a written capture
  becomes a `by_ref` parameter of the lifted fn, and a read-only one stays the
  uncopied value parameter of D62. The D8 `arrays=ref` shims of
  `src/aether/experiment.c` now also run for those parameters (shim 2
  normalises a literal-built array before a call that resizes it, shim 3
  copies a whole-value read such as `let c = xs;` or `ret xs;`); they touch
  only array-typed by-reference parameters. ARR-001 no longer warns on a write
  to a by-reference parameter. A nested fn assigning a captured `const` is
  "cannot assign to constant". Everything else in D62 stands: not a value
  (FUNC-001), one level deep, free functions only (METH-001), visible after its
  declaration.
- **FUNC-002 is retired.** Nothing emits it; the code (kind `nested-assign`)
  stays reserved so archived diagnostics keep their meaning.
- **par (D64).** PAR-001 has a new case: a local that a nested fn assigns
  (directly or through a sibling) in one `par` branch may not be used by
  another branch of the same block, as a capture or a call argument. One
  writer per local in one branch runs.
- **spawn with by-reference arguments (pscal-core).** `compileSpawnArgument`
  passes a VAR parameter's address, as an ordinary call does; every `spawn`
  argument used to compile as an rvalue, so a by-reference capture in a `par`
  branch failed with "GET_INDIRECT requires an address on the stack".
- **Annotations on a nested fn (D65).** `@pure`, `@pre` and `@post` directly
  above a nested fn are collected in statement position and mean what they
  mean on any fn (they were SYN-001, "unexpected token in block"). Contract
  clauses may read the fn's parameters and its captures. A `@pure` nested fn
  may read its captures but not write one, itself or through a sibling
  (ANN-001); it calls only `@pure` functions, siblings included, and purity
  diagnostics quote the declared name, not `__aether_<host>_<name>`. An
  annotation in a body that no `fn` follows is ANN-001, as at top level.
- **Census** (`results/decisions/nested_assign_2026-10-18.md`, re-run on this
  release): the 21 archived attempts that stopped on FUNC-002 now run, and
  match the measured lowering exactly: 15 pass and 6 print a wrong answer
  (devstral's own N-Queens diagonal bug); first attempts 7 pass, 2 silent
  wrong. The 3 `hard2_grade_bands` attempts with `@pure` above a nested fn pass.
- **Fixtures:** `tests/fx/lower/nested_fn_*_pass` (counter, element write and
  append, N-Queens, adjacency appends, union-find compression, heap, recursive
  host, sibling writes, the by-reference shims, every value kind, host
  parameters, annotations), `tests/fx/par/nested_fn_par_write_pass`, and
  `tests/fx/diag/nested_fn_{par_twice,par_read,par_sibling,pure_write,pure_call,annot_detached,const_write}_fail`;
  `diag/nested_fn_assign_fail` and `nested_fn_element_fail` became pass
  fixtures. Registry: `nested_fn_assign` is class 1 with a top-level-`let`
  twin, new `nested_fn_par_write` (class 2, PAR-001) and `nested_fn_annotated`
  (class 1). Spec: `G.NestedFn.2` is `@ok`; new `G.NestedFn.4` (PAR-001) and
  `G.NestedFn.5` (ANN-001).
- **ANN-001 for an annotation in a body.** `@pre`/`@post`/`@pure` inside a
  function body with no `fn` after it was the generic SYN-001; it is ANN-001
  now, with a placement hint (spec `G.Annotation.3`; three drills re-pinned).
- The tracked replay (instrument check, b9f29b1) is unchanged attempt for
  attempt (it holds none of the nested-fn attempts): first attempts 242->219,
  42 waived (the D55 class), 0 unwaived; corpus drift 0; oracle 204/204;
  dataset gates pass. The gain is in the archive the census reads: 18 attempts
  move from coded error to pass (15 writers, 3 annotated).

## 2026-10-17-5

**The modern surface, Release B: quantifier expressions and nested functions with
read-only capture** (decisions D58 and D62; the accepted proposal
[`docs/proposals/2026-10-17-modern-surface.md`](docs/proposals/2026-10-17-modern-surface.md)).
Front end only: pscal-core stays at `a67836f`, rea at `94ea5f5`.

- **Quantifiers (D58).** `all NAME in COLLECTION: COND` and `any NAME in ...: COND`
  are `Bool` expressions, usable anywhere an expression is, `@pre`/`@post`
  included. The header is a `loop` header (`parseLoopRangeWith`, the body
  callback form of `parseLoopRange`): an array, a `Text`, a ToonNode array, or a
  range with an optional `step`. Lowered (`aetherParseQuantifier`, ast_lower.c)
  to a hoisted `let __aether_q_N: Bool = true|false;` and a block holding the
  loop, whose body tests COND and breaks on the first deciding element; COND's
  own hoists (a nested quantifier, a slice) run inside the loop. An empty
  collection leaves `all` true and `any` false. NAME is declared inside the loop
  block, so it is visible only in COND; it follows the loop-variable rule
  (NAME-001, `validateNoDuplicateLocals`, contract clauses checked against the
  next `fn`'s parameters). On the right of `&&`/`||` and in a `?:` branch the
  loop runs only when evaluated (the existing hoist deferral). COND certainly
  not a Bool (a literal, arithmetic, an Int/Real/Text name) is TYPE-001;
  `all(...)`/`any(...)` as calls stay SCOPE-001 with a hint naming the
  quantifier; `all`/`any` remain ordinary names elsewhere (a two-token
  lookahead for `NAME in`).
- **Contract clauses splice their hoists.** `buildContractGuard` takes the
  clause sub-parser's hoists (`parseExprFromText(..., outHoists)`, which now
  shares the temp counters) and places them ahead of the test, in a block of
  their own; `contractHoists` lets the short-circuit and conditional deferrals
  run there, so `@pre n <= length(xs) && all i in 0..n: xs[i] > 0` never
  indexes out of range. A failing quantified clause is CON-001 like any other.
- **Nested functions (D62).** A `fn` in the body of a free top-level function
  (not a method, a `mod` member or a `self` extension fn) reads the enclosing
  parameters and the locals declared before it, calls itself and its sibling
  nested fns (declared before or after), and is visible after its declaration
  in the block that declares it. Lowered by lambda lifting
  (`aetherLiftNestedFns`, run when the host body is complete): each becomes a
  top-level `__aether_<host>_<name>` with one extra parameter per enclosing
  local it reads, directly or through the siblings it calls (fixed point),
  passed by name at every call; prototypes and lifted fns are emitted ahead of
  the host. A capture parameter gets none of a declared array parameter's
  prologue copies (the VM shares an array value; not `by_ref`, whose
  whole-value reads fail the un-alias step): N-Queens n=8 reading a
  20,000-element array runs in 0.70 s against 30.17 s with the array as a
  copied top-level parameter, 0.18 s against 0.22 s with an 8-element one
  (best of three, `results/decisions/d62_readonly_capture.md`); recursion depth
  is unchanged (100,000 frames run, 1,000,000 overflow, either way).
- **FUNC-002 (new code).** A nested fn assigning an enclosing local, an element
  of one (`cols[r] = c`), or resizing it (`xs = xs + [v]`) is rejected: "return
  the new value and assign it in the enclosing function, or keep it in a record
  field". A field write through an enclosing record (`st.count = ...`) is
  allowed. Other nested-fn errors: storing, passing or returning it is
  FUNC-001; a `fn` inside a nested fn ("one level deep") or a method (METH-001)
  is FUNC-001; a call before the declaration, outside the declaring block, or
  where a reused name hides a captured local is SCOPE-001; a wrong argument
  count names the nested fn. Inside its host a nested fn shadows a builtin of
  the same name (`fn count(...)`), and nowhere else. The tuple pre-pass also
  registers indented `fn` lines (a nested tuple-returning fn) unless a
  column-0 `fn` has the name.
- **Registry:** `nested_fn` flips from class 2 (FUNC-001) to class 1 with a
  top-level twin; new `nested_fn_assign` (FUNC-002), `nested_fn_value`
  (FUNC-001), `quantifier` and `quantifier_contract` (class 1, hand-written
  twins) and `quantifier_call` (SCOPE-001). 128 entries.
- **Spec:** `NestedFn` joins `Statement` and `Quantifier` joins `Primary`;
  `G.FnDecl.3` (`@reject=FUNC-001`) becomes `G.NestedFn.1` (`@ok`), with
  `G.NestedFn.2` (`@reject=FUNC-002`), `G.NestedFn.3` (two deep, FUNC-001) and
  `G.Quantifier.1`/`.2`.
- **Fixtures:** `fx/lower/quantifier_{basic,positions,short_circuit,contract}_pass`
  (every expression position the lowering contract names),
  `fx/diag/quantifier_{contract,not_bool,missing_colon}_fail`,
  `fx/names/quantifier_name_reuse_fail`;
  `fx/lower/nested_fn_{nqueens,siblings,union_find,shared_copy,block}_pass` (the block one
  was `diag/nested_fn_block_fail`), `fx/diag/nested_fn_assign_fail` (was
  `nested_fn_fail`, now FUNC-002), `nested_fn_{element,value,deep,method,scope}_fail`.
- **Drills:** `repair_global_const_scope` taught "move the nested fn to the top
  level", now legal; repointed as `repair_nested_fn_assigns_local` (a nested fn
  accumulating into an enclosing `Real` -> return the area), in both overlays.
- **Replay (fine-tune first attempts):** the five no-guide attempts that wrote
  a nested helper (`cs_nqueens` x3, `cs_dijkstra`, `algo_union_find_components`)
  all assign captured state (`count = count + 1`, `adj = ...`, path
  compression on `parent`), so they move from FUNC-001 to FUNC-002 and none
  flips to pass under the read-only rule; the replay tallies of
  `training/training_runs/*/score` are otherwise identical to 2026-10-17-3.
  The tracked replay (instrument check, d67626c) is unchanged too: first
  attempts 242->219, 42 waived (the D55 class), 0 unwaived; corpus drift 0;
  oracle 204/204; dataset gates pass.

## 2026-10-17-4

**`[v; n]`: n copies of v** (decision D7, class 1; registry `repeat_literal`
holds). Language 2026-10-17-3 removed `setlength`, which left presizing an
array to a loop. pscal-core gains `aether_repeat_array` (in
`src/ext_builtins/strings/stdlib.c`); rea is unchanged.

- **Lowering.** The parser turns `[v; n]` into a call of
  `aether_repeat_array(v, n)`, so it is an ordinary expression in every
  position (let, assignment, `ret`, call argument, guard, `else if`, loop
  condition, tuple item, record field, if-expression, foreach collection, an
  operand of `+`; fixture `tests/fx/lower/repeat_literal_positions_pass`), and
  v and n are evaluated once each. A nested array value is copied per element,
  so `[[0; cols]; rows]` gives independent rows; n <= 0 gives `[]`; an Int
  element is 64-bit and a one-character literal is Text. Inferred `let`s type
  through it (`let ys = [0; 4];` is an `Int[]`).
- **A record value is TYPE-001** (`repeat_literal_record_fail`, registry
  `repeat_literal_record`): one record would fill every slot, so a write
  through one would show in all, a silent wrong answer. The hint names the
  loop; the engine refuses it at run time where the type is not known.
- **`[1, 2; 3]` and `[0; 3, 4]`** stay SYN-001 (`repeat_literal_list_fail`
  names the comma form).
- **`new Int[n]`**'s SYN-001 hint now names `[0; n]` (registry
  `new_sized_array`).
- **`reversed(xs)`** of a nested array copies the rows too (it shared them).
- **Spec:** the `ArrayLit` production gains `"[" Expr ";" Expr "]"`;
  `G.ArrayLit.3` flips from `@bug=D7` to `@ok`.
- **Migration:** the presizing loops written by the 2026-10-17-3 migration
  are `[0; n]` again in 6 bench reference solutions (tasks_scale, trap
  `trap_toplevel_init`) and 5 seed instruction pairs (stdout unchanged).

## 2026-10-17-3

**The modern standard surface, Release A: the Pascal-era forms are removed and a
small standard library ships in the same release** (decisions D55, D56, D57,
D59, D60, D61 and D63; the accepted proposal
[`docs/proposals/2026-10-17-modern-surface.md`](docs/proposals/2026-10-17-modern-surface.md)).
Owner decisions are final: `try_parse_int`/`try_parse_real`, `min_of`/`max_of`,
`sorted_by(xs, "field")` with a literal field name, and no transition period.
Engine: pscal-core gains `src/ext_builtins/strings/stdlib.c` and the stdin
readers in `builtin.c`, registered under Aether-private engine names
(`aether_upper`, `aether_count`, ...) that the Aether front end aliases its
surface names to: plain names would enter every front end's namespace, and
Pascal resolves a builtin's name ahead of a program's own `count` or `sum`
variable (two Pascal tests broke that way before the rename). rea is unchanged. Release B (quantifiers D58, nested
functions D62) is separate.

- **New helpers, class 1 (taught).** Text: `upper`, `lower` (whole string;
  `upcase`/`toupper` are now aliases of `upper`, shipping D21), `replace` (every
  occurrence), `contains`, `starts_with`, `ends_with`, `index_of` (zero-based
  code point, `-1` when absent), `count`, `repeat`, `reversed`, `join(Text[],
  sep)`. Arrays (`Int[]`, `Real[]`, `Text[]`; each returns a new value):
  `sum`, `min_of`, `max_of`, `sorted`, `sorted_desc` (stable), `reversed`,
  `contains`, `index_of`, `count`; records: `sorted_by(xs, "field")` /
  `sorted_by_desc` with a string-literal field name checked at compile time
  against the element type's `Int`/`Real`/`Text` fields (FIELD-002 otherwise)
  and lowered to the field's record slot. Checked parsing: `try_parse_int(t) ->
  (Int, Bool)` and `try_parse_real(t) -> (Real, Bool)` (surrounding whitespace
  and one sign allowed; `""`, `"12x"`, hex, inf/nan and out-of-range values are
  `(0, false)`), declared fns over four hidden pure builtins, parsed into a
  program that calls them; the parse is the engine's own, so Linux and macOS
  agree. `round_to(x, digits)` rounds half away from zero (2.675 -> 2.68).
  I/O, all effectful (FX-001, rejected in `@pure`, covered by `--deny`):
  `read_file`, `read_lines`, `write_file`, `append_file`, `delete_file`,
  `rename_file`, `file_exists` (alias of `fileexists`), `read_line`,
  `read_all_stdin`, `args`, `clock_ms`, `now_text`. MStream: `mstream_new`,
  `mstream_from_text`, `mstream_text`, `mstream_append_byte`, `mstream_save`,
  `mstream_free` (aliases), and new `mstream_append_text`, `mstream_length`,
  `mstream_load(path) -> MStream`. Inferred `let`s type through them
  (`let ys = sorted(xs);` is an `Int[]`, `let parts = split(t, ",");` a
  `Text[]`).
- **Removed, class 2.** `copy`, `pos`, `stringofchar`, `val`, `valreal`,
  `itoa`, `inttostr`/`IntToStr`, `assign`, `assignfile`, `reset`, `rewrite`,
  `append`, `close`, `closefile`, `eof`, `erase`, `rename`, `readln`, `read`,
  `paramcount`, `paramstr`, `gettime`, `getdate`, `setlength` and the
  unprefixed `mstreamcreate`/`mstreamfromstring`/`mstreambuffer`/
  `mstreamappendbyte`/`mstreamloadfromfile`/`mstreamsavetofile`/`mstreamfree`
  are SCOPE-001 at the call, with a hint naming the modern form (a program's own
  `fn rename(...)` still wins); the type `File` is TYPE-002 naming
  `read_file`/`write_file`. `writeln(f, ...)`/`readln(f, ...)` need a `File`,
  so they end there. Method spellings on builtin types (`s.upper()`,
  `s.contains(x)`, `xs.sort()`, `xs.join(",")`, `xs.sum()`, ...) stay SCOPE-001
  and the hint now names the function; `s.trim()`/`s.split(sep)` remain UFCS
  calls. Unknown-name hints point at the snake_case MStream family and the new
  file helpers.
- **Engine fixes riding along.** `parse_int` returns a 64-bit Int (it truncated
  through `makeInt` to 32 bits: `parse_int("3000000000")` was `-1294967296`;
  trap `trap_parse_int_big` now passes). `getBuiltinReturnType` knows the new
  Text helpers, so `upper(t)[0]` indexes a Text.
- **Registry:** 63 new entries (one per removed spelling, the method hints, a
  class-1 entry with a hand-written twin per helper, `fileexists` and
  `toupper` class 4); `itoa_family` became `removed_itoa`/`removed_inttostr`
  and `upcase_single_char` flipped to class 1 (holds). 122 entries.
- **Spec:** `File` leaves the `TypeName` production; the leniency table drops
  `itoa(n)` and adds `upcase`/`toupper` and `fileexists`; `LEN.len.1` prints
  `upcase("ab")` (`AB`) instead of `itoa(8)`; new section 6.1 with
  `REM.copy.1` (`@reject=SCOPE-001`), `REM.own.1` (`@ok`, a user `fn rename`)
  and `REM.file.1` (`@reject=TYPE-002`).
- **Migration, same release.** `tools/modernize_surface.py` rewrites the
  mechanical cases (`copy` -> slice, `pos` -> `index_of`, `itoa`/`inttostr` ->
  `int_to_text`, `stringofchar` -> `repeat`, `paramcount`/`paramstr` ->
  `args()`, the MStream names) and lists the rest for a hand edit. Applied to
  the examples (`base/file_io` rewritten on the one-call helpers), tests, the
  guides, 64 corpus programs (manifest sha256s refreshed; every one prints its
  manifest stdout, or, for the environment-dependent ones, exactly what the old
  program printed), 25 seed instruction pairs (prompts too), 1 distilled pair,
  the drill fixed sources, and 21 bench reference solutions (not a grading
  field: `tasks_version` unchanged; every reference prints its expected stdout,
  212/212 including the slow suite). Drill `repair_mstream_new_name` taught the
  inverse of D61 and is repointed as `repair_mstream_engine_names`
  (`mstreamcreate` -> `mstream_new`); `repair_array_slice` re-pinned to
  SCOPE-001; four drills added (`itoa`, `pos`, `val`, the File sequence).
- **Fixtures:** `tests/file_io_pass` and `readln_no_alias_pass` use the
  one-call helpers; `fx/basic/val_edges_pass` became `try_parse_edges_pass`;
  `fx/diag/file_{param,return,field}_fail` are TYPE-002 class-2 fixtures and
  `file_global_helper_pass` passes a path; `fx/basic/upcase_whole_text_pass`
  is the backlog probe `upcase_text` (W4-45) moved into tests; probe
  `eof_always_true` (W5-29) is retired with `eof`; backend conformance
  `stdin_pipe` (W5-12) passes through `read_line` and is `pass`.
- **Replay:** the 33 archived programs that call a removed form (26 `itoa`,
  4 `copy`, one each `assign`, `val`, `mstreamfromstring`) are waived as one
  class in `tests/replay/WAIVED`.

## 2026-10-17-2

**`toon_parse(raw)` reads `raw`; a TOON getter can be passed to a Void fn.**
Two bug fixes. pscal-core moves to `9464b57`.

- **`toon_parse(raw)` parsed `raw`'s first literal (silent wrong).** The
  builtin-alias pre-pass kept a line-ordered table of `let raw: Text = "..."`
  bindings and inlined that literal into `toon_parse(raw)`. It ignored a later
  `raw = ...` (in `fx` too) and any parameter or local of the same name in a
  later function. So `let raw: Text = "{}"; raw = "{\"w\": 5}";` parsed `{}`,
  and a first value of `""` failed at run time with "input length is 0".
  The table is gone and `toon_parse` always reads the variable. A program
  that never reassigns the name prints the same as before. Fixture
  `tests/fx/basic/toon_parse_reassigned_pass`.
- **A Text, Real or Bool TOON getter passed straight to a Void fn compiles.**
  `shout(toon_get_text(root, "k"))`, where `shout` returns `Void`, was
  "Compiler Error: argument 1 to 'shout' expects type UNICODESTRING but got
  VOID." The engine had no return type for `toon_get_text`, `toon_get_text_or`,
  `toon_text_value`, `toon_type`, the Real getters, `toon_get_bool_or` and the
  other Bool getters, and a call statement checks arguments exactly
  (pscal-core `9464b57`). Fixture
  `tests/fx/basic/toon_getter_into_void_fn_pass`.

## 2026-10-17-1

**Open-ended slices; a tuple-return function can forward a same-typed tuple;
a nested `fn` is FUNC-001.** Owner-approved (2026-10-16) from the Qwen3.8
fine-tune's no-guide failures on `tasks_cs` / `tasks_hard_nontoon`. Front end
only: pscal-core stays at `632f4d1`, rea at `a0fae25`.

- **`xs[a..]` and `xs[..b]` (W4-35).** `queue = queue[1..];` was `[SYN-001]
  expected an expression after '..', found ']'`. An open end means
  `length(base)` and an open start means `0`, for arrays, `Text` and fields
  (`o.f[a..]`, `self.f[..n]`); `xs[n..]` is empty. A Text slice still clamps
  (D37d): `s[a..]` lowers to one `copy` with a count past any end, so a start
  at or past the end gives `""`. `xs[..]` (no bound) is SYN-001 with a hint to
  assign the array, and a `loop` range still needs both bounds: `loop i in
  a.. {` and `loop i in ..n {` now say so (SYN-001, hints name `a..length(xs)`,
  `xs[a..]` and `0..n`) instead of "expected an expression after '..'".
  Backlog probe `open_slice` moved to `tests/fx/lower/slice_open_w435_pass`.
- **A slice evaluates its base and bounds once.** The array lowering copies
  element by element and used to re-evaluate the base per element and the
  bounds per pass, so `made()[1..3]` called `made()` twice and printed its
  effect twice. A base that is not a plain variable/field/index chain, or that
  makes a call, now goes into a temp first; so does a bound that makes a call.
  Plain bases are unchanged (no extra copy).
- **A reversed array slice is ARR-003.** `xs[2..1]`, or `xs[4..]` on three
  elements, failed with the uncoded backend line "SetLength: array length must
  be non-negative."; it is now `[ARR-003] slice start 4 is past its end 3:
  ...` on stderr, exit 1. A start or stop past the end of a bounded slice was
  already the VM's ARR-003 and is unchanged.
- **Tuple forwarding (D53).** In a function returning `(A, B)`, `ret g(x);`
  where `g` returns `(A, B)`, and `ret t;` where `t` holds one (`let t = g();`,
  `let t: (A, B) = g();`), used to be `[TUP-001] a tuple-return function must
  return a tuple literal`. They are accepted: each tuple fn has its own record
  type, so the value is copied field by field into this fn's tuple return, and
  a tuple `@post` sees it. Recursive-descent parsers written by models do
  exactly this (`ret parseNumber(s, pos);`). What stays TUP-001 names the legal
  forms: a different tuple type ("this function returns (Int, Int), but the
  returned value is (Int, Text)") or any other value ("... a tuple literal
  `(...)`, a call to a function returning (Int, Int), or a variable holding
  one").
- **A `fn` inside a function body is FUNC-001 (D50, W6).** Models write
  `fn backtrack(row: Int) -> Void { ... }` inside `main` to capture locals
  (N-Queens, Dijkstra); it was `[SYN-001] expected ')' to close argument list`.
  It is now `[FUNC-001] functions cannot be declared inside another function`,
  hint "move it to the top level and pass the values it needs as parameters (or
  a record for shared mutable state)", at the inner `fn`. Nested functions stay
  out of the language. Surface registry `nested_fn` now holds.

Spec: G.FnDecl.3 is `@reject=FUNC-001` (was SYN-001); new G.Postfix.4 (open
slices), G.Postfix.5 (`xs[..]`), G.RetStmt.4 (forwarding), G.RetStmt.5 (a scalar
from a tuple fn) and G.Range.4 (`loop i in 1..`). Fixtures: `tests/fx/lower/`
`slice_open_{array,text,field,w435}_pass`, `tuple_forward_pass`;
`tests/fx/diag/` `slice_open_past_end_fail`, `slice_reversed_fail`,
`slice_no_bounds_fail`, `loop_open_{range,low}_fail`,
`tuple_forward_mismatch_fail`, `tuple_ret_scalar_fail`,
`nested_fn{,_block}_fail`. Guides: full, medium and small `2026-10-17-1`.

## 2026-10-16-5

**A copy of a nested array copies its rows, however they were built; ARR-001
flags a nested write; a `Text[]` element can be indexed by character.**
Reported against `2026-10-16-3` (aether `96f099f`) while writing 2-D DP
training data. Front end only: pscal-core stays at `632f4d1`, rea at `a0fae25`.

- **Nested copies are deep (owner decision 2026-10-16, ahead of the rest of
  D8).** Copying an `Int[][]` used to copy the outer array only. A row written
  as a literal is a static array and was copied with it; a row built by
  appending is a dynamic array and stayed shared. So whether a write through a
  copy reached the original depended on how the row was made:
  `fn poke(t: Int[][]) -> Int { t[0][0] = 77; ret 0; }` left `[lit]` alone and
  changed `[built]`, and `let b2: Int[][] = a2; b2[0][0] = 99;` changed `a2`
  only when its rows were appended. Every copy site now copies each row (each
  level of an `Int[][][]`): `let` and assignment, a call result, concatenation
  (`a + [[3]]`, `f + a`, `a + a`, `ret t + [...]`), slices (`a[0..1]`), record
  and array literals and appends (W4-47, which copied "at depth 1"), and
  `loop row in m` over an `Int[][][]`. At the call boundary a nested parameter
  is copied with its rows when the body writes through two indices
  (`t[r][c] = v`, also `val(s, t[r][c], code)` and the other builtins that
  write an argument); a parameter the body only reads keeps the top-level copy,
  so a read-only grid helper costs what it did. The cost is O(cells) per copy:
  a helper that writes `dp[i][j]` and returns `dp`, called 300 times on a
  150x150 grid, went from 0.33 s to 2.0 s (Debug build), because it used to
  write the caller's rows in place. Lowering: `buildArrayUnaliasStmtRank` and
  `aetherBodyWritesNested` in `src/aether/ast_lower.c`. The rest of D8 (V-strict
  turning ARR-001 into an error, or by-ref) is still pending.
- **ARR-001 covers a write through two indices.** `fn mark(seen: Bool[][], r:
  Int, c: Int) -> Void { seen[r][c] = true; }` used to work on a loop-built
  grid only because its rows were shared, and drew no warning; the write is now
  lost like any other write to an array parameter, and ARR-001 says so.
- **ARR-002 lets a `Text[]` element be indexed by character.** `let lines:
  Text[] = ["ab", "cd"]; println(lines[1][0]);` was rejected ("'lines' is
  declared 'Text[]', a one-dimensional array, but is indexed twice here"). The
  second index on a `Text[]` is character access, read or written; an `Int[]`
  indexed twice is still ARR-002.

Census: the replay lap (218 cases), every `.aether` program under
`components/aether/tests`, `examples/` and the umbrella's `Tests/` (1,003
programs, run against `2026-10-16-3` and this change, stdout and exit status)
and the specialization dataset gates: nothing changes except
`lower/lit_alias_depth1_pass`, which pinned the depth-1 copy and is now
`lower/lit_alias_deep_pass` (`99 2 97 98` → `1 2 3 4`). Nine other programs
differ between runs of the old binary too (random, wall-clock, `par` order).

Docs: the full guide drops its interim caveat that appended rows stay shared on
copy, and its ARR-001 and ARR-002 entries name the nested write and the `Text[]`
case (full guide `2026-10-16-3`; medium and small state only the value-copy
rule, which now holds for every row).

Fixtures: `lower/nested_copy_param_pass`, `lower/nested_copy_sites_pass`,
`lower/lit_alias_deep_pass`, `diag/arr001_nested_write_warn_pass`,
`diag/text_array_char_index_pass`.

## 2026-10-16-4

**A self-append sees the array before it grows, a function named like a builtin
is the one called, and `new Int[n]` is a compile-time error.** Reported against
`2026-10-16-3` (aether `96f099f`). Needs pscal-core `632f4d1` (a user routine
keeps its own parameter modes); rea stays at `a0fae25`.

- **Silent wrong result: the appended element saw the grown array.** `t = t +
  [length(t)]` stored 2 for a one-element `t`, and `s = s + [s[length(s) - 1]]`
  appended 0, not the last element: the append grows the target first and then
  evaluates the element into the new slot. So the prefix-sum idiom `sums = sums
  + [sums[length(sums) - 1] + x]` over 4, 0, 12 gave `0 4 0 12`, not `0 4 4 16`,
  with exit 0. An element that reads the target's variable, or makes a call, is
  now evaluated into a temp before any append step (and before the copy in `b =
  a + [..b..]`); this covers fields (`o.xs = o.xs + [..]`), rows (`g[0] = g[0] +
  [..]`) and chains (`c = c + [length(c)] + [length(c)]` appends the old length
  twice). An element that does neither (`xs = xs + [i * 2]`) lowers as before.
- **A function named like a builtin.** `fn rename(xs: Int[]) -> Void` called as
  the statement `rename(a);` failed with "SetLength expects a string or dynamic
  array target" (exit 1): the compiler passed `a` by address, as the builtin's
  file parameter is. The same held for `inc`, `dec`, `close`, `reset`,
  `rewrite`, `append`, `assign`, `erase`, `read`, `readln`, `eof`, `readkey`,
  `dispose`, `blockread`, `getdate`, `gettime` and `getmousestate`. The engine's
  table of builtin VAR parameters now applies only when the call reaches the
  builtin, so the user function is called with its own parameter modes. A user
  `fn pow(b: Int, e: Int) -> Int` (or `sqrt`, `sin`, `exp`, ...) no longer draws
  the NARROW-001 warning meant for the Real-valued builtin.
- **NAME-001 for a builtin the compiler handles by name.** A function named
  `print`, `println`, `printf`, `write`, `writeln`, `assignfile`, `closefile`,
  `createthread`, `waitforthread`, `lock`, `unlock`, `destroy`, `mutex`,
  `rcmutex` or one of four `yyjson*` predicates, or like a TOON or memory-stream
  getter the type checks read by name (`toon_get_text`, `toon_key`,
  `mstreamcreate`, ...), is `[NAME-001] a function named 'X' collides with the
  builtin `X``. These compiled before and failed far from the cause: `printf`
  silently returned 0, `lock(xs)` was "Mutex id must be integer", and
  `toon_get_text` was a TYPE-001 against the builtin's return type. A method of
  any of these names is unaffected.
- **`new Int[4]` is SYN-001.** It compiled as `(new Int)[4]` and failed at run
  time with the uncoded `VM Error: Pointer does not point to an array for
  element access`. It is now `[SYN-001] `new Int[...]`: Aether has no sized array
  constructor`, with a hint to start from `[]` and grow the row in a loop.

Guides: the full guide is `2026-10-16-2` (the NAME-001 and SYN-001 lines);
medium and small are unchanged.

Fixtures: `lower/self_append_reads_target_pass`, `diag/new_sized_array_fail`,
`names/builtin_named_fn_pass`, `names/builtin_handled_by_name_fail`,
`names/toon_builtin_named_fn_fail`.

## 2026-10-16-3

**Two TOON false positives are gone, and the `_or` getters and `toon_len` do
what the guides say.** Reported against `2026-10-15-2`. Needs pscal-core
`e48ce37` (the native `_or` getters, `toon_len` on an absent node); rea
stays at `bdbf9ce`.

- **A field named like a TOON handle.** With a `ToonNode` local `orders` in
  scope, `b.orders = b.orders + 1` was `[TOON-001] opaque TOON handle 'orders'
  cannot be used in arithmetic expressions`; a `Text` local of the same name
  compiled. A name right after `.` is a field and is no longer looked up as a
  handle; `0..orders` is still a range bound, and a bare `orders` in the same
  expression is still TOON-001 (reported once, not twice).
- **A scalar getter inside a larger expression.** `let v: Real =
  toon_int_value(cell) * 1.0;`, `let big: Bool = toon_int_value(cell) > 2;` and
  `let c: Real = toon_get_int_or(root, "k", 0) * 1.0;` were `[TYPE-001] binding
  for 'v' must use Int when initialized from 'toon_int_value'`: the check read
  the first identifier on the right, not the whole expression. It now applies
  only when the getter call is the whole right-hand side; anything else is typed
  by the engine like any other expression. An Int getter also initializes or
  assigns a `Real` binding (`let w: Real = toon_int_value(cell);`, `r =
  toon_int_value(cell);`), as `let x: Real = n;` with `n: Int` already did. A
  `Text` or `Bool` binding from an Int getter is still TYPE-001.
- **`_or` getters fall back on null and on a wrong type.** `toon_get_int_or`,
  `toon_get_text_or`, `toon_get_real_or` and `toon_get_bool_or` returned the
  type's zero value when the key was present but null or of another type
  (`toon_get_int_or(root, "b", -1)` with `b: 2.5` was 0; `toon_get_text_or(root,
  "a", "fb")` with `a: 3` was `""`). They now return the fallback for a missing
  key, a null and a wrong-typed value alike; an Int still satisfies
  `toon_get_real_or`, and an Int fallback to it is a Real (`2.000000`, it printed
  `2`). Each lowers to one native call (`YyjsonGet{String,Int,Number,Bool}Or`)
  instead of `YyjsonHasKey(...) ? Getter(YyjsonGetKey(...)) : fallback`, so it
  resolves the key once and allocates no node handle.
- **`toon_len` on an absent node is 0.** `toon_len(toon_key(root, "zzz"))`, a
  dotted miss and `toon_at` past the end returned -1; they now return 0, as a
  null node does and as the guides' NEST-001 paragraph says. This closes
  W5-09: the backend conformance probe `toon_len_absent` is now `pass`.

Docs: `mstreamloadfromfile` is `-> Bool` in the builtin registry and the full
guide's appendix (the runtime always returned Bool); `toon_null_value` is noted
as Bool. Guides (their own stamps): full, medium and small are
`2026-10-16-1` (the `_or` rule; medium and small pay for it with cuts).

Fixtures: `diag/toon_handle_field_name_pass`, `diag/toon_handle_beside_field_fail`,
`diag/scalar_call_in_expr_pass`, `basic/toon_or_fallback_pass`.

## 2026-10-16-2

**`val` gives the same answer on every platform.** On Linux, `val("", n, code)`
reported a clean parse (`n = 0`, `code = 0`), while macOS reported `code 1` and
left `n` alone, as the guide says. The builtin read success from libc's
`strtoll`/`strtod` error reporting, which differs between glibc and Apple's
libc. A checked parse therefore accepted the empty string on the Linux graders,
and the reference for `tasks_frontier:val_checked_ledger` failed there.
pscal-core `08e2941` now checks the syntax itself, with Pascal `Val` semantics:

- `code` is 0 on success. Otherwise it is the 1-based position of the first bad
  character, or one past the end when the text stops early: `""` → 1, `"-"` →
  2, `"  "` → 3, `"1e"` → 3. `n` is left unchanged.
- Leading blanks and tabs are skipped. Anything after the number is an error,
  including trailing blanks.
- An `Int` overflow reports the digit that leaves the range (it reported one
  past the end).
- A `Real` target still accepts `inf`, `infinity` and `nan`. It now rejects hex
  floats (`"0x1p3"` → 2), and `"1e-400"` parses to 0 instead of failing.
- `""` into a `Real` is now an error on macOS too; it used to give `0.0` with
  `code 0`.

Programs that stay on the documented cases (the guide's table) print the same
output on macOS as before. Needs pscal-core `08e2941`; rea stays at `bdbf9ce`.
Fixture: `tests/fx/basic/val_edges_pass`.

## 2026-10-16-1

**`toon` is an ordinary name, and three TOON/MStream diagnostics name the real
helper.** The Qwen3.8-27B fine-tune pilot (2026-10-15) tripped on all three.
Needs rea `bdbf9ce`; pscal-core stays at `2cb751d`.

- **`toon` as a name.** Any `toon:` followed by whitespace was taken for the
  start of an embedded TOON block, so `fn count(toon: Int)`, `let toon: Int = 3;`
  and `Box { toon: 1 }` failed with an uncoded `Aether TOON parser error: only
  whitespace or comments may follow 'toon:'` — at line 1, because the block
  pass never counted the lines before a marker. A block marker now has to end
  its line (a `//` comment may follow); `toon:` with code after it is a name and
  compiles. After `=` it is still a marker, so inline TOON content
  (`let p: Text = toon: a: 1`) keeps that error, now `[SYN-001]` at the right
  line with a hint, as is `'toon:' must be followed by an indented TOON block`.
- **One-argument field getter.** `toon_get_text(node)` (likewise `_int`,
  `_real`, `_bool`) is still `[BUILT-002]`, but its hint now names the node-value
  accessor, `toon_text_value(node)`, instead of the `_or` fallback form: one
  argument almost always means an array element read with a field getter.
- **Invented helper names.** `[SCOPE-001] identifier 'x' not in scope.` is
  followed by `hint: did you mean ...?` for the common wrong guesses:
  `toon_get_root` → `toon_root`, `toon_get_key` → `toon_key`, `toon_get_at` →
  `toon_at`, `toon_get_str` → `toon_get_text`, the bare `toon_int`/`toon_text`/
  `toon_bool`/`toon_real` → the `_value` accessors, `toon_length` → `toon_len`,
  `has_toon_parser` → `has_toon`, `yyjson_parse_file` → `toon_parse_file`, and
  `mstream_new`/`mstream_create` (and the underscored spellings of the other
  MStream helpers) → `mstreamcreate` and friends. The table is
  `aetherUnknownIdentifierHint` (`diagnostics.c`); rea prints it through its new
  `unknownIdentifierHint` front-end hook. The diagnostic's first line is
  unchanged.

## 2026-10-15-2

**A broken contract is coded CON-001, on stderr.** A failed `@pre` or `@post`
printed `Aether @pre failed in f` as an ordinary line on **stdout**, with no code,
and exited 1. A harness diffing stdout read it as wrong output, and nothing tied
it to a guide section. It now prints `[CON-001] Aether @pre failed in f` (or
`@post`) to **stderr** and still exits 1; stdout carries only what the program
printed before the failure. The guard's `writeln` targets the VM's `stderr` file
(`buildContractGuard`). All three guides gain a CON-001 repair bullet. Front end
only; the pins stay at pscal-core `2cb751d` and rea `b1320cf`.

## 2026-10-15-1

**A foreach over a field compiles; a slice in a loop condition is rebuilt every
pass.** `loop x in o.items` was `[TYPE-001] cannot infer the type of the
collection`, and the full guide told models to bind the field to a typed local
first. An array slice or a record literal in a loop condition was built once,
before the loop, so `loop total(xs[i..n]) > 5 { i = i + 1; }` never ended. Front
end only; the pins stay at pscal-core `2cb751d` and rea `b1320cf`. Outside the
L-series: two defects, shipped together.

- **Foreach over a field.** `loop x in o.items` (and `o.inner.f`, `self.f` in a
  method or an extension method, `items[i].f`, a Text or String field, a row of a
  2-D field, a method called on a field) is typed the way a slice's base is and
  loops. Binding the field to a typed local first still works. An unknown field is
  `FIELD-002`; a bare field name in a method is `TYPE-001` with a hint to write
  `loop v in self.f`. A tuple item and a field of a type from a `use`d module still
  need the typed local.
- **Slices and record literals in conditions.** An array slice or a record literal
  in a loop condition (`loop`, `loop while`, `while`) or in a range loop's upper
  bound is now rebuilt before every test. In the right operand of `&&` / `||`, a
  branch of `?:` or an if-expression, or an `else if` condition it is built only
  when that part is evaluated, so `n <= length(xs) && total(xs[0..n]) > 0` is
  false instead of an `ARR-003` out-of-bounds stop.
- **Range upper bound.** A range loop re-reads its upper bound before every pass,
  with or without a slice in it, and a slice there now follows that rule. Backlog
  W4-33 records the re-read as the defect to fix: whoever makes the bound evaluate
  once must move the bound's hoists back ahead of the loop with it (the
  `aetherTakePendingHoists` call in `parseLoopRange`).

Guide: the full guide is `2026-10-15-1` (a field is a loop collection); medium and
small are unchanged.

Fixtures: `lower/foreach_field_pass`, `lower/slice_in_condition_pass`,
`diag/foreach_{unknown,bare}_field_fail`.

## 2026-10-13-1

**A slice of a field compiles.** `o.f[a..b]`, a nested field, `self.f[a..b]` in a
method or an extension method, a field of an array element (`items[i].tags[0..2]`),
a row of a 2-D field and the result of a method called on a field were all
`[SCOPE-001] identifier '__aether_slice_0' not in scope`: the slice's hoisted
temporary was declared without a type whenever its base was reached through a
record, and the resolver then refused it. Found on the B0 baseline boards, where
it cost first attempts in three model families (`hard2_expr_eval`,
`hard2_heap_sort`). Front end only; the pins stay at pscal-core `2cb751d` and rea
`b1320cf`. Outside the L-series: one defect, shipped on its own.

- **Typing.** The parser records each field's declared type; a slice base that is
  a field, a field of an element, or a method result on such a receiver takes its
  type from there. A slice of a Text field lowers to `copy` as a Text local does.
  Let-inference for other expressions is unchanged.
- **Diagnostics.** A slice the parser cannot type is `TYPE-001` at the slice
  (tuple item `t.0[a..b]`, a field of a record type declared in a `use`d module, a
  bare field name inside a method, with a hint to write `self.f[a..b]`), never an
  internal name. An unknown field is `FIELD-002`; a slice of a non-array is
  `TYPE-001` (it was an uncoded run-time error); an array slice inside `@pre` /
  `@post` is `ANN-001`.
- **Also changed:** a local declared `String` and sliced now behaves as a `Text`
  (it died at run time with "Cannot assign ARRAY to string").

Fixtures: `lower/slice_of_field_pass`, `diag/slice_{untyped_base,non_array,unknown_field,bare_field,in_contract}_fail`.

Replay (hard_nontoon first attempts of the B0 baseline, 116 attempts, against
2026-10-06-1): pass 53 -> 61; the 9 that move are `hard2_expr_eval` (8) and
`hard2_heap_sort` (1), all from the SCOPE-001 above; nothing that passed on L1
fails. One stored repair attempt moves from that SCOPE-001 to a wrong answer: its
own range drops two elements per step (`xs[0..n - 2]`), which the compile error
had been hiding.

Known and not fixed here: a slice in a `loop` condition is evaluated once, before
the loop; a call used as a slice base is called once per element copied.

## 2026-10-08-1

**Release L1 (front end batch A, with batch B items W4-19, W4-22, W4-25, W4-32 and
W4-47): the wrong priors models bring become coded errors, and several silent
behaviours stop being silent.** Placeholders in `println`, methods on builtin
types, `//` as division, chained comparisons, printing an array, `int()` of Text,
case-insensitive name collisions, a `fn main` that never runs and unterminated
strings all used to compile; most printed something plausible and exited 0. Each
is now a coded error whose hint names the Aether form. No engine change: the pins
stay at pscal-core `2cb751d` and rea `b1320cf`. Not in this release, because their
decisions wait on measurement: rejecting discarded values and definite-return
analysis (W4-03/W4-04, D39) and the `/` rule (W8-13, D4).

### Entry points and bindings

- **The entry-point rule (D16, ENTRY-001).** `fx { println("top"); } fn main() -> Void { ... }`
  printed `top` and skipped main with exit 0; it is now `[ENTRY-001] fn main will not
  run: this file also has top-level statements`. A file with nothing to run (no main
  and no statements, only comments, or empty) and `fn main(args: Text[])` /
  `fn main() -> Text` are ENTRY-001 too. Script mode (statements and no main), an
  explicit `main();` call and a file that declares a `mod` stay legal (W4-05).
- **A file-scope `let` with a non-literal initializer is in scope again.**
  `let xs: Int[] = mk();` beside `fn main` was `[SCOPE-001] identifier 'xs' not in
  scope` at every use since the 671882a pin; it now works, as do a file-scope record
  literal and a file-scope `let (a, b) = pair();`, which ran nothing (W4-06).
- **A main-program `let n: Int = 3.7;`** truncates with its NARROW-001 warning, as in
  a fn, instead of aborting with an uncoded `Cannot assign REAL to INT64`. `t = r;`
  from a Real binding is a NARROW-001 warning, not TYPE-001, and Int into a Real is
  accepted (W4-08).
- **`let n: Int = Int(x);`** (and `Real(...)`, `Bool(...)`) compiles; it was
  SCOPE-001. `fn __init__` in a type is `[SYN-001] '__init__' is not a constructor`
  naming `new T { field: value }`; it compiled as a method nothing called (W4-10).
- **An unterminated string literal is SYN-001.** `"abc;` printed `abc` with the `;`
  swallowed, `"C:\data\";` printed `C:\data"`, and a raw newline inside a literal was
  reported as SCOPE-001 on the next line (W4-13).
- **TOON Bool accessors print `true`/`false`** and are real Bools in `-> Bool` returns
  and Bool[] literals; they printed `1`/`0` (W4-09).

### Names and parsing

- **No more "Undefined global variable" from wrapped code.** Wrapping a `let` or a
  `let (a, b) =` before its initializer compiled clean and failed at run time; two
  destructures, inline-object calls or concat returns on one line collided.
  Synthesized temps now share their declaration's line and take the parse serial.
  The reformat-invariance known list loses every "Undefined global" pair (W7-04).
- **A contract clause is one whole expression.** `@pre x > 0, x < 100`, `@post a b`
  and `@pre v in 0..100` enforced only their first part; they are ANN-001 with a fix
  hint (W7-09).
- **Comparisons do not chain (D32).** `0 <= i < n`, `a == b == c`, `a < b == c` and an
  unparenthesised `not a == b` are PREC-001; `18 <= age < 65` computed
  `(18 <= age) < 65`. Parenthesised forms are unchanged (W4-15).
- **Methods no longer hijack builtins or free functions.** `f(obj)` calls a method
  only for an extension method; a function or method named `length`, `setlength`,
  `copy` or `halt`, and any identifier starting with `__`, is NAME-001 (W4-12).
- **NAME-001 ignores case and covers more scopes (D17).** `let limit` beside
  `const LIMIT` overwrote the const; a parameter `limit` answered to `LIMIT`; a loop
  variable named like a visible `let` left the `let` at 0. Each, and a local or
  parameter reusing a top-level or imported const or `let`, is NAME-001. Interim:
  `let s = trim(s)` with an outer `s` is NAME-001 until the engine compiles the
  initializer before the slot (W4-22). This reverses `tests/scoped_bindings_pass`.

### Wrong priors

- **FMT-001: no placeholders (D48).** `println("{} items", n)`, `{name}`, `{0}`,
  `{:.2}`, `${}` and `%d`/`%.2f` with more arguments printed the placeholder
  literally; they are compile errors naming `println("n = ", n)` and
  `formatfloat(x, 2)`. `f"..."` is FMT-001 instead of SYN-001 (W6-04).
- **`//` is never an operator (D45).** It always starts a comment. Directly after an
  expression, `// 2` or `// (` is error DIV-002 and other expression-shaped text a
  DIV-002 warning; `x // 2;` used to divide while the same line without `;` was a
  comment (W8-14).
- **Printing an array and `int()` of Text are TYPE-001 (D14).** An array argument to
  print/println printed `ARRAY(dims:1,...)`; `int(Text)` was 0. The hints name the
  element loop, `parse_int`/`parse_float` and `ord`. Reverses the 2026-07-19
  docs-only decision (W8-15).
- **A statically Real value into `%`, `div`, `shl`/`shr`, `int_to_text`, `chr`,
  `copy` or a Text index is TYPE-001**; these stopped with uncoded VM errors. A Real
  array index still truncates, pending the `/` decision (W4-19).
- **A `File` cannot be a parameter, return value, record field or copied binding**
  until File copies are reference-counted; it crashed with rc 139 or corrupted the
  heap (D34, W4-32).
- **Methods on builtin types (D7, D21).** `xs.push`, `.size()`, `s.contains`,
  `n.toString()`, `s.toUpper()` and `t.pos(x)` are SCOPE-001 naming the Aether form;
  they were codegen errors without a file, run-time "Undefined global" errors or
  silently wrong output. `xs.length` / `s.length` lower to `length(...)`; builtins
  whose first argument is the receiver keep working (`s.trim()`) (W6-05).
- **The TYPE-001 "cannot infer" hint names the initializer's type** (`Text[]`,
  `Real`, ...), never `let x: Int` for an array (D15, W6-06; inferring homogeneous
  array literals stays scheduled for L2).
- **Tuple annotations (D53).** `let t: (Int, Text) = f();` is accepted when it matches
  the called tuple fn. A mismatched annotation (also on destructuring, which used to
  swap fields silently), a tuple parameter, field or array element, and `(a, b) =
  f();` are TUP-001 (W4-25).

### Arrays in literals

- **An array placed in a record literal's field or an array literal's element is a
  copy (W4-47).** `new Box { data: xs }`, `Box { data: xs }`, `[xs]`, `g = g + [xs]`,
  `ret [xs]` and `f(new Box { data: xs })` used to share storage with an `xs` built by
  appending, so a later `b.data[0] = 99` silently changed `xs` (a literal-built `xs`
  was unaffected). They now copy as `x = xs` already did, one level deep: the rows
  of an `Int[][]` placed in a field stay shared with the source's rows, as in
  assignment. Field-to-field assignment (`b.data = c.data`) still aliases; it needs
  the typed pass and is tracked for L2.

Census: every program in tests/, examples/ and the umbrella's corpus candidates was
compared on its AST and diagnostics, exit status and stdout against b1a8bed, item by
item. The only changes outside the new fixtures are the ones each item names, plus
four corpus programs: two whose golden output was an array pointer dump (already
non-canonical), `09_linked_list` (a `length()` method, NAME-001) and
`61_module_const_import_quoted` (a local `greeting` beside an imported `Greeting`,
NAME-001). The umbrella re-records or renames them. The 218 frozen replay programs
are unchanged. The reformat-invariance known list falls from 90 to 62 entries.

## 2026-10-07-1

**Release L0 (front end batch A, pscal-core core-0, rea r-0): eight silent-wrong
or crashing behaviours fixed. A nested `continue` advances only its own loop,
a missing operand is `[SYN-001]`, `exit(n)` ends the program, `Int` arithmetic
from literals no longer wraps at 32 bits, array indexing no longer leaks, a
`println` from a `par` branch is never torn mid-line, `readln` writes no escape
into piped output, and a zero-argument record call assigned to an lvalue is a
call.** Every fix has a fixture; the gate log is in the pin commit.

### Front end

**A nested `continue` advances only its own loop** (`tests/loop_nested_continue_pass.aether`).
The range/foreach lowering puts the counter step before each `continue`. It
also reached into inner loops that were already lowered, so an inner
`continue` stepped every enclosing `loop i in a..b` / `loop x in xs` counter,
with exit 0:

```
loop i in 0..3 { loop j in 0..3 { if j == 1 { continue; } print("(", i, ",", j, ")"); } }
```

printed `(0,0)(1,2)(2,0)(3,2)` and now prints `(0,0)(0,2)(1,0)(1,2)(2,0)(2,2)`.
A `continue` in a `loop cond { }` nested in a range loop was hit the same way.
8-neighbour counts, pair loops and Life were silently wrong. All three guides
already state the correct semantics. Rea had the same rewriter and the same
bug; it is fixed in r-0 (below).

**A missing operand or index is `[SYN-001]`** (`tests/dangling_operand_fail.aether`,
`tests/array_empty_index_fail.aether`, `tests/param_list_single_error_fail.aether`).
An operator with nothing after it used to compile into a program with a hole
in it and exit 0. `if x == { ... }` ran neither branch, `ret x + ;` returned
nil from a `-> Int` function, `println(x + )` printed a blank line,
`let a: Int = xs[];` read whatever value was evaluated just before it, and
`println(xs[1..])` dropped its argument. Each is now
`[SYN-001] Aether parser error: expected an expression after '+', found ';'.`
The `xs[]` case says "empty index `[]`", and adds "to append, write
`xs = xs + [v];`" when an assignment follows. A bad parameter list now reports
one error; the false "functions must declare an explicit return type" that
followed it, even when the signature had `-> Int`, is gone. No program that
compiled correctly before is affected.

**`exit(n)` ends the program with status n, and an entry `fn main() -> Int`
sets the exit status** (`tests/exit_in_helper_status_pass.aether`,
`tests/main_returns_int_status_pass.aether`, `tests/user_fn_named_exit_pass.aether`,
`tests/exit_no_arg_pass.aether`). `exit` reached the backend's Pascal
`Exit(value)`, which returns from the enclosing function, so in a helper the
program carried on:

```
fn check(n: Int) -> Int { if n < 0 { fx { println("bad"); exit(1); } } ret n; }
```

printed bad, then 1, then the rest of the program, and exited 0. In a
`-> Bool` function exit returned true, in a `Text` function it was a type
error, and in a `Void` function an uncoded error. `exit` is now an alias of
`halt` (still fx). `fn main() -> Int { ret 2; }` now exits 2; it used to exit
0. A user's own `fn exit`, or any user function named like an aliased builtin
(`fn sleep(n: Int) -> Int` used to fail with "Built-in procedure 'delay'
cannot be used as a function"), is called as written.

### Engine

pscal-core core-0 (`b801f63`..`2cb751d`) and rea r-0 (`b1320cf`):

- **Indexing an array no longer leaks** (conformance `leak_guard`). Every
  element read or write allocated an ArrayObj that only the Pascal
  pointer-to-array path ever freed: 8e6 reads peaked at about 650 MB and now
  at 3 MB.
- **`Int` arithmetic on literal-derived values no longer wraps at 32 bits**
  (conformance `int64_literal`, `int64_overflow_trap`; `tests/recursion_pass.aether`
  now computes `factorial(20)`). An INT32 result that does not fit widens to
  INT64: `fact(20)` printed -2102132736 and now prints 2432902008176640000,
  `30*24*60*60*1000` is 2592000000, and `fact(25)` is a runtime integer
  overflow with exit 1 instead of 2076180480 with exit 0. `parse_int` past
  2^31 and `<<` still wrap (`int64_paths`, release L1). A store into a
  narrower Pascal or CLike variable still truncates there; only unstored
  intermediates change (D19 probes).
- **A `println` from a `par` branch is never torn mid-line**
  (`tests/par_stdout_lines_pass.aether`, now gating). One `write`/`writeln`
  call holds the stream lock from its first argument through the newline.
  Line order between branches is still unspecified.
- **`readln` from stdin no longer writes a cursor escape into piped or
  redirected output** (conformance `stdin_pipe_no_escape`). Piped output began
  with `ESC[?25h`. Leading-whitespace stripping and the 1023-byte line split
  are still open (`stdin_pipe`, release L1).
- **A zero-argument record- or tuple-returning call assigned to an existing
  variable, field or array element is a call** (conformance
  `zeroarg_record_call`). `r = mk();` stored `mk`'s address, and the next field
  read failed with "Cannot access field on a non-record", exit 1.
- **Rea: a `continue` in a nested loop runs only its own loop's step.**

Pin gate (pscal-core `2cb751d`, rea `b1320cf`): build, conformance, `tests/run.sh` uncached and on a cold and warm cache, rea's suite, the umbrella suites, the D19 probes and an AddressSanitizer lap all pass. Corpus A/B of the engine change over the umbrella's 748 corpus candidates: 735 compared, 0 differences (13 nondeterministic left out). D19 is settled ungated: only unstored Pascal and CLike intermediates moved, and every Pascal and CLike golden passes.
Lane census of the front-end changes over tests, examples, corpus candidates
and benchmark sources: the only program whose behaviour changes outside the
new fixtures is corpus `22_game_of_life_random` (its neighbour count uses a
nested `continue`), re-recorded in the umbrella. Deletion sweep (every single-token deletion of the test and example seeds that did not already fail with a code): all 67 deletions that left an operator without an operand and ran silently wrong now fail with `[SYN-001]`, and 37 that failed uncoded or crashed are now coded too; the 53 the sweep also flagged leave a valid expression (`n * f(x)` becomes `n * (x)`).

## 2026-10-06-1

**Raw sockets are effects, and the sandbox reaches builtins run on a thread.
A `socket*` call outside `fx` is now `FX-001`, a `@pure` function may no
longer open a socket, and `--deny net` now stops sockets and a network
builtin handed to `threadpoolsubmit` / `task_spawn`.**

The 13 socket builtins (`socketcreate`, `socketbind`, `socketbindaddr`,
`socketlisten`, `socketaccept`, `socketconnect`, `socketsend`,
`socketreceive`, `socketpoll`, `socketclose`, `socketsetblocking`,
`socketpeeraddr`, `socketlasterror`) had no effect classification in
pscal-core, so every check that reads it treated them as pure. All three
guides already listed `socket*` as effectful; the compiler did not agree:

```
@pure
fn openSocket() -> Int {
    let s: Int = socketcreate(0);   // compiled, and returned a live handle
    ret s;
}
```

and `aether --deny net,proc` let a program create, bind and listen on a
socket with exit 0, while `dnslookup` under the same flag was denied. The
benchmark harness runs model-written programs with `--sandbox-deny net,proc`,
so its network sandbox had the same hole.

They are now network effects like `dnslookup` and the `http*` family
(pscal-core `fb2cf14`). A program that already called sockets inside `fx`, as
the guides teach, is unaffected. One that called them outside `fx` now gets
`[FX-001] Aether effect error: call to 'socketcreate' requires an fx block.`,
and the `@pure` example above also gets `[ANN-001] Aether purity error: pure
function 'openSocket' cannot call effectful builtin 'socketcreate'.`
`builtin_info(...)` and `builtins_json(true)` now report `"effectful": true`
for all 13, and the generated builtin appendix in the full guide marks them
*fx*. `--fx-record` / `--fx-replay` are unchanged: sockets stay out of the
journal and run live on replay, as before.

A second route around `--deny` went through threads (pscal-core `607a4e8`). A
builtin started with `threadpoolsubmit` / `threadspawnbuiltin`, or through
the `task_spawn` / `task_queue` aliases, runs its handler on the worker
without passing the sandbox, so

```
let t: Int = threadpoolsubmit("dnslookup", "localhost");
```

printed `127.0.0.1` under `--deny net,proc` (the raw name was unclassified, so
`--deny proc` missed it too), and `task_spawn("dnslookup", ...)` did the same
under `--deny net`. The target builtin is now checked against the deny list
before the job is queued, and the raw spellings `threadpoolsubmit`,
`threadgetresult`, `threadgetstatus`, `threadcancel`, `threadlookup`,
`threadpause`, `threadresume`, `threadsetname`, `threadstats` and
`threadstatsjson` are effects like their `thread_*` aliases, so outside `fx`
they are now `FX-001`. `par` is unaffected.

## 2026-09-23-1

**A top-level `let` initializes where it stands. Every top-level `let` used to
run its initializer before the program's first statement, so a `let` read the
globals above it as they were before any statement had assigned them.**

The parser filed each top-level `let` with the declarations (functions, types,
consts) rather than with the statements around it, and the backend emitted the
declarations' initializers as one block ahead of the program body. In

```
let a: Int = 1;
a = 5;
let b: Int = a;
```

`b` came out 1, not 5. The same happened after a loop (`let captured = total;`
read `total` before the loop ran) and with records (`let other: Node = head;`
bound whatever `head` held before it was reassigned).

A top-level `let` now stays among the statements, and its initializer runs in
source order. Its binding is still hoisted: the backend defines every global's
slot before any code runs, so a `fn` can read or assign a global declared
below it, exactly as before. A file made of `let`s and a `fn main` still gets
the implicit `main()` call; the `let`s are declarations, not top-level
statements.

Rea had the same bug and gets the same fix (rea `parseRea`, pscal-core
`hoistStatementGlobalSlots`). Pascal (`var` sections precede `begin`) and CLike
(no top-level statements) never interleave the two and are unaffected.

## 2026-09-22-1

**A `use`d file's own `main()` no longer runs, and no longer stops the
importer's top level from running. An importer's globals used to read as
undefined whenever the file it `use`d declared a `main`.**

A file written to run on its own as well as to be `use`d declares a top-level
`fn main` for its self-test. When `use`d, its `main` was already kept from
becoming the program's entry point, but its call to `main` still ran as
module initialisation: the implicit one the parser adds to a file with no
top-level statements, or an explicit `main();`. That call bound to the
importer's `main` by bare name. Codegen then compiled that body on the spot
with no JUMP around it, so the program's top level ran into it and left
through its `ret`. The importer's `main` ran once and the output could look
right, but everything else in the importer's top level was skipped,
including the prologue that defines its globals. A global read in `main`
failed with `Undefined global variable`.

A `use`d file's call to `main()` is now dropped: its `main` is for running the
file on its own. Other top-level statements still run as module
initialisation. Separately, the shared backend now jumps over a routine it
compiles at a top-level call site, so no other call can make the top level run
into a routine body. Rea's parser gets the same change, since `#import` had
the same bug.

The load-time verifier found this. It now follows control flow from each entry
point across the whole chunk (pscal-core), and it rejected the cached bytecode
of `tests/module_imported_main_not_entry_point_pass.aether` for a top-level
JUMP into a routine body.

## 2026-09-05-1

**Loops iterate collections and step; the word operators exist; the shared
lexer no longer reserves other languages' words; arrays compare structurally;
TYPE-002 catches Pascal's scalar names.** Six language changes, each a construct
a model reached for and the language rejected (`docs/ideas_and_todo.md` carries
the traces):

- **`loop NAME in COLLECTION { }`** (also spelled `for`) iterates an array
  (`T[]`), a `Text` (one character at a time) or a TOON array node
  (`ToonNode`). The element is bound at the top of every iteration with the
  collection's element type, so `item.field` and `let x = item;` infer inside
  the body; a row of a nested array binds by value (the same un-alias step
  `let row: Int[] = table[i]` gets); `continue` advances; a call-valued or
  sliced collection is evaluated once. A non-collection is a coded `SYN-001`
  ("cannot iterate over a value of type Int"); an un-inferable one is
  `TYPE-001`. Lowering: `parseForeach` in `src/aether/ast_parser.c`, the same
  index-loop shape the range form emits.
- **`loop i in a..b step n`.** A literal step is inlined and its sign picks the
  comparison (`step -1` counts down with `i > b`); any other step is evaluated
  once before the loop and tested at run time in both directions; a literal
  `step 0` is rejected. `continue` still applies the step. The range variable
  is now registered in the parser's inference table as an `Int`, so
  `let s = i;` inside a range loop infers (it used to be a TYPE-001).
- **`and`, `or`, `not`** are exact synonyms of `&&`, `||`, `!` at the same
  precedence. They used to surface as `[SCOPE-001] identifier 'and' not in
  scope`.
- **The shared-lexer leak is closed.** rea's lexer reserves about 48 words and
  Aether used fewer than half. The rest -- `join`, `spawn`, `match`, `class`,
  `case`, `default`, `switch`, `try`, `catch`, `throw`, `super`, `extends`,
  `alias`, `module`, `import`, `do`, `return`, `my`, the operator word `mul`,
  and the lowercase type names `int`, `text`, `word`, `byte`, `char`, `str`,
  `bool`, `float`, `void`, `mstream` -- are demoted to ordinary identifiers at
  the token boundary (`aetherDemoteForeignKeyword`), so `let join: Int`, a
  field named `word` or `text`, and a method named `match` all work. The
  reserved set is now exactly Aether's own: its keywords and the operator
  words `div mod xor and or not`. Foreign *statement* keywords are still
  rejected, by text, at statement start, and now with a hint naming the
  Aether form (`'return' is not Aether syntax` / `Aether returns with ret`):
  `return`, `var`, `def`, `func`, `class`, `struct`, `import`, `elif`,
  `foreach` always; `match`, `switch`, `case`, `try`, `catch`, `throw`, `do`
  only when the next token cannot continue an expression statement, so
  `match = true;` stays a plain assignment. The `reserved_*` fixtures use
  `for` and `div` where they used `word` and `mul`.
- **`xs == ys` / `xs != ys` on arrays** compare structurally -- same rank and
  bounds, elements pairwise equal, nested arrays recursing, records by
  identity -- instead of dying with the uncoded "Operands not comparable"
  (pscal-core `src/vm/vm.c`, `pscalArraysDeepEqual`; ordering operators on
  arrays stay an error). This is a shared-VM change and benefits every front
  end; rea's own suite passes against it.
- **TYPE-002 completed.** `Char`, `Byte`, `Word`, `Str` and lowercase
  spellings such as `int` or `string` slipped past the 2026-08-17-1 check:
  rea's type resolver accepts Pascal's scalar keywords case-insensitively and
  hands back a transient stub that the guard mistook for a resolved type, so
  they surfaced later as an uncoded `identifier 'Char' not in scope` or, in
  parameter position, an internal `makeValueForType` warning. The check now
  judges a name by Aether's own type table plus registered `type`s
  (`aetherAstIsBuiltinTypeName`), in two stages: the names decidable before
  module loading (rea's scalars, case variants of Aether's types) are reported
  first and gate rea's pass, so the coded diagnostic is the only output;
  everything else is checked after imports load, as before. The suggestion
  table grew to match (`Char` -> `Text`, `Byte`/`Word` -> `Int`, `int` ->
  `Int`, `string` -> `Text`, ...).

Documentation only, same release: dotted TOON paths (`"server.port"` in
`toon_key`, `toon_has_key` and every getter -- objects only, never an array
index) were always supported but forbidden by KEY-001 in all three guides; the
rule now teaches them, and NEST-001 says what is true, that a missing
intermediate degrades rather than crashes. `setlength`, the numeric-cast
spellings `Int(x)` / `real(n)` / `bool(n)`, `toon_key_or` / `toon_null`,
`s[i] = "x"` and arrays of records inside records are documented.

New fixtures: `loop_foreach_pass`, `loop_foreach_scalar_fail`,
`loop_step_pass`, `loop_step_zero_fail`, `word_operators_pass`,
`array_equality_pass`, `identifier_foreign_keywords_pass`,
`foreign_keyword_return_fail`, `foreign_keyword_match_fail`,
`unknown_type_scalar_fail`, `toon_dotted_path_pass`; new example
`examples/base/loop_forms_foreach`.

### The TYPE-002 base work, drafted on 2026-08-17 and unshipped until now

Written against the 2026-08-06 tree, before the upstream `2026-08-11-2` change
that gave the same mistake the `SCOPE-001` spelling. It lands here, so `TYPE-002`
supersedes that spelling: every position and array depth `2026-08-11-2` covered
is still rejected, imported modules are still walked with each diagnostic
attributed to the module's own file, and the three `unknown_type_*` fixtures now
assert `TYPE-002`. What the code adds is the distinct code, the "did you mean"
suggestion, and the stage that runs before rea's pass (above).

**`TYPE-002`: a type name that does not resolve is now an error, and `Float` /
`String` are accepted aliases.**

A type name that was neither a builtin nor a declared `type` used to compile
silently. `buildTypeNode` emits a bare `AST_TYPE_REFERENCE` at `TYPE_UNKNOWN`
for any name it cannot resolve at parse time — which it must, because a
signature may legitimately name a record declared further down the file or
exported by a module that has not been loaded yet — and nothing ever went back
to ask whether the name eventually resolved.

That was not a cosmetic gap. An unresolved annotation lowers to a slot built by
`makeValueForType(TYPE_UNKNOWN)`, which accepts whatever value it is handed, so
the declared type did no checking at all:

```aether
fn asText(n: Int) -> Zorblatt { ret "text!"; }
fn asInt(n: Int)  -> Zorblatt { ret 40 + n; }
fn asReal(n: Int) -> Zorblatt { ret 1.5; }
```

All three compiled and ran — one undefined name behaving as `Text`, `Int` and
`Real` in a single program. A plain typo (`-> Txet`) did the same, with no
diagnostic anywhere. The observed case was a model writing `-> String`
throughout a program: it produced correct output, so nothing ever told the
author the annotation was inert.

`TYPE-002` now rejects such a name, naming the Aether spelling where one exists
(`Double` → `Real`, `Integer` → `Int`, `Boolean` → `Bool`) and the right shape
where it does not (`List` → `T[]`; `Map`/`Set` → no such type, use parallel
arrays or a `type` with array fields). It runs after module loading, so a name
still unresolved at that point is unresolvable rather than merely early —
forward references to a later `type` and to imported types both keep working.

Two knock-on fixes:

- The backend's internal `Warning: makeValueForType called with unhandled type 0
  (UNKNOWN_VAR_TYPE)` no longer reaches the user on this path. It carried no
  code and named nothing actionable; the program is now rejected before codegen.
- `let v: Nope = ...` used to produce an uncoded `identifier 'Nope' not in
  scope`, which pointed at the scope rules rather than the type name. It is
  `TYPE-002` too.

**`Float` and `String` are now documented, accepted spellings of `Real` and
`Text`.** `Float` was already a real alias in the type table but appeared in
none of the guides; `String` was not an alias at all and only "worked" via the
hole above. Both now lower identically to their canonical spellings and are
documented as accepted. They are the only two foreign type names accepted —
everything else is a `TYPE-002` naming what to write instead.

## 2026-08-11-4

**A function may now share a type's name. `new T()` used to dispatch to a
same-named function instead of constructing: an abort when the arities
disagreed, and silent execution of that function when they agreed.**

Aether identifiers are case-insensitive (PSCAL heritage), so `fn widget` and
`type Widget` are one name. A class publishes its constructor under its own bare
name, and codegen accepted whatever occupied that name as the constructor, so
`new Widget()` called the free function with the freshly allocated object as its
first argument. With `fn widget(t: Widget, v: Int)` the frame came up a slot
short and the VM aborted with `Local slot index 1 out of range (declared
window=1, live window=1)`, reported against the function's line rather than the
`new` that mis-dispatched. With `fn widget(v: Int)` the arities matched and the
call simply went through: the function ran during construction, printing and
mutating whatever it liked, and the program exited 0.

Aether has no user-defined constructors (`new T()` is the only one), so any hit
on that bare name was a collision, never a real constructor. Constructor
resolution now requires the `Class.Class` method shape that Rea's constructors
actually carry, and defining a free function no longer overwrites a same-named
class's constructor. Both halves live in pscal-core's shared codegen, so Rea gets
the same correction: a class constructor and a same-named free function are now
independent in either declaration order.

The collision is legal, not diagnosed. Naming a function after a type is still
worth avoiding for readability, and the guides steer away from it.

## 2026-08-11-3

**A `par` branch whose target is defined further down the file now runs. It used
to be skipped silently — no diagnostic, no output, exit 0.**

Declaration order was free everywhere in Aether except here, and the exception
failed in the worst way: the program looked like it succeeded. `par { workerA();
workerB(); }` in a `main` written above both workers produced nothing at all.
The boundary was the function *containing* the `par`, not `main`, so a `par`
inside a helper dropped any target defined below that helper. Branches with
arguments were affected identically.

The cause sits at the seam between Aether's parser and the shared backend.
Aether resolves forward references by running a pre-pass that appends a
body-less prototype for every top-level `fn` ahead of the real declarations
(`aetherRunForwardScan`). The backend compiles each prototype as a routine like
any other — an empty `JUMP`/`RETURN` stub — and in doing so sets the symbol's
`is_defined` flag and a `bytecode_address` pointing at that stub. The real body
compiles later and overwrites the address.

Ordinary calls never noticed, because `CALL_USER_PROC` names its callee and is
resolved late. `spawn` bakes the address in at compile time, and its guard
against exactly this case tested `is_defined` — which the prototype had already
set. So a branch whose body had not been compiled yet spawned a thread onto the
stub, which returned immediately. Rea, which has no prototype pre-pass, took the
guard's other path and worked in both directions all along; that difference is
what made this look like a `par` defect rather than an ordering one.

`is_defined` is documented as "the body has been compiled" but is set for
prototypes too, so a separate `is_body_compiled` now carries that meaning and
the spawn guard tests it. Forward targets take the existing resolve-by-name path
(`vmHostCreateThreadAddr` looks the routine up in the procedure table at spawn
time, when every address is final); targets already compiled still get the
direct address, so bytecode for code that worked before is unchanged.

`ORDER-001` is retired from the three LLM guides along with the silent-skip
warnings in their concurrency sections: top-level declaration order is now free
without exception. The locals constraint those rules also carried is already
stated by `SCOPE-001`.

## 2026-08-11-2

**An unknown type name in an annotation is now a compile error (SCOPE-001) in
every position. A typo'd element type such as `let v: Bogus[] = []` previously
compiled clean; so did `fn f(p: Bogus)`, a `Bogus` return type, and a `Bogus`
record field.**

Aether had no type-name validation of its own. It leaned on pscal-core's
var-decl path in `compiler.c`, which resolves a type specifier only when that
specifier is a bare `AST_TYPE_REFERENCE` sitting directly under the declaration.
That caught exactly one shape, `let v: Bogus = 1`, and nothing else: an array
annotation wraps the reference in an `AST_ARRAY_TYPE` (so `Bogus[]` slipped
past), and parameters, return types, record fields and tuple items are not var
decls at all. The unresolved annotation then reached the backend as
`TYPE_UNKNOWN`, where the only symptom was a runtime `makeValueForType called
with unhandled type 0` warning, or nothing whatsoever.

Validation now lives in Aether's own semantic pass, which checks every
`AST_TYPE_REFERENCE` the walk reaches. That covers all positions and any depth
of `[]` nesting without enumerating call sites, because the parser emits this
node type *only* for an annotation whose name it could not map. Known builtins
never appear (they become `AST_TYPE_IDENTIFIER`), and a type declared later in
the file still resolves, which is why the check cannot live in the parser.

The pass runs after `reaPerformSemanticAnalysis`, which is what resolves `use`
imports, so types exported by an imported module are registered before they are
checked. Imported modules are separate ASTs, so they are walked too, with each
diagnostic attributed to the module's own file rather than the importer's.

Diagnostics are deduplicated per file by (name, line): the parser's
forward-declaration pre-pass leaves every top-level signature in the AST twice,
which would otherwise double-report each unknown type in a parameter list or
return type.

Verified against the 282-program corpus in `Tests/aether_specialization` and
`examples/`: no previously-compiling program is newly rejected.

## 2026-08-11-1

**A tuple whose items include an array type, such as `(Int, Int[])`, now
compiles. It previously failed to destructure and leaked an internal warning
to stderr.**

An Aether type name carrying the `[]` array suffix is a string convention:
`(Int, Int[])` stores the item type literally as `"Int[]"`. Turning such a
name back into a type node is what `buildTypeNodeFromName` is for. It strips
one `[]` at a time and wraps each level in an `AST_ARRAY_TYPE`, matching the
shape the token-stream path builds for an explicit `: Int[]` annotation.
Both tuple sites called plain `buildTypeNode` instead, which has no notion of
the suffix and did a literal `lookupType("Int[]")`.

That single miss broke the two halves of the tuple lowering in different and
individually confusing ways:

* The **per-binding local** failed to resolve, and the failure surfaced as
  `identifier 'Int[]' not in scope`, blaming an absent variable for what was
  a perfectly valid type.
* The **synthesized record field** silently became `TYPE_UNKNOWN`, so
  instantiating it reached pscal-core's `makeValueForType` canary and printed
  `Warning: makeValueForType called with unhandled type 0 (UNKNOWN_VAR_TYPE)`.
  This half fired even when compilation *succeeded*: binding a tuple-returning
  call to a single name (`let t = pair();`) exited 0 while spraying one such
  warning per instantiation.

```aether
fn split() -> (Int, Int[]) {
    let xs: Int[] = [1, 2, 3];
    ret (3, xs);
}

let (n, xs): (Int, Int[]) = split();   // was: identifier 'Int[]' not in scope
```

Both the explicitly annotated and the inferred destructuring forms were
affected, in any item position, and at any array rank (`Int[][]` included).
No new diagnostic code was added, because there was never an unsupported type
here to report: `Int[]` has always been the correct spelling, and the fix is
that it is now accepted. Covered by `tests/tuple_array_item_pass.aether`,
which folds stderr into its expected-output comparison so a returning warning
fails the suite rather than passing quietly.

## 2026-08-09-1

**Array `+` works for chains of any length, in any position, and no longer
corrupts data when the destination is on the right. Bounds errors gain
`ARR-003` and a source line.**

Array `+` is not a VM operation: the front end lowers it to `setlength` plus an
indexed copy loop, which are *statements*, so each lowering has to provide a
statement position to splice into. Three of them do — the `let` initializer, the
`ret` temp, and assignment — and all three were incomplete.

**Chains.** Every lowering peeled exactly ONE operand and left the rest as the
initializer. With two terms that remainder is a plain array and everything
worked; with three or more it was still an array `+`, which reached the VM:

```aether
let r: Int[] = a + b;          // worked
let r: Int[] = a + b + c;      // Runtime Error: ... Got ARRAY and ARRAY
ret quicksort(less) + [pivot] + quicksort(greater);   // unwritable
```

Now a shared collector peels the whole left spine into an operand list and each
lowering initializes from the innermost operand, then appends one step group per
remaining operand — correct for any length, because every step mutates the
destination in place. Ordinary `Text`/`Int`/`Real` `+` is untouched: the walk
stops at the first right operand that is not manifestly array-valued.

**Self-reference.** When the destination appeared in the RIGHT operand, the copy
clobbered it before the steps read it — producing wrong data with no diagnostic
at all:

```aether
ys = [0] + ys;    // "0 0", should be "0 1 2"
xs = xs + [v];    // always correct -- its copy `xs = xs` is a no-op
```

Operands that read the destination are now hoisted into a temp before the copy.
The documented append idiom was only ever safe by that accident.

**`ARR-003`.** An out-of-range index was reported by `computeFlatOffset()`,
which has no VM pointer and so could only print a bare line and exit:

```
Runtime error: Index 1 out of bounds [0..0] in dimension 1.
```

No file, no line, no code — and it exited before the VM's own check could
produce a located error. Bounds are now checked where the VM is in scope, so the
failure carries a code, the offending index, the valid range and a source line.

Also: concat temps are named with a serial as well as a line, since a chain puts
several on one line and they collided (`duplicate variable
'__aether_concat_other_<line>'`).

Regression fixture: `tests/array_concat_chain_pass.aether` — mixed literal/call
operands, element order, five terms, an empty literal mid-chain, two concats on
one line, the `ret` position, assignment position, destination on the right,
both sides at once, self-duplication, and value semantics.

## 2026-07-26-9

**`PREC-001`, and `has_builtin` can now see core builtins.**

`PREC-001` is a new **warning** for a bitwise operator whose right operand is a
comparison. `&`, `|` and `^` bind looser than `== != < <= > >=`, so

```aether
(flags & mask) != 0    // true  -- Bool, what was meant
 flags & mask != 0     // 1     -- Int, parses as flags & (mask != 0)
```

The second is not an error. It yields an `Int` that prints as `1`, so a
permission check written that way looks like it passes.

It is not warned unconditionally, because `&` and `|` double as **eager**
(non-short-circuiting) boolean operators on `Bool` operands, where
`ready & (n == 0)` is a legitimate conjunction. The warning fires only when the
left operand is provably `Int` — an integer literal, or a variable declared
`Int` — which is the bitmask case and not the boolean one. Zero hits across all
66 examples and every fixture.

**`has_builtin` searched only the extended registry** (pscal-core `3a5de72`),
which made it inverted for core builtins rather than merely unhelpful:
`has_builtin("network", "SocketCreate")` answered false on a build where
`socketcreate(0)` returns a working handle, so a guard written the obvious way
skipped code that would have run. It now falls back to the core registry when
the category lookup misses. The category is ignored on that path since core
builtins are not filed under one. A name in neither registry still answers
false — the half that matters, so an absent OpenAI builtin is not waved through.

## 2026-07-26-8

**`readln` no longer aliases every copy of the `Text` it fills** (pscal-core
`7ac4275`, pointer bumped here).

Copying a string Value copies the `StringObj` pointer, so any number of Values
share one object. `readln` reused the object already attached to its
out-parameter, mutating every sharer at once:

```aether
readln(f, line);  a = line;    // "one"
readln(f, line);  b = line;    // "two"
readln(f, line);               // "three"
// a, b and line ALL read "three"
```

**Reading a file into an array produced N copies of the last line**, with no
error and no warning — in a language whose stated purpose is parsing structured
input. It surfaced because a new example that tracks the longest line it has
read reported the last line instead.

Plain assignment was never affected — `s = t;` then `t = "x";` leaves `s` alone,
because assignment rebinds rather than mutating. `readln` now does the same.

Also in this release: four examples closing the last corpus holes — `file_io`,
`sockets`, `branching`, and (from the previous commit) the text/tuple/bitwise
set. No other language behaviour changed.

## 2026-07-26-7

**The `FIELD-003` message no longer contradicts itself.** It said *"only
constant field defaults are supported; set computed values at construction"*
and hinted *"use a literal or constant expression"* — but the check rejects a
named `const` (`= MAX_SCORE`) and any arithmetic over one (`= MAX_SCORE + 1`).
Neither is *computed*, and the second is exactly what the hint recommended, so
someone who hit this the obvious way could not act on the message. It now says a
field default must be a **literal**, names the named-const case outright, and
points at construction for anything else.

No behaviour change — the same programs are accepted and rejected as before.

## 2026-07-26-6

**`ARR-002`: a one-dimensional array indexed twice is now a compile-time error.**
Declaring `Int[]` and then writing `dp[i][j]` is the DP-table shape models
produce — they get the *declaration's rank* wrong, not the indexing syntax.
Aether has real nested arrays, so the fix is one character in the type.

It used to compile and fail at runtime with an uncoded message that names
neither the variable nor the rank:

```
VM Error: Expected a pointer to an array for element access.
```

Per the cs-aug20 eval this was failing `cs_lcs` across models. The new
diagnostic names the binding and its declared type, and offers both remedies —
`Int[][]` with rows built as arrays, or a computed offset `dp[r * width + c]`.

The check is parse-time and deliberately narrow: it fires only when the base is
a plain variable whose *declared* type name carries exactly one `[]`. A slice
(`xs[a..b][i]`) lowers through a temp before the second index, a field base
(`self.rows[i][j]`) is not a bare variable, and `Text` has rank 0 — none are
judged. Zero hits across all 60 examples and every test fixture.

All three guides gain a nested-array section, since the corpus gap is what
produced the wrong declarations in the first place: `examples/base/nested_arrays`
landed in the previous commit and nothing before it showed `Int[][]` written
correctly.

## 2026-07-26-5

**An inferred `let` calling a name that does not exist now says so.** It used to
report a type-inference failure and advise an annotation — which then produced a
*different* diagnostic for the same root cause:

```aether
let db = sqlite_open("x");        // [TYPE-001] cannot infer the type of 'db'
                                  // hint: add an explicit type ...
let db: Int = sqlite_open("x");   // [SCOPE-001] identifier 'sqlite_open' not in scope
```

Both spellings share one cause — the builtin does not exist — and the inferred
form never said so. The hint made it worse by sending the reader toward the
annotation, so the real error looked like a consequence of following the advice.
A model alternating between the two forms sees one program yield two unrelated
codes; one did exactly that and spent a dozen turns concluding the compiler was
non-deterministic.

When the inferred-`let` path cannot derive a type and the initializer calls a
name that is provably unknown — not a declared top-level function, not a
registered VM builtin, not in the function-return table — it now reports
`SCOPE-001` naming the callee, with a hint pointing at the builtin list.

The redirect is deliberately narrow. A real builtin whose return type the
inference table does not carry (`let s = socketcreate(0);`) still gets
`TYPE-001` and the annotate hint, because there the hint is correct; so does an
untyped array literal.

## 2026-07-26-4

**Each thread gets its own random stream, and implicit `Real` → `Int`
truncation now warns as `NARROW-001`.** Both close findings recorded on
2026-07-26.

`rand_seed` in pscal-core is `_Thread_local`, which is what makes `rand_r`
thread-safe — but a worker that never seeded itself started from the same
constant `1` as every other worker and drew an **identical** sequence.
`randomize()` only ever touched the calling thread, and calling it inside each
branch did not help either: it seeded from `time(NULL)`, whose whole-second
resolution handed concurrent callers the same value. So every `par` branch
produced the same "random" numbers, and a parallel Monte Carlo, sampler, or
shuffle silently computed the same draws in each branch and reported a
confidently wrong aggregate.

Each thread now derives its seed on first draw from a shared base mixed with a
unique per-thread index. Three properties hold together: branches diverge
because their indices differ; with no `randomize()` a run stays bit-for-bit
reproducible; and `randomize()` moves the base with microsecond entropy, so two
runs launched inside the same second no longer replay. Which branch gets which
stream is not deterministic — indices are handed out in first-draw order — but
the streams are independent, which is the property that matters.

`NARROW-001` is a new **warning** (never an error — truncation is sometimes
meant) for a Real-valued expression stored in an `Int` target. It fires on a
Real literal, an always-Real builtin, a call to a function declared `-> Real`,
arithmetic with a Real operand, and specifically on zero-argument `random()`,
whose `[0, 1)` result truncates to a constant `0` and quietly pins every
"random" choice to the same value. `int(...)` is the explicit spelling and is
never flagged.

The check deliberately does not trust the AST's resolved types, because they
are wrong in exactly the places that matter: `min(3, 5)` is annotated `REAL`
though min/max/clamp/abs preserve their operand type, `7 / 2` is annotated
`REAL` though `Int / Int` evaluates to `Int`, `sqr(3)` comes through as `VOID`,
and zero-argument `random()` is annotated `INTEGER` even though that is the
Real arity. A naive check fires on ordinary integer code. Instead the pass uses
a hand-verified always-Real builtin table, an arity check for `random`, and a
newly recorded per-function "declared return type is Real" flag; the call
node's own `var_type` is still `0` during semantic analysis.

It also fires only on an **explicitly written** `: Int`. An inferred
`let x = intVal * realVal;` still reads as integral during the pass even though
it resolves to Real, so warning on it would be plain wrong — only a type the
author typed can contradict its initializer. Verified against the whole corpus:
zero warnings across all 58 examples and every test fixture.

## 2026-07-26-3

**A `type` with methods now survives a `use "..."` import.** Calling any method
on an imported type used to die at runtime with `Runtime Error: Global
'<class>_vtable' not found in symbol table.`, so an imported module could export
functions, constants and method-less records, but not the one thing a C builtin
cannot introduce — a type with behaviour.

An imported module is compiled as its own unit, and the backend prefixed the
enclosing module's name onto every routine name it compiled. For a plain export
that is correct (`StackMod.make`), but a method's name is *already* mangled by
the semantic pass as `class.method`, so a method arrived at codegen as
`stackmod.stack.push` and got a second, differently-named procedure symbol. The
canonical `stack.push` symbol — the one the class's method table, the vtable
emitter, and every call site resolve through — was left with no bytecode
address. `emitVTables` derives the vtable global from the text before the first
dot, so it emitted `stackmod_vtable` (the module's name, and a single table
shared by every class in the module) while `new Stack()` still loaded
`stack_vtable`, which nothing ever defined. Codegen now applies the unit prefix
only to unqualified routine names, leaving class-mangled methods on their
canonical symbol. A visible side effect: the compiler no longer emits a
duplicate copy of an imported method's body at its first call site, which it
did to give the address-less symbol an address.

Two limits are unchanged and unrelated to this fix. Type names are global, so
two imported modules that each export a same-named type still collide, and a
type and a function whose names differ only in case (`Shape` / `shape`) still
collide within a module.

## 2026-07-26-2

**Wrong builtin arity is a compile-time `BUILT-002`, and `toon_parse_string`
aliases to `toon_parse`.** Both come from a 2026-07-26 reasoning trace (full
writeup in `docs/ideas_and_todo.md`) in which a model wrote its first Aether
program from the guide alone.

`BUILT-002` is a new diagnostic for a call with the right builtin name and the
wrong argument count, checked at compile time for a hand-verified set of
fixed-arity conversion and math helpers (`formatfloat`, `realtostr`, `clamp`,
`min`, `max`, `ord`, `chr`, `copy`, `pos`, `trim`, `parse_int`, `parse_float`,
`parse_bool`, `split`). The motivating case is `formatfloat(r, 0, 2)` — the
width slot spliced out of the `println`-only `r:0:prec` form — which previously
reached the VM and failed with the uncoded runtime message `FormatFloat expects
(numeric [, integer precision])`. That meant a generated program passed a
`--no-run` gate and died on its first formatted number; the repair loop also saw
`code:null`. It now fails at compile time and carries `BUILT-002` through
`--diagnostics-json` / `--diagnostics-toon`.

Each arity range is read off the corresponding `arg_count` guard in pscal-core,
not off the published signature: `formatfloat`'s signature says
`(value: Real, precision: Int)` but the VM accepts one or two arguments, so a
signature-derived check would have rejected the valid one-argument call. A
name the user redeclares — a top-level `fn max(a, b, c)` or a method named
`min` — is not judged against the table. Variadic or overloaded builtins
(`length`, `println`, `write`) are deliberately absent.

**Breaking, narrowly:** a program that called one of those fourteen builtins
with a bad argument count used to compile and fail later at runtime. It now
fails to compile. Any such program was already broken at that line.

`toon_parse_string(text)` joins the existing compatibility-alias table
(`parse_json`, `root_node`, `close_doc`, `lookup_string`, `lookup_int`) and
rewrites to `toon_parse(text)`. The name is over-determined by our own surface:
`toon_parse_file(path)` exists, so `toon_parse` reads as a bare stem whose
`_string` sibling ought to exist. Matching is whole-identifier, so the real
`toon_parse` and `toon_parse_file` are untouched. As with every alias, it stays
out of the guides except as a never-generate entry — the canonical name is
`toon_parse`.

## 2026-07-26-1

**`Text` is 0-based.** `s[0]` is the first character (it was a runtime error),
`s[a..b]` is a real half-open substring like an array slice, `copy(s, start,
count)` takes a 0-based `start`, and `pos(needle, s)` returns `-1` when the
needle is absent instead of `0`. Arrays were always 0-based and are unchanged;
this removes the split where `Text` alone indexed from 1.

The semantics landed in `0a557a5` but the version stamp was missed at the time.
This entry records it retroactively, so `aether --version` and the benchmark
provenance stamp finally distinguish 0-based from 1-based builds. Anything
stamped `2026-07-20-1` or earlier is 1-based; note that the cs-aug20 training
corpus and eval results were produced against `0a557a5`/`c660b1b`, which are
0-based despite carrying the older stamp.

Migrating: a hand-managed cursor is the thing to look for, not just `copy()`
call sites. `loop i in 1..string_len(s) + 1`, `let i = n; loop i >= 1`, a
two-pointer seeded at `1`/`n`, `copy(s, i + 1, 1)` inside a `0..n` loop, and
`pos(...) == 0` as an absent-test all still compile and silently do the wrong
thing. The last one is the worst: absent is now `-1`, and `0` is a legitimate
match at the first character, so the test is wrong in both directions.

## 2026-07-20-1

**Three language-level fixes for gaps the corpus/specialization board had been
compensating for with training data instead:** chained `else if` in
if-expression position, array slicing sugar, and a new compile-time warning
for the array-value-copy/record-reference-backed mutation trap. All three
were previously tracked as `docs/ideas_and_todo.md` gaps/ideas; corpus
drilling was the fallback plan for the third, but a static check protects
every future model and human, not just whatever fine-tune got drilled on it.

- **Chained `else if` in if-expression position.** Statement-position
  `if`/`else if`/`else` already supported chains; the expression form
  (`let g: Text = if a {x} else if b {y} else {z};`) only accepted a single
  `else`, rejecting the chained form with a SYN-001. `parseIfExpr`
  (`src/aether/ast_parser.c`) now recurses into itself on `else if`, exactly
  mirroring `parseIfStmt`'s existing chain handling -- each nested branch is
  itself a fully-typed `AST_TERNARY`, so no changes were needed to
  `resolveConditionalType`.

- **Array slicing `xs[a..b]`.** Previously unsupported (`SYN-001: expected
  ']' to close index expression`) -- the most-reached-for missing collection
  operation across 13 model families per the idea-miner findings. Implemented
  as sugar scoped to indexing brackets (`parsePostfix`'s `[` handling),
  **not** a first-class Range value -- consistent with the earlier
  no-closures decision to avoid constructs that can float around ambiguously.
  Lowers to a hoisted temp-array declaration plus a presize-and-copy loop
  (`setlength` + indexed writes, the same idiom `buildArrayAppendSteps`
  already used for `xs + [item]`), spliced into the enclosing statement via
  the existing pending-object-literal mechanism. Half-open range, matching
  `loop i in a..b`'s convention. The element type is inferred from the base
  array's own type (reusing `inferLetTypeName`, the same inference `let x =
  e;` uses) so `xs[a..b]` works for `Int[]`/`Text[]`/`Real[]`/record arrays
  alike, both as a `let` initializer and directly as a call argument.

- **New warning `[ARR-001]`: array-parameter mutation with no observable
  effect on the caller.** Arrays are value-copied at the call boundary,
  unlike records (pointer-backed) -- a `Void` function that writes into an
  array parameter via indexed assignment can never have that mutation
  observed by its caller. This was the single largest compiled-but-wrong
  failure class the specialization board had found (17 identical unsorted
  `quick_sort` outputs across 10 model families). `ARR-001` is a genuine
  **warning**, not an error -- a new `reportAetherWarningCoded`/
  `reportAetherWarning` pair in `src/aether/semantic.c` deliberately does not
  increment `pascal_semantic_error_count`, so it never fails a build (a
  function may legitimately use an array parameter as pure internal scratch
  space). The check (`aetherValidateArrayParamMutation`) is a real AST walk
  modeled on the existing FX-001 effect-fence walk, not a source-text scan.
  Sweeping the full 682-item specialization corpus through this new check
  immediately found 3 real hits -- two were promoted training examples
  (`insertion_sort`, `bubbleSort`) whose verified "expected" stdout was
  *literally the unsorted input*, meaning the corpus itself had been
  training models that this no-op output is what correct sorting looks
  like; both fixed to return-and-reassign and reverified. See the
  Aether-specialization-findings doc / PBuild commit history for the corpus
  side of that fix.

New fixtures: `tests/else_if_expression_pass.aether`,
`tests/array_mutation_warning_pass.aether`, `tests/array_slice_pass.aether`.
Both guide docs updated (if-expression section, Dynamic arrays section).

## 2026-07-19-4

**Added a `File` type, closing the "no file-content read/write API reachable
from Aether's own type system" gap.** The underlying VM's Pascal-style file
I/O builtins (`assign`, `reset`, `rewrite`, `append`, `read`, `readln`,
`write`, `writeln`, `close`, `eof`, `erase`, `rename`) were always real and
working, but Aether's declared-type surface (`Int`, `Real`, `Text`, `Bool`,
`Void`, records, arrays, tuples, `ToonDoc`/`ToonNode`, `MStream`) had no
syntax for the "file variable" binding those builtins require -- `let f:
Text; assign(f, path);` failed at runtime with "First arg to Assign must be
a file variable.", and there was no type name that would satisfy that check.
Investigation found rea (the C-like frontend Aether lowers into) already has
exactly this: its lowercase `text` keyword (distinct from Aether's own
capitalized `Text` string type) maps to `TYPE_FILE`, and
`external/rea/examples/base/hangman5` already exercises the full
`text f; assign(f, path); reset(f); readln(f, line); eof(f); close(f);`
idiom end to end through the shared pscal-core compiler/VM. So this needed
no VAR-parameter-by-reference generality or new opaque-handle machinery --
`readln(f, line)`'s write-back into `line` is handled by compiler.c's
existing per-builtin-name lvalue-argument evaluation (`compiler.c` around
the `is_read_proc` / file-builtin-name checks), keyed purely on the arg's
`var_type == TYPE_FILE`, not on general by-ref params. The fix is a single
binding-table entry in `ast_parser.c`'s `mapAetherType` --
`{ "File", "text", TYPE_FILE }` -- so `let f: File;` now lowers to the same
AST shape rea's own parser produces for `text f;`. New fixture
`tests/file_io_pass.aether` exercises write (`assign`/`rewrite`/`writeln`/
`close`) then read-back (`reset`/`readln`/`eof`/`close`) then `erase`, and
both guides' "Files and environment" sections now document `File` and the
content-I/O builtins alongside the existing existence/metadata ones.

## 2026-07-15-2

**`ret expr` now coerces the value to the function's declared return type,
closing the last numeric-sink gap.** `/` on two Ints produces a Real at
runtime, and every Int-typed sink coerced it back -- `let m: Int = a / b;`
truncates, record fields and array elements truncate, and parameter passing
truncates via `assignRealToIntChecked` at `CALL_USER_PROC` argument setup --
except the function-return path: a `-> Int` function whose body ended in
`ret (a + b + c) / 3;` leaked the raw Real to the caller, so
`println(meanOfThree(12, 30, 30))` printed `24.000000` instead of `24`.
Longstanding, not a regression (reproduced on 2026-07-09-1 both pre- and
post- the setTypeValue fix); rea's `return` had the same gap since the fix
site is shared. Fixed in pscal-core#7 (`095cf67`, "fix(vm): returnFromCall
now coerces the value to the declared return type") by mirroring the
CALL-time parameter coercion against `function_symbol->type`: Real value
into an int-like declared type truncates (range-checked), and symmetrically
an Int value returned from a `-> Real` function now promotes (previously
`fn half(a: Int) -> Real { ret a; }` printed `5`, now `5.000000`). Declared
non-numeric return types are untouched. This makes the guides' "Int / Int
is integer-style division" rule hold through function returns; note that a
*direct* `println(7 / 2)` (no typed sink anywhere) still prints `3.500000`.
New fixture `tests/ret_int_division_pass.aether` pins both directions.

## 2026-07-15-1

**Fixed a SIGBUS/SIGSEGV crash on ordinary recursive functions that divide
their argument (e.g. `digitSum(n)` recursing on `n/10`).** Since
2026-07-09-1 (bisected culprit `e9dbd27`, "VM 2.0 Phase 4i checkpoint 3d --
the physical Value collapse"; last known good `2026-07-01-8`), pscal-core's
`setTypeValue()` freed the *old* `Int64Box`/`LongDoubleBox` when a `Value`
was already one of those box-owning types, but never zeroed `val->bits` when
retyping *into* `TYPE_INT64`/`TYPE_UINT64`/`TYPE_LONG_DOUBLE` from a
different family such as `TYPE_DOUBLE` (whose bits are a raw NaN-boxed
payload, not a pointer). The leftover bit pattern was then misread by the
next `SET_INT_VALUE`/`SET_REAL_VALUE` as a live box pointer and handed to
`pscalObjRelease`, corrupting memory. In practice this hit the Int/Int
division -> recursive-call path directly: a division boxes a `Double`
result, and the callee's Int64 parameter-coercion step (`CALL_USER_PROC`
argument setup, ~vm.c:11202) then retyped that slot and hit the stale bits.
Fixed in pscal-core#6 (`b90a2c7`, "fix(vm): zero bits when setTypeValue
retypes into a box-owning type"), pulled in via aether#10. Programs that
previously crashed now compile and run correctly with no source change; new
fixture `tests/ret_recursion_pass.aether` pins the digit-sum repro. (The
canonical minimal repros bisected upstream are pscal-core's
`Tests/aether_specialization/corpus_candidates/306_recursion_000` through
`_008`.)

Note for board readers: any cs-aug4 training corpus or eval board stamped
`2026-07-09-1` and dated on or after 2026-07-15 was already built against
the post-fix compiler (commit `9a44e67`) -- the stamp just hadn't been
bumped yet when those runs were produced. Boards stamped `2026-07-09-1` from
before 2026-07-15 predate the fix and ran on the crashing compiler.

## 2026-07-09-1

**`MStream` is now a first-class opaque Aether type; HTTP + memory streams
join the curated surface (new rule MS-001).** Previously the memory-stream
handle had no source-level type name at all -- `let s: MStream = ...` failed
`identifier 'MStream' not in scope`, `let s = mstreamcreate()` could not
infer, and the only "working" spelling (`let s: Int = ...`) compiled and then
crashed the VM at runtime with `Cannot assign MEMORY_STREAM to integer`. That
made `httprequest` (whose response body arrives in a memory-stream out-buffer)
unusable from typed Aether; observed in the wild as a local model burning 50
generate/repair iterations on a simple weather-API program with no valid
spelling available.

- `MStream` lowers to rea's `mstream` / `TYPE_MEMORYSTREAM` (unlike
  `ToonDoc`/`ToonNode`, which are integer handles). Explicit declarations,
  inference from `mstreamcreate()` / `mstreamfromstring()` /
  `socketreceive()`, and `mstreambuffer(ms) -> Text` extraction all
  type-check on both the AST parser and the textual type-flow pass.
- The opaque-handle semantic fences were generalized from the ToonDoc/ToonNode
  boolean to a three-kind model. New compile-time MS-001 diagnostics (with the
  standard `help:` guide pointer): a stream-returning builtin bound to a
  non-`MStream` type (the exact former runtime crash), arithmetic on an
  `MStream` binding, and cross-kind assignment/argument misuse between
  `MStream` and TOON handles (`mstreambuffer(doc)`, `let doc: ToonDoc =
  mstreamcreate()`). TOON diagnostics keep their historical wording and
  TOON-001 code.
- HTTP is now documented in both guides (new sections + rule MS-001 +
  repair-rule entry): `httpsession` / `httprequest` / `httpsetheader` /
  `httpclose` with the `MStream` out-buffer shape, fx-gating noted
  (`http*` and mstream file ops effectful; in-memory mstream ops pure), and
  `AETHER_ENABLE_CURL=ON` called out as the build prerequisite.
- New fixtures: `mstream_handle_pass`, `mstream_decl_int_fail` (pins the
  compile-time catch of the former runtime crash),
  `mstream_handle_arithmetic_fail`, `mstream_cross_kind_fail`,
  `http_session_fx_fail`, `http_mstream_compile_pass` (compile-only, passes on
  curl-less builds); new example `examples/base/http_weather` (runs live on a
  curl build).

## 2026-07-04-2

**Tuple returns lowered to a reentrant record-by-value instead of shared
globals; TUP-001's call-cycle rejection and PAR-003 removed as structurally
obsolete.** Since 2026-06-27, `ret (a, b)` / `let (a, b) = f(x)` lowered each
tuple slot to a shared per-function global (`__aether_tuple_N_itemK`), which
is not reentrant — the 2026-07-04-1 bump added compile-time *rejection* of
direct recursion, indirect recursion (`a() -> b() -> a()`), and concurrent
`par`-branch calls to the same tuple-returning function, but valid, idiomatic
code (e.g. a recursive accumulator returning two values) simply could not be
written as a tuple return and had to be rewritten as a record.

Tuple returns now synthesize a hidden record type per signature
(`__AetherTuple<id>`, fields `item0..itemN-1`) and return it through the same
reentrant, per-call-frame record-by-value path an ordinary record-returning
function already used (the VM deep-copies a record on every return —
`returnFromCall`/`copyRecord` in pscal-core). Each call — recursive or
concurrent — gets its own independent result, so the corruption class the
2026-07-04-1 checks existed to catch is now structurally impossible. TUP-001's
call-cycle rejection and PAR-003 are removed accordingly (the "not a known
tuple-return function" / arity-mismatch diagnostics, unrelated to reentrancy,
are unchanged and still carry `TUP-001`). Programs that previously failed to
compile under those two checks now compile and run correctly:
`tuple_recursion_pass.aether` (was `tuple_recursion_fail.aether`),
`tuple_indirect_recursion_pass.aether` (was `tuple_indirect_recursion_fail.aether`),
`par_shared_tuple_call_pass.aether` (was `par_shared_tuple_call_fail.aether`).

## 2026-07-04-1

**TUP-001 generalized to any call cycle through tuple-returning functions;
new PAR-003 for concurrent tuple-fn calls in `par`.** Tuple returns lower to
shared per-function globals (`__aether_tuple_N_itemK`), which are not
reentrant. The 2026-07-01-7 TUP-001 check only rejected a tuple-returning
function calling *itself* directly; indirect recursion (`a() -> b() -> a()`)
and two `par` branches calling the *same* tuple-returning function were both
still silently-corrupting gaps. TUP-001 is now a real call-graph cycle
detector (any cycle length, not just self-calls), and a new `[PAR-003]`
mirrors PAR-001's shared-record tracking for tuple-fn callees, rejecting the
same tuple-returning function called from more than one `par` branch.
Programs that previously compiled and silently corrupted their results in
either shape now fail to compile with a coded diagnostic instead.
Regressions: `tuple_indirect_recursion_fail.aether`,
`par_shared_tuple_call_fail.aether`.

## 2026-07-01-8

**`@pure` above `export fn` now compiles when the module file is the direct
compile target.** The textual annotation validator only accepted a bare `fn`
on the line after an annotation, so the guide-canonical module form
(`@pure` above `export fn`) failed `[ANN-001] @pure must annotate the next
function declaration` when a module file was compiled directly — imports were
unaffected because only the main source runs through that validator.
Surfaced by the 2026-07-01 training-corpus re-verification (12 of 14 module
candidates hit it); pre-existing, not introduced by the -5/-7 batch.
A truly detached annotation (e.g. `@pure` above `export const`) still fails.
Regression: `module_pure_export_direct_pass`.

## 2026-07-01-7

**The parser no longer tolerates malformed input silently.** Four
strictness/correctness fixes, all emitting standard coded diagnostics:

- **Missing closers are hard errors.** An unclosed block, argument list, index
  expression, parenthesized expression, if-expression branch, if-condition
  paren, tuple return/destructure pattern, parameter list, or `type` body now
  emits `[SYN-001] expected '<closer>' to close ... (opened at line N)` instead
  of parsing "successfully". A statement the parser cannot recognize mid-block
  is likewise a `[SYN-001]` instead of silently truncating the block.
- **Fixed-size array types diagnose cleanly.** `let xs: Int[3]` used to
  half-consume the suffix and corrupt the token stream (unrelated downstream
  error); it is now a single `[SYN-001] fixed-size array types are not
  supported.` with a hint pointing at `Int[]`.
- **Malformed print format specs diagnose cleanly.** A `:` in a
  `print`/`println` argument with no number after it (width or precision
  position) used to be swallowed, misaligning the stream; now
  `[SYN-001] expected a number after ':' in print format spec.`
- **Tuple-return functions may not call themselves.** Tuple returns lower to
  per-slot globals, so direct recursion silently corrupted results; it is now
  rejected at compile time with `[TUP-001] tuple-return functions cannot call
  themselves (tuple slots are not reentrant)` and a return-a-record hint.
  (Indirect recursion remains undetected; the guides note the rule.)

Regressions: `unclosed_block_fail`, `unclosed_call_fail`,
`fixed_size_array_fail`, `write_format_colon_fail`, `tuple_recursion_fail`,
`recursion_pass` (non-tuple recursion unaffected).

## 2026-07-01-6

**Every frontend diagnostic is now coded, and name/type tracking is scoped per
function.** Two hardening changes from the same review batch as -5:

- **Coded diagnostics everywhere.** The 44 remaining raw parser errors
  (`L<n>: ...` with no code, path, hint, or guide pointer) now emit the
  standard `path:line: [CODE] ...` format: syntax errors as `SYN-001`, tuple
  limitations as `TUP-001`, imports as `IMP-001`, field defaults as
  `FIELD-003`, inference as `TYPE-001`. The semantic scalar/TOON type-flow
  family (cross-assigns, helper argument mismatches, opaque-handle arithmetic,
  typed-binding rules) emits explicit `[TYPE-001]`/`[TOON-001]` codes at the
  site instead of relying on message-text inference, and the overbroad
  `" first argument"` -> TOON-001 inference pattern now requires a
  `call to 'toon_*'` context. `--diagnostics-json` code assertions added for
  the cross-assign fixtures.
- **Function-scoped binding tables.** The parser's inference bindings and the
  semantic scalar/TOON tables were program-global: one function's locals
  leaked into every later function (last-write-wins), mis-typing same-name
  locals and masking real misuse. Locals (and parameters) are now scoped to
  their function, with globals/imports/function signatures still visible
  program-wide and shadowed globals restored on function exit. Programs that
  accidentally relied on cross-function leakage now get the normal
  `[TYPE-001] cannot infer` / scope diagnostics; same-name locals of
  different types in different functions now each infer correctly, and
  previously-masked TOON handle misuse is reported. Regression fixtures:
  `scoped_bindings_pass` / `scoped_bindings_fail`.

## 2026-07-01-5

**The fx effect fence (FX-001) and @pure purity checks (ANN-001) now run on the
real AST instead of scanning physical source lines.** The parser registers the
facts it used to erase (fx blocks, @pure declarations, builtin-alias surface
spellings) in side registries, and semantic analysis walks the parsed tree.
Three verified line-scan defects are fixed:

- **Escape closed:** an effectful call split across lines (`println` on one
  line, `("hi");` on the next) compiled and ran with no fx anywhere; it is now
  rejected with the standard `[FX-001]` diagnostic at the call's true line.
- **False positive removed:** `fx` with its `{` on the following line parsed
  fine but drew a spurious `[FX-001]` from the text scan; it now compiles and
  runs.
- **Purity hole closed:** a `@pure` function containing an `fx { ... }` block
  compiled (only the calls inside were checked). Per the guide, @pure functions
  may not contain fx blocks at all: the block itself is now rejected with
  `[ANN-001] ... pure function 'f' contains an fx block.`, independent of its
  contents.

Diagnostics keep their codes, file:line format, and surface spellings (a call
written `println`/`sleep`/`task_spawn`/`ai_chat` is quoted that way even though
the AST carries the canonical builtin). Compiler-injected @pre/@post guard
bodies (writeln/halt) are exempt from the fence, as before. Also
`toon_parse_file`'s canonical lowering (`YyjsonReadFile`) is now classified
effectful in pscal-core, keeping file I/O behind the fence on the AST path.

Two more changes ride in this version:

- **The legacy text rewriter is retired.** `translate.c`/`translate.h`
  (~10k lines) are deleted; the AST parser is the only frontend. The
  `AETHER_PARSER` environment variable is no longer consulted and the
  `--dump-rewrite` CLI flag is gone. `docs/parser_roadmap.md` records the
  migration history.
- **Alias lowering no longer rewrites string literals or comments.** The
  per-line alias pass rewrote builtin names inside user strings, silently
  corrupting program output (`println("call sleep(5) now")` printed
  `call delay(5) now`). String literals (with `\"` escapes) and `//` comments
  are now copied verbatim; aliases outside strings lower unchanged.
  Regression: `tests/alias_string_literal_pass.aether`.
- **New examples compile lap.** `tests/run_examples.sh` compile-checks every
  shipped example (capability-gated), registered as the `aether_examples`
  CTest, so the examples tree cannot rot silently.

## 2026-07-01-4

**Record/type fields may now declare a constant default value:
`type Counter { value: Int = 7; }`.** Models reach for field defaults, and a
declared default previously hit the raw `L1: unexpected token in type body.`
parse error. This lands it as a coherent record-initialization improvement.

- **Constant field defaults (`field: Type = <const>`).** After a field's type,
  an `=` introduces a compile-time-constant default: `count: Int = 0`,
  `ratio: Real = 1.5`, `name: Text = ""`, `on: Bool = true`, `xs: Int[] = []`,
  and trivial constant expressions (`-3`, `2 + 3`). The default is applied at
  construction for `new T()` and for the unset fields of `new T { ... }`; an
  explicit construction-site value always overrides it. Parsed on the AST path
  (`ast_parser.c`); the frontend attaches the constant to the field's
  `AST_VAR_DECL`, and the shared engine's `emitDefaultFieldInitializers`
  (pscal-core `compiler.c`) applies it on top of the type-zero so string
  capacity and int→real widening match the record-literal path.
- **New coded diagnostic `FIELD-003`.** A non-constant default (references
  another field, `self`, or calls a function) or a populated array default is
  rejected with `[FIELD-003]` and a hint pointing at construction-time
  `new T { field: value }`, replacing the generic "unexpected token" error. A
  type-mismatched default (`value: Int = "x"`) is a `[TYPE-001]` type error.
- **Guides:** both guides now document constant field defaults (FIELD-003) and
  surface `new T { field: value }` as the recommended way to initialize with
  values (partial sets keep unset fields' defaults), while keeping the
  no-`__init__`/no-`fn new()` stance.

## 2026-07-01-3

**Three compiler errors that were still raw (no `code` in `--diagnostics-json`,
plain stderr) now join the coded set, so models and the repair loop can map each
to its guide section.** All fixes are on the default AST path (`ast_parser.c` /
the shared engine); the legacy `translate.c` rewriter (`AETHER_PARSER=rewriter`)
is untouched. A generative-testing pass 2 flagged these as the remaining uncoded
tier after the 2026-07-01 diagnostic work.

1. **Bare `ret;` in a non-`Void` function -> `FLOW-002`.** An empty `ret;` where a
   value is required used to emit a raw `L<n>: return requires a value.` with no
   code. It now routes through `reportAetherAstError` (kind `function`), emitting
   `[FLOW-002]`, a hint (`ret <expr>;`, or declare `-> Void`), and the guide-help
   pointer. `FLOW-002` is distinct from `FLOW-001` (the fallthrough rule) because
   the fix differs: give the existing return a value vs add a return.
2. **Non-call statement inside `par { ... }` -> `PAR-002`.** The par-arity rule
   ("only direct call statements are allowed inside par blocks") already routed
   through `reportAetherAstError` (kind `par`) but was uncoded because no `par`
   case existed in `aetherInferDiagnosticCode`. It now maps to `PAR-002`, distinct
   from `PAR-001` (the shared-record data-race crash, which is emitted separately).
3. **Backend `Unknown field` -> `FIELD-002` via a collector backstop.** rea's
   semantic pass already codes undefined-field access as `FIELD-002` for a
   resolvable receiver (the `state.state` case the pass flagged is caught here,
   already coded). But a receiver whose record type is *unresolvable* (e.g. a
   method call on a variable of an undefined type) slips past semantics to
   codegen, which emits a raw, uncoded `L<n>: Compiler error: Unknown field
   'T.m'.` The `--diagnostics-json` collector (`rea/src/rea/main.c`) now backfills
   the code for any bracket-less line via the registered frontend's inference, so
   that backend string (and any other uncoded-but-recognized message) carries its
   code in the JSON rather than `code: null`.

`aetherInferDiagnosticCode` (the single source of truth for the message-to-code
map) gains the `return requires a value` -> `FLOW-002` and `only direct call
statements` -> `PAR-002` entries. Both LLM guides gain `FLOW-002` and `PAR-002`
troubleshooting rows (and the enumerated code list in the long guide now includes
`PAR-001`/`PAR-002`, previously omitted). Locked in by `--diagnostics-json`
`"code"` assertions in `tests/run.sh` for all three (`function_empty_return_fail`,
`par_fail_non_call`, `backend_unknown_field_coded_fail`). No language capability
changed.

## 2026-07-01-2

**The tuple-destructuring diagnostic now carries the real `TUP-001` code (was the
placeholder `feature`) and, on the default AST path, an actionable hint.** A
generative-testing pass caught models reaching for multi-value destructuring
(`let (name, age, score) = parseLine(line);`) against a callee that is not a
defined top-level tuple-return function. That correctly fails, but the diagnostic
reported `code: "feature"` in `--diagnostics-json` and emitted no hint, so it did
not feed the code-to-guide-section map that the compiler and guide self-correction
loop rely on.

1. **Every tuple feature-limitation diagnostic now resolves to `TUP-001`:**
   destructure target is not a known tuple-return function, arity mismatch,
   "requires a direct call", binding a tuple-return call to a single name, and
   tuple-return methods. The former placeholder kind `feature` is now the semantic
   kind `tuple`, mapped to `TUP-001` in `aetherInferDiagnosticCode` (the single
   source of truth), so both the default AST path (`ast_parser.c`) and the legacy
   rewriter (`AETHER_PARSER=rewriter`, `translate.c`) agree.
2. **The AST-path destructuring diagnostics gained a hint.** The three were raw
   `aetherDiagf` calls with a hardcoded `[feature]` bracket and no hint; they now
   route through `reportAetherAstError`, emitting `TUP-001`, a hint (previously
   `null` in `--diagnostics-json`), and the guide-help pointer, byte-for-byte
   identical to the rewriter path.
3. **No language capability changed.** Destructuring a direct call to a defined
   top-level tuple-return function (`let (a, b) = pair();` where `pair -> (Int,
   Int)`) still compiles. Both LLM guides now document the alternative for the
   cases that legitimately cannot destructure (a method, an undefined helper, or a
   nested expression): return a record/object and read its fields.

## 2026-07-01-1

**Three silent-failure gaps in the AST frontend now produce coded diagnostics
instead of an empty exit 1 or a crash.** The common thread: a program that failed
with a nonzero exit code but *no message at all* (empty stderr, empty
`--diagnostics-json`) — or that crashed the VM — is the worst case for both humans
and the LLM repair loop, because there is nothing to react to. All three are on the
default AST path (`ast_parser.c`).

1. **Silent parse failures → `[SYN-001]` backstop.** A parse error that set the
   parser's error flag but reported nothing (a `NULL` propagating up a chain that
   only set `p->hadError`) used to exit 1 with empty output. Every parser
   diagnostic now funnels through one counting sink (`aetherDiagf`); after the
   authoritative parse, if it bailed with `hadError` but emitted nothing, a coded
   `[SYN-001]` is emitted, anchored at the token where parsing stalled. Example: a
   non-keyword top-level construct such as `rec Point { ... }` (a common slip for
   `type`) now reports `unexpected token ':'` instead of nothing.

2. **Undefined method on a record → `[SCOPE-001]` at compile time.** The parser
   lowers `recv.method()` on a user record to a call to the mangled global
   `Type.method`; with no such method it silently degraded to an undefined-global
   read that failed only at runtime (`Undefined global variable 'Point.distance'`).
   A post-parse walk (`aetherCheckMemberCalls`) verifies each `Type.method` call
   resolves to a defined method, mirroring the field-read check that already emits
   `FIELD-002`. Conservative by construction (only a real user record type with no
   parent, and only when the name is neither a method nor a field), so it does not
   reject valid corpus programs.

   ```
   let p: Point = Point { x: 1, y: 2 };
   fx { println(p.distance()); }   // before: runtime "Undefined global variable 'Point.distance'"
                                   // now:    [SCOPE-001] method 'distance' is not defined on type 'Point'
   ```

   (Code note: `SCOPE-001`, not `METH-001` — the latter already denotes the
   "methods do not capture outer locals" rule in the guide; the hint cross-references
   it.)

3. **`par` branches sharing a record → `[PAR-001]` at compile time.** Handing the
   *same* pointer-backed record to more than one `par` branch makes the spawned
   threads write it concurrently, a heap double-free that aborts the VM
   (SIGABRT/SIGTRAP) with no diagnostic. `parseParBlock` now rejects the aliased
   record at compile time; the safe idiom (each branch its own record, combined
   after the block) is unaffected. Only user aggregate args are flagged — scalars
   are value-copied per thread and are never shared.

   ```
   par {
       setname(a, "one");   // before: intermittent SIGABRT (concurrent double-free)
       setname(a, "two");   // now:    [PAR-001] record 'a' is shared by more than one par branch
   }
   ```

Regressions: `tests/method_undefined_fail.aether`, `tests/par_shared_record_fail.aether`,
`tests/unknown_construct_fail.aether` (full `tests/run.sh` green under the AST frontend).

## 2026-06-30-4

**A `@pre`/`@post` contract predicate that compares a whole collection to a
scalar is now rejected at compile time (`ANN-001`) instead of crashing the VM at
runtime.** A contract like `@post result > 0` on a `T[]`-returning function lowers
to a runtime guard `if (!(result > 0)) ...`; because pscal-core cannot compare an
array to an integer, that guard aborted with
`Runtime Error: Operands not comparable for operator '>'. Left operand: ARRAY,
Right operand: Int` — a runtime failure for what is really a type error in the
annotation:

```
@post result > 0
fn make(n: Int) -> Int[] { ... ret xs; }   // before: compiled, crashed at runtime
                                            // now:    ANN-001 at compile time
```

The AST frontend now type-checks each contract comparison as it lowers the guard
(`checkContractComparisons` in `ast_parser.c`): a bare `result` in a `@post`
resolves to the function's return-type name, every other operand goes through the
existing binding/return-type inference, and a comparison with exactly one array
operand and one scalar operand is reported with a coded diagnostic that points at
the fix:

```
prog.aether:7: [ANN-001] Aether contract rewrite error: @post predicate compares
an array (`Int[]`) to a scalar (`Int`) with `>`; arrays and scalars are not comparable.
hint: use a collection predicate, for example `length(result) > 0`.
```

The check is deliberately conservative to avoid false positives on the benchmark
corpus: it fires only for the unambiguous array-vs-scalar case (`< > <= >= == !=`)
and leaves array-vs-array and any operand whose type cannot be named untouched.
The documented fix — a collection predicate such as `length(result) > 0` — and all
scalar contracts (`@post result > value` on an `Int` return) compile unchanged.
The same check covers `@pre` predicates over array parameters. Reported by
`qwen3-coder-next`, which hit the runtime crash from a Sieve of Eratosthenes whose
`@post` compared the returned array to `0`.

## 2026-06-30-3

**A `-> Void` method now compiles on an object whose type was *inferred* from
`new`, not only when the instance is explicitly annotated.** Records are
pointer-backed, so a statement-level method call passes the receiver as the
method's `POINTER` `self` argument. The inferred-`let` path built the declared
type node by hand and, for any user type, emitted a bare `AST_TYPE_REFERENCE` at
`TYPE_UNKNOWN` — it never consulted `lookupType` to detect a record and wrap it in
`AST_POINTER_TYPE`. So the variable stayed untyped and a later statement-level
Void-method call failed:

```
type C { value: Int; fn inc() -> Void { self.value = self.value + 1; } }
let c = new C();   // inferred -> argument 1 to 'c.inc' expects type POINTER but got VOID
let c: C = new C(); // explicit -> OK
c.inc();
```

The inferred binding now routes through the same `buildTypeNode` helper the
explicit `: T` annotation uses, which resolves a record/class name to
`AST_POINTER_TYPE -> AST_TYPE_REFERENCE(RECORD)` (var_type `POINTER`). The inferred
and annotated forms now produce a byte-identical declaration, so `let c = new C();`
and `let c: C = new C();` behave the same. Builtin and un-inferable types are
unchanged (the "cannot infer" diagnostic still fires). The same fix covers an
inline object-literal method chain that returns a record
(`let x = Foo { ... }.makeBar();`). Reported by `mistral-small-24b`, which could
not recover from the raw `POINTER`/`VOID` message.

## 2026-06-30-2

**Trailing `//` comments on `@pre`/`@post` annotations no longer leak into the
contract guard.** The AST frontend captured the whole physical remainder of an
annotation line as the contract expression, so a trailing line comment was
lowered into the runtime guard and parsed as code. A comment that happened to
contain an undeclared word failed compilation, e.g.

```
@post result >= 1 // Factorial of 0 is 1, otherwise positive
   -> [SCOPE-001] identifier 'Factorial' not in scope.
```

whereas a comment whose words all resolved compiled silently — the diagnostic
depended on the *prose*, not the code. `collectPendingAnnotations` now stops the
expression at the first unquoted `//`, so comment text is stripped unconditionally
before lowering; a `//` inside a string literal in the expression (e.g.
`@post result != "http://none"`) is preserved. The guard itself is unchanged, so
a contract that should fire still fires. The legacy text-rewriter fallback
(`AETHER_PARSER=rewriter`, `extractAnnotationExpr`) got the symmetric fix, so both
frontends agree. **Not breaking:** programs that compiled
before still compile; programs that erred only because of an annotation comment
now compile. Companion to the still-open "malformed `@cost` is silently dropped"
gap. (Surfaced by generative-model testing; `gemini-3.1-pro-preview`.)

## 2026-06-30-1

**Reserved-word / member-name collisions now name the collision (SYN-001).** A
`type` field or a `fn` method named after a reserved word previously failed with
a generic parse error that never identified the cause: a field colliding with a
type-name keyword (`word`, `text`, `int`, `byte`, `bool`, ...) reported
`unexpected token in type body`, and a method colliding with an operator word
(`mul`/`div`/`mod`/`xor`) or a value/structure keyword (`new`, `for`, ...)
reported `expected function name after 'fn'`. This was the single broadest gap
generative-model testing surfaced (several distinct LLMs hit it) — neither the
model nor a human could act on the message. The AST frontend now emits a SYN-001
diagnostic that names the offending word and its class, e.g. `'word' is a
reserved type name and cannot be used as a field name` and `'mul' is a reserved
operator word and cannot be used as a method name`, each with a rename hint;
`fn new()` additionally points at the (absent) constructor idiom (`new T()` +
field assignment, or a top-level factory `fn`). **Not breaking:** these programs
were already rejected — only the diagnostic improved, on the default (AST) path;
the legacy rewriter path is unchanged. Both LLM guides gained a "reserved words
to avoid as member names" note.

## 2026-06-27-3

**Effect model now covers host interaction (FX-001 / `@pure` soundness).** The
effectful-builtin set that FX-001 enforces (and that `@pure` rejects) previously
covered only output, `read`/`readln`, `ai_chat`, tasks/threads, and `sleep`.
Filesystem (`mkdir`/`rmdir`/`fileexists`/file-handle I/O), environment
(`getenv`), CLI (`paramstr`/`paramcount`), `random`/`randomize`, the external
clock (`gettime`/`getdate`/`realtimeclock`), console input (`readkey`/
`keypressed`), networking (`http*`/`socket*`/`dnslookup`), database (`sqlite*`),
and the `task_wait` family all escaped both checks — so a `@pure` function could
read the environment or call `random` and still compile. These are now
effectful: they require an `fx { ... }` block and are rejected inside `@pure`
functions. Pure builtins (math, string, conversion, in-memory streams,
`toon_parse` on a string) are unaffected. The name match is now
case-insensitive, and the rule is enforced identically on the AST and rewriter
paths. **Breaking:** code that called these outside `fx` (or inside `@pure`)
must move the call into an `fx` block. SDL/graphics and terminal screen-control
builtins are intentionally left ungated.

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
