# Aether Language Specification (skeleton)

*Audience: maintainers, and anyone who needs to know whether a program is
Aether.* This document says what Aether accepts, what it rejects, and what an
accepted program prints. It describes language `2026-10-07-1`.

**Status.** §1 (lexical structure), §2 (grammar) and §6 (leniencies) are
written. §3 to §5 are headed stubs that name the decision rows still open
against them.

**Normativity (decision D30).** This spec is normative. A program the compiler
handles differently from what the spec says is a compiler bug, unless the
behaviour is one of the leniencies enumerated in §6. A spec change that alters
whether some program is accepted or rejected, or what it prints, is a language
change: it bumps `VERSION` and gets a `CHANGELOG.md` entry. A behaviour the spec
does not mention yet is not thereby legal. It is a gap, to be written down or
rejected.

**The examples are tests.** Every example below is a complete program in a fence
tagged for `python3 tools/verify_guide_snippets.py --spec docs/aether_spec.md`
(CTest `aether_spec`), so a wrong claim here fails the build:

| Tag | Means |
|---|---|
| `@ok` | compiles, runs, exits 0 (or `rc=N`), and prints exactly the `text` block after it |
| `@reject=CODE` | fails to compile with exactly that diagnostic code, and no uncoded record |
| `@trap=CODE` | compiles, then stops at run time with `[CODE]` |
| `@bug=D<n>` | pins **today's** outcome (`now=`) of a behaviour a decision row has decided to change or is still deciding (`@bug=gap`: a known defect with no row yet). When it changes, the gate reports it as FLIPPED without failing, and the spec, `VERSION` and the row are updated together |

Ids name the section: `L.` lexical, `G.<Production>.` grammar, `S.`/`D.`/`P.`
the stubs, `LEN.` leniencies. Every production in §2 has at least one `@ok` and
one `@reject` or `@trap` example; the gate checks that too.

---

## 1. Lexical structure

### 1.1 Source text, whitespace and comments

Source is UTF-8. Spaces, tabs and newlines separate tokens and are otherwise
insignificant. `// ...` runs to the end of the line; `/* ... */` may span lines
and does not nest (the first `*/` closes it). A `#!` line at the very start of a
file is ignored.

```aether @ok id=L.comment.1
#!/usr/bin/env aether
// a line comment
fn main() -> Void {
    /* a block
       comment */ fx { println("ok"); }
}
```

```text
ok
```

```aether @reject=SYN-001 id=L.comment.2
fn main() -> Void {
    /* a /* nested */ comment */
    fx { println("never"); }
}
```

**The `//` rule (decision D45).** `//` is never an operator. Today the shared
lexer reads `//` after an expression as integer division when the rest of the
line looks like an expression, so `x // 2;` divides. D45 makes that an error when
the text after `//` starts with a digit or `(`, and a warning otherwise.

```aether @bug=D45 now=ok id=L.comment.3
fn main() -> Void {
    let x: Int = 7;
    let y: Int = x // 2;
    fx { println(y); }
}
```

```text
3
```

### 1.2 Identifiers and keywords

An identifier is a letter or `_` followed by letters, digits and `_`.
**Identifiers are case-insensitive**: `count` and `Count` name the same binding.
Keywords are case-sensitive and lower-case: `Let` is not `let`. The keywords are
`fn let const type ret if else loop while for in step par fx use mod export new
nil true false break continue self`, and the operator words `and or not xor div
mod`. Other languages' keywords (`match`, `class` as a name, `word`) are ordinary
identifiers.

Today only the words the shared lexer reserves are refused as names. The words
Aether matches by text (`fn let ret loop in step par fx use self`, and `and`,
`or`) are still accepted as binding names. That is a gap with no decision row
yet, not a leniency:

```aether @ok id=L.ident.1
fn main() -> Void {
    let Count: Int = 2;
    let match: Int = 3;
    fx { println(count + match); }
}
```

```text
5
```

```aether @reject=SYN-001 id=L.ident.2
fn main() -> Void {
    Let x: Int = 1;
}
```

```aether @reject=SYN-001 id=L.ident.3
fn main() -> Void {
    let div: Int = 1;
}
```

```aether @bug=gap now=ok id=L.ident.5
fn main() -> Void {
    let loop: Int = 1;
    let ret: Int = 2;
    fx { println(loop + ret); }
}
```

```text
3
```

Because names fold case, a second binding that differs only in case is a
collision. Decision D17 makes it a coded error; today it is rejected without a
code.

```aether @bug=D17 now=uncoded id=L.ident.4
fn main() -> Void {
    let total: Int = 1;
    let Total: Int = 2;
}
```

### 1.3 Literals

- **Int**: decimal digits, or `0x` hex digits; `_` between digits is ignored.
  Values are 64-bit.
- **Real**: digits with a `.` and an optional exponent (`2.5e2`). `.5` and `2.`
  are accepted.
- **Text**: `"..."` or `'...'`, on one line. Escapes: `\n`, `\t`, `\"`, `\\`,
  `\uXXXX`. An unknown escape keeps its backslash. Adjacent Text literals join.
- **Bool**: `true`, `false` (lower case). **nil** is the empty record reference.
- There are no binary (`0b`) or octal literals.

```aether @ok id=L.lit.1
fn main() -> Void {
    fx { println(0xff, " ", 1_000, " ", 9000000000 * 2, " ", 2.5e2, " ", .5); }
}
```

```text
255 1000 18000000000 250.000000 0.500000
```

```aether @ok id=L.lit.2
fn main() -> Void {
    fx { println("a\tb\"c\\d", " ", 'x', " ", "ab" "cd", " ", "A"); }
}
```

```text
a	b"c\d x abcd A
```

```aether @reject=SYN-001 id=L.lit.3
fn main() -> Void {
    fx { println(0b101); }
}
```

```aether @reject=SCOPE-001 id=L.lit.4
fn main() -> Void {
    let b: Bool = True;
}
```

```aether @reject=SYN-001 id=L.lit.5
fn main() -> Void {
    fx { println("abc); }
}
```

---

## 2. Grammar

Notation: `"x"` a token, `[ x ]` optional, `{ x }` zero or more, `|`
alternatives. Statements end with `;` (see §6 for where a missing one is
tolerated). Precedence is given by the expression productions, loosest first.

### 2.1 Programs and declarations

```ebnf
Program    = { Item } ;
Item       = UseDecl | FnDecl | TypeDecl | ConstDecl | Statement ;
UseDecl    = "use" Text ";" ;
FnDecl     = { Annotation } "fn" Ident "(" [ Params ] ")" "->" ReturnType Block ;
Params     = Param { "," Param } ;
Param      = Ident ":" Type ;
ReturnType = Type | TupleType ;
TupleType  = "(" Type "," Type { "," Type } ")" ;
Annotation = "@pre" Expr | "@post" Expr | "@pure" | "@cost" Int [ Ident ] ;
```

A program is its items in order. If it declares `fn main`, `main` is the entry
point; if it has top-level statements and no `main`, they run in order (script
mode). A file with both must call `main();` itself, or it is rejected with
ENTRY-001 (decision D16, §5). Declarations may appear in any
order. An annotation takes the rest of its
line and attaches to the next `fn`.

```aether @ok id=G.Program.1
fn main() -> Void {
    fx { println(twice(4)); }
}
fn twice(n: Int) -> Int { ret n * 2; }
```

```text
8
```

```aether @reject=SYN-001 id=G.Program.2
}
fn main() -> Void { }
```

```aether @ok id=G.Item.1
let x: Int = 3;
fx { println(x + 1); }
```

```text
4
```

```aether @reject=SYN-001 id=G.Item.2
class Point { }
fn main() -> Void { }
```

```aether @ok id=G.UseDecl.1
use "geometry";
fn main() -> Void {
    fx { println(Geometry.area(2, 3), " ", Geometry.Sides); }
}
```

```text
6 4
```

```aether @reject=IMP-001 id=G.UseDecl.2
use;
fn main() -> Void { }
```

```aether @ok id=G.FnDecl.1
fn add(a: Int, b: Int) -> Int { ret a + b; }
fn main() -> Void { fx { println(add(1, 2)); } }
```

```text
3
```

```aether @reject=SYN-001 id=G.FnDecl.2
fn f() { ret; }
fn main() -> Void { f(); }
```

```aether @reject=SYN-001 id=G.FnDecl.3
fn main() -> Void {
    fn helper() -> Int { ret 1; }
}
```

```aether @ok id=G.Params.1
fn mix(a: Int, s: Text, ok: Bool) -> Text { ret s; }
fn main() -> Void { fx { println(mix(1, "p", true)); } }
```

```text
p
```

```aether @reject=SYN-001 id=G.Params.2
fn f(a: Int,, b: Int) -> Int { ret a; }
fn main() -> Void { }
```

```aether @reject=SYN-001 id=G.Params.3
fn f(a: Int, b: Int = 2) -> Int { ret a + b; }
fn main() -> Void { }
```

```aether @ok id=G.Param.1
fn total(xs: Int[]) -> Int {
    let s: Int = 0;
    loop x in xs { s = s + x; }
    ret s;
}
fn main() -> Void { fx { println(total([1, 2, 3])); } }
```

```text
6
```

```aether @reject=SYN-001 id=G.Param.2
fn add(a, b) -> Int { ret a + b; }
fn main() -> Void { }
```

```aether @ok id=G.ReturnType.1
fn none() -> Void { ret; }
fn grid() -> Int[][] { ret [[1, 2], [3]]; }
fn main() -> Void {
    none();
    fx { println(length(grid())); }
}
```

```text
2
```

```aether @reject=SYN-001 id=G.ReturnType.2
fn f() -> { ret; }
fn main() -> Void { }
```

```aether @ok id=G.TupleType.1
fn trio() -> (Int, Text, Bool) { ret (1, "b", true); }
fn main() -> Void {
    let (a, b, c) = trio();
    fx { println(a, b, c); }
}
```

```text
1btrue
```

```aether @reject=SYN-001 id=G.TupleType.2
fn one() -> (Int) { ret 1; }
fn main() -> Void { }
```

A tuple type is written only as a return type; decision D7 also accepts a
matching annotation on a binding, which today is rejected:

```aether @bug=D7 now=SYN-001 id=G.TupleType.3
fn pair() -> (Int, Int) { ret (1, 2); }
fn main() -> Void {
    let t: (Int, Int) = pair();
}
```

```aether @ok id=G.Annotation.1
@pre n >= 0
@post result >= 1
@pure
fn fact(n: Int) -> Int {
    if n <= 1 { ret 1; }
    ret n * fact(n - 1);
}
fn main() -> Void { fx { println(fact(5)); } }
```

```text
120
```

```aether @reject=ANN-001 id=G.Annotation.2
@pre
fn f(n: Int) -> Int { ret n; }
fn main() -> Void { }
```

```aether @reject=SYN-001 id=G.Annotation.3
fn f(n: Int) -> Int {
    @pre n >= 0
    ret n;
}
fn main() -> Void { }
```

`@cost` is decorative by decision: its syntax is validated and nothing enforces
it. An unknown `@word` and `@pure()` are accepted today and ignored; W4 makes
them errors.

### 2.2 Types, records and constants

```ebnf
TypeDecl   = "type" Ident "{" { Member } "}" ;
Member     = Field | Method ;
Field      = Ident ":" Type [ "=" Literal ] ";" ;
Method     = { Annotation } "fn" Ident "(" [ "self" { "," Param } | Params ] ")" "->" ReturnType Block ;
ConstDecl  = "const" Ident [ ":" TypeName ] "=" Expr ";" ;
Type       = TypeName { "[" "]" } ;
TypeName   = "Int" | "Real" | "Text" | "Bool" | "Void" | "ToonDoc" | "ToonNode"
           | "MStream" | "File" | Ident ;
```

A record type's fields are `name: Type;`, one per declaration, with an optional
literal default. Inside a method the receiver is `self`, written as the first
parameter or left implicit; fields are read and written through it, never as
bare names (METH-001: methods do not capture outer locals either). A `type` or `const` may also be declared inside a block.
`T[]` is a dynamic array of `T`; `T[][]` an array of arrays.

```aether @ok id=G.TypeDecl.1
type Point {
    x: Int;
    y: Int = 7;
}
fn main() -> Void {
    let p: Point = new Point { x: 1 };
    fx { println(p.x, p.y); }
}
```

```text
17
```

```aether @reject=SYN-001 id=G.TypeDecl.2
type Point { x: Int, y: Int }
fn main() -> Void { }
```

```aether @ok id=G.Member.1
type Counter {
    n: Int = 0;
    fn bump(self) -> Void { self.n = self.n + 1; }
    fn get(self) -> Int { ret self.n; }
}
fn main() -> Void {
    let c: Counter = new Counter();
    c.bump();
    c.bump();
    fx { println(c.get()); }
}
```

```text
2
```

```aether @reject=SYN-001 id=G.Member.2
type C {
    n: Int;
    fn loop(self) -> Void { }
}
fn main() -> Void { }
```

```aether @ok id=G.Field.1
type Box {
    n: Int = 5;
    label: Text = "z";
    on: Bool = true;
    xs: Int[] = [];
}
fn main() -> Void {
    let b: Box = new Box();
    fx { println(b.n, b.label, b.on, length(b.xs)); }
}
```

```text
5ztrue0
```

```aether @reject=FIELD-003 id=G.Field.2
const M: Int = 3;
type C { n: Int = M; }
fn main() -> Void { }
```

```aether @ok id=G.Method.1
type Acc {
    total: Int = 0;
    @pure
    fn peek(self) -> Int { ret self.total; }
    fn add(self, k: Int) -> Void { self.total = self.total + k; }
}
fn main() -> Void {
    let a: Acc = new Acc();
    a.add(4);
    fx { println(a.peek()); }
}
```

```text
4
```

```aether @ok id=G.Method.3
type Tally {
    keys: Text[] = [];
    counts: Int[] = [];
    fn bump(k: Text) -> Void {
        loop i in 0..length(self.keys) {
            if self.keys[i] == k {
                self.counts[i] = self.counts[i] + 1;
                ret;
            }
        }
        self.keys = self.keys + [k];
        self.counts = self.counts + [1];
    }
}
fn main() -> Void {
    let t: Tally = new Tally();
    t.bump("a");
    t.bump("b");
    t.bump("a");
    fx { println(length(t.keys), " ", t.counts[0]); }
}
```

```text
2 2
```

```aether @reject=SCOPE-001 id=G.Method.4
type C {
    n: Int = 2;
    fn get() -> Int { ret n; }
}
fn main() -> Void { }
```

```aether @reject=SYN-001 id=G.Method.2
type C {
    n: Int;
    fn new(self) -> Void { }
}
fn main() -> Void { }
```

```aether @ok id=G.ConstDecl.1
const LIMIT: Int = 3;
const NAME = "x";
fn main() -> Void {
    const K: Int = 4;
    fx { println(LIMIT * K, NAME); }
}
```

```text
12x
```

```aether @reject=SYN-001 id=G.ConstDecl.2
const XS: Int[] = [1, 2];
fn main() -> Void { }
```

```aether @ok id=G.Type.1
fn main() -> Void {
    let g: Text[][] = [["a"], ["b", "c"]];
    fx { println(length(g), length(g[1])); }
}
```

```text
22
```

```aether @reject=SYN-001 id=G.Type.2
fn main() -> Void {
    let xs: Int[3] = [1, 2, 3];
}
```

```aether @ok id=G.TypeName.1
fn main() -> Void {
    let i: Int = 2;
    let r: Real = 1.5;
    let t: Text = "t";
    let b: Bool = false;
    fx { println(i, " ", r, " ", t, " ", b); }
}
```

```text
2 1.500000 t false
```

```aether @reject=TYPE-002 id=G.TypeName.2
fn main() -> Void {
    let x: Integer = 1;
}
```

### 2.3 Statements

```ebnf
Block      = "{" { Statement } "}" ;
Statement  = LetDecl | TupleLet | ConstDecl | TypeDecl | Assignment | IfStmt
           | LoopStmt | WhileStmt | ForStmt | FxBlock | ParBlock | RetStmt
           | "break" ";" | "continue" ";" | ExprStmt | Block ;
LetDecl    = "let" Ident [ ":" Type ] [ "=" Expr ] ";" ;
TupleLet   = "let" "(" Ident "," Ident { "," Ident } ")" "=" Call ";" ;
Assignment = Postfix AssignOp Expr ";" ;
AssignOp   = "=" | "+=" | "-=" | "*=" | "/=" | "%=" ;
IfStmt     = "if" Expr Block [ "else" ( IfStmt | Block ) ] ;
LoopStmt   = "loop" [ Expr | Ident "in" ( Range | Expr ) ] Block ;
Range      = AddExpr ".." AddExpr [ "step" AddExpr ] ;
WhileStmt  = "while" Expr Block ;
ForStmt    = "for" Ident "in" Range Block ;
FxBlock    = "fx" Block ;
ParBlock   = "par" "{" { Call ";" } "}" ;
RetStmt    = "ret" [ Expr | TupleLit ] ";" ;
ExprStmt   = Expr ";" ;
```

`let` bindings are mutable. A `let` without a type is inferred from its
initializer when that is obviously safe and is otherwise TYPE-001 (§3). `loop`
alone repeats until `break`; `loop cond` is a while loop; `loop i in a..b` counts
from `a` up to but not including `b`, by `step` when given; `loop x in xs` visits
each element of an array or each character of a Text. `while` and `for` are exact
synonyms of the corresponding `loop` forms. Effects live in `fx` blocks (FX-001,
§3). `par` runs each call on its own thread and waits for all of them.

```aether @ok id=G.Block.1
fn main() -> Void {
    { let x: Int = 1; fx { println(x); } }
}
```

```text
1
```

```aether @reject=SYN-001 id=G.Block.2
fn main() -> Void {
    let x: Int = 1;
```

```aether @ok id=G.Statement.1
fn main() -> Void {
    let i: Int = 0;
    loop {
        i = i + 1;
        if i == 2 { continue; }
        if i > 3 { break; }
        fx { print(i); }
    }
    fx { println(""); }
}
```

```text
13
```

```aether @reject=SYN-001 id=G.Statement.2
fn main() -> Void {
    var x: Int = 1;
}
```

```aether @ok id=G.LetDecl.1
fn main() -> Void {
    let n = 3;
    let s = "a";
    let later: Int;
    later = n * 2;
    fx { println(n, s, later); }
}
```

```text
3a6
```

```aether @reject=TYPE-001 id=G.LetDecl.2
fn main() -> Void {
    let xs = [1, 2];
}
```

```aether @reject=NAME-001 id=G.LetDecl.3
fn main() -> Void {
    let x: Int = 1;
    let x: Int = 2;
}
```

```aether @ok id=G.TupleLet.1
fn pair() -> (Int, Text) { ret (1, "a"); }
fn main() -> Void {
    let (n, s) = pair();
    let t = pair();
    fx { println(n, s, t.0, t.1); }
}
```

```text
1a1a
```

```aether @reject=TUP-001 id=G.TupleLet.2
fn main() -> Void {
    let (a, b) = (1, 2);
}
```

```aether @ok id=G.Assignment.1
type P { x: Int = 0; }
fn main() -> Void {
    let p: P = new P();
    let xs: Int[] = [1, 2];
    p.x = 5;
    xs[1] = 9;
    fx { println(p.x, xs[1]); }
}
```

```text
59
```

```aether @reject=SCOPE-001 id=G.Assignment.2
fn main() -> Void {
    y = 2;
}
```

```aether @ok id=G.AssignOp.1
fn main() -> Void {
    let n: Int = 3;
    n += 2;
    n -= 1;
    n *= 3;
    n /= 2;
    n %= 4;
    fx { println(n); }
}
```

```text
2
```

```aether @reject=SYN-001 id=G.AssignOp.2
fn main() -> Void {
    let n: Int = 3;
    n |= 1;
}
```

```aether @reject=SYN-001 id=G.AssignOp.3
fn main() -> Void {
    let n: Int = 3;
    n++;
}
```

```aether @ok id=G.IfStmt.1
fn main() -> Void {
    let x: Int = 0;
    if x > 0 { fx { println("pos"); } } else if x < 0 { fx { println("neg"); } } else { fx { println("zero"); } }
}
```

```text
zero
```

```aether @reject=SYN-001 id=G.IfStmt.2
fn main() -> Void {
    let x: Int = 1;
    if x > 0 { x = 2; } elif x < 0 { x = 3; }
}
```

```aether @reject=SYN-001 id=G.IfStmt.3
fn main() -> Void {
    if { }
}
```

```aether @ok id=G.LoopStmt.1
fn main() -> Void {
    let xs: Int[] = [3, 4];
    let n: Int = 0;
    loop n < 2 { n = n + 1; }
    loop x in xs { fx { print(x); } }
    loop c in "ab" { fx { print(c); } }
    fx { println(" ", n); }
}
```

```text
34ab 2
```

```aether @reject=SYN-001 id=G.LoopStmt.2
fn main() -> Void {
    let n: Int = 3;
    loop x in n { }
}
```

```aether @reject=SYN-001 id=G.LoopStmt.3
fn main() -> Void {
    foreach x in [1] { }
}
```

```aether @ok id=G.Range.1
fn main() -> Void {
    loop i in 0..3 { fx { print(i); } }
    loop i in 0..10 step 4 { fx { print(" ", i); } }
    fx { println(""); }
}
```

```text
012 0 4 8
```

```aether @reject=SYN-001 id=G.Range.2
fn main() -> Void {
    loop i in 0..5 step 0 { }
}
```

Decision D7 accepts `a..=b` as the inclusive range; today it is rejected:

```aether @bug=D7 now=SYN-001 id=G.Range.3
fn main() -> Void {
    loop i in 0..=2 { fx { print(i); } }
}
```

```aether @ok id=G.WhileStmt.1
fn main() -> Void {
    let i: Int = 0;
    while i < 3 { i = i + 1; }
    fx { println(i); }
}
```

```text
3
```

```aether @reject=SYN-001 id=G.WhileStmt.2
fn main() -> Void {
    while true
}
```

```aether @ok id=G.ForStmt.1
fn main() -> Void {
    let s: Int = 0;
    for j in 0..4 { s = s + j; }
    fx { println(s); }
}
```

```text
6
```

```aether @reject=SYN-001 id=G.ForStmt.2
fn main() -> Void {
    for (let i: Int = 0; i < 3; i = i + 1) { }
}
```

```aether @ok id=G.FxBlock.1
fn say(s: Text) -> Void {
    fx { println("said ", s); }
}
fn main() -> Void { say("hi"); }
```

```text
said hi
```

```aether @reject=FX-001 id=G.FxBlock.2
fn main() -> Void {
    println("hi");
}
```

```aether @reject=FX-001 id=G.FxBlock.3
fn main() -> Void {
    fx println("hi");
}
```

```aether @ok id=G.ParBlock.1
type Acc { n: Int = 0; }
fn work(a: Acc, k: Int) -> Void { a.n = k * 2; }
fn main() -> Void {
    let a: Acc = new Acc();
    let b: Acc = new Acc();
    par { work(a, 1); work(b, 2); }
    fx { println(a.n + b.n); }
}
```

```text
6
```

```aether @reject=PAR-002 id=G.ParBlock.2
fn main() -> Void {
    let x: Int = 0;
    par { x = 1; }
}
```

```aether @reject=PAR-001 id=G.ParBlock.3
type Acc { n: Int = 0; }
fn work(a: Acc, k: Int) -> Void { a.n = k; }
fn main() -> Void {
    let a: Acc = new Acc();
    par { work(a, 1); work(a, 2); }
}
```

```aether @ok id=G.RetStmt.1
fn sign(n: Int) -> Text {
    if n < 0 { ret "neg"; }
    ret "nonneg";
}
fn minmax(a: Int, b: Int) -> (Int, Int) { ret (min(a, b), max(a, b)); }
fn main() -> Void {
    let (lo, hi) = minmax(5, 2);
    fx { println(sign(-1), " ", lo, hi); }
}
```

```text
neg 25
```

```aether @reject=FLOW-002 id=G.RetStmt.2
fn f(n: Int) -> Int {
    ret;
}
fn main() -> Void { }
```

```aether @reject=SYN-001 id=G.RetStmt.3
fn f() -> Int { return 1; }
fn main() -> Void { }
```

```aether @ok id=G.ExprStmt.1
type C { n: Int = 0; fn inc(self) -> Void { self.n = self.n + 1; } }
fn main() -> Void {
    let c: C = new C();
    c.inc();
    fx { println(c.n); }
}
```

```text
1
```

```aether @reject=SYN-001 id=G.ExprStmt.2
fn main() -> Void {
    let x: Int = 1;
    x + ;
}
```

### 2.4 Expressions

```ebnf
Expr       = IfExpr | OrExpr ;
IfExpr     = "if" Expr "{" Expr "}" "else" ( IfExpr | "{" Expr "}" ) ;
OrExpr     = AndExpr { ( "or" | "||" ) AndExpr } ;
AndExpr    = BitOrExpr { ( "and" | "&&" ) BitOrExpr } ;
BitOrExpr  = BitXorExpr { "|" BitXorExpr } ;
BitXorExpr = BitAndExpr { ( "^" | "xor" ) BitAndExpr } ;
BitAndExpr = EqExpr { "&" EqExpr } ;
EqExpr     = RelExpr [ ( "==" | "!=" ) RelExpr ] ;
RelExpr    = ShiftExpr [ ( "<" | "<=" | ">" | ">=" ) ShiftExpr ] ;
ShiftExpr  = AddExpr { ( "<<" | ">>" ) AddExpr } ;
AddExpr    = MulExpr { ( "+" | "-" ) MulExpr } ;
MulExpr    = Unary { ( "*" | "/" | "div" | "%" | "mod" ) Unary } ;
Unary      = ( "-" | "!" | "not" ) Unary | Postfix ;
Postfix    = Primary { "." Ident [ "(" [ Args ] ")" ] | "." Digits
           | "[" Expr [ ".." Expr ] "]" } ;
Primary    = Literal | Ident | Call | "(" Expr ")" | ArrayLit | RecordLit
           | NewExpr | "self" | "nil" ;
Call       = Ident "(" [ Args ] ")" ;
Args       = Arg { "," Arg } ;
Arg        = Expr [ ":" Expr [ ":" Expr ] ] ;
ArrayLit   = "[" [ Expr { "," Expr } ] "]" ;
RecordLit  = Ident "{" [ Ident ":" Expr { "," Ident ":" Expr } ] "}" ;
NewExpr    = "new" Ident ( "(" ")" | "{" [ Ident ":" Expr { "," Ident ":" Expr } ] "}" ) ;
TupleLit   = "(" Expr "," Expr { "," Expr } ")" ;
Literal    = Int | Real | Text | "true" | "false" ;
```

Binary operators of one level associate to the left. As in C, `&`, `|` and `^`
bind looser than the comparisons (PREC-001 warns on `a & b != 0`). The word
operators are exact synonyms of their symbols, `not` included: `not` binds as
tightly as `!`. `div` and `%`/`mod` are integer quotient and remainder and
truncate toward zero. Comparisons do not chain (decision D32), and
`not X == Y` without parentheses is rejected by the same decision; both are
accepted today with C meaning, which is the silent hazard D32 removes.

```aether @ok id=G.Expr.1
fn main() -> Void {
    let a: Int = 2;
    fx { println(a * 3 + 1, " ", (a + 1) * 3, " ", a > 1 and a < 3); }
}
```

```text
7 9 true
```

```aether @reject=SYN-001 id=G.Expr.2
fn main() -> Void {
    let f: Int = x => x * 2;
}
```

```aether @ok id=G.IfExpr.1
fn main() -> Void {
    let x: Int = 7;
    let g: Text = if x > 8 { "A" } else if x > 6 { "B" } else { "C" };
    fx { println(g); }
}
```

```text
B
```

```aether @reject=SYN-001 id=G.IfExpr.2
fn main() -> Void {
    let x: Int = 1;
    let y: Int = if x > 0 { 1 };
}
```

```aether @ok id=G.OrExpr.1
fn main() -> Void {
    let b: Bool = false;
    fx { println(b or true, " ", b || b); }
}
```

```text
true false
```

```aether @reject=SYN-001 id=G.OrExpr.2
fn main() -> Void {
    let b: Bool = true or ;
}
```

```aether @ok id=G.AndExpr.1
fn main() -> Void {
    let b: Bool = true;
    fx { println(b and false, " ", b && b); }
}
```

```text
false true
```

```aether @reject=SYN-001 id=G.AndExpr.2
fn main() -> Void {
    let b: Bool = true and ;
}
```

```aether @ok id=G.BitOrExpr.1
fn main() -> Void {
    fx { println(6 | 1, " ", (6 & 3) | 8); }
}
```

```text
7 10
```

```aether @reject=SYN-001 id=G.BitOrExpr.2
fn main() -> Void {
    fx { println(6 | ); }
}
```

```aether @ok id=G.BitXorExpr.1
fn main() -> Void {
    fx { println(6 ^ 3, " ", 6 xor 3); }
}
```

```text
5 5
```

```aether @reject=SYN-001 id=G.BitXorExpr.2
fn main() -> Void {
    fx { println(6 ^ ); }
}
```

```aether @ok id=G.BitAndExpr.1
fn main() -> Void {
    let flags: Int = 6;
    fx { println(flags & 2, " ", (flags & 1) != 0); }
}
```

```text
2 false
```

```aether @reject=SYN-001 id=G.BitAndExpr.2
fn main() -> Void {
    fx { println(6 & ); }
}
```

```aether @ok id=G.EqExpr.1
fn main() -> Void {
    let xs: Int[] = [1, 2];
    let ys: Int[] = [1, 2];
    fx { println(1 == 1, " ", "a" != "b", " ", xs == ys); }
}
```

```text
true true true
```

```aether @reject=SYN-001 id=G.EqExpr.2
fn main() -> Void {
    let a: Int = 1;
    fx { println(a === 1); }
}
```

```aether @bug=D32 now=ok id=G.EqExpr.3
fn main() -> Void {
    let a: Int = 1;
    fx { println(not a == 2); }
}
```

```text
false
```

```aether @ok id=G.RelExpr.1
fn main() -> Void {
    fx { println(1 < 2, " ", 2 <= 2, " ", "a" < "b", " ", 3 >= 4); }
}
```

```text
true true true false
```

```aether @reject=SYN-001 id=G.RelExpr.2
fn main() -> Void {
    let a: Int = 1;
    fx { println(a =< 1); }
}
```

```aether @bug=D32 now=ok id=G.RelExpr.3
fn main() -> Void {
    let i: Int = 5;
    fx { println(0 <= i < 3); }
}
```

```text
true
```

```aether @ok id=G.ShiftExpr.1
fn main() -> Void {
    fx { println(1 << 3, " ", 16 >> 2); }
}
```

```text
8 4
```

```aether @reject=SYN-001 id=G.ShiftExpr.2
fn main() -> Void {
    fx { println(1 << ); }
}
```

```aether @ok id=G.AddExpr.1
fn main() -> Void {
    let r: Real = 1 + 2.5;
    let s: Text = "a" + "b";
    fx { println(5 - 7, " ", r, " ", s); }
}
```

```text
-2 3.500000 ab
```

```aether @reject=SYN-001 id=G.AddExpr.2
fn main() -> Void {
    let x: Int = 1 + ;
}
```

```aether @ok id=G.MulExpr.1
fn main() -> Void {
    fx { println(7 div 2, " ", -7 div 2, " ", 7 % 3, " ", -7 mod 3, " ", 3 * 1.5); }
}
```

```text
3 -3 1 -1 4.500000
```

```aether @reject=SYN-001 id=G.MulExpr.2
fn main() -> Void {
    let x: Int = 2 * ;
}
```

Int / Int is decision D4, still being measured; today it yields a Real:

```aether @bug=D4 now=ok id=G.MulExpr.3
fn main() -> Void {
    fx { println(7 / 2); }
}
```

```text
3.500000
```

```aether @ok id=G.Unary.1
fn main() -> Void {
    let n: Int = 3;
    fx { println(-n, " ", !true, " ", not false); }
}
```

```text
-3 false true
```

```aether @reject=SYN-001 id=G.Unary.2
fn main() -> Void {
    let x: Int = 1;
    let y: Int = ++x;
}
```

```aether @ok id=G.Postfix.1
type P { x: Int; }
fn main() -> Void {
    let xs: Int[] = [5, 6, 7];
    let s: Text = "hello";
    let ps: P[] = [new P { x: 1 }, new P { x: 2 }];
    fx { println(xs[0], " ", length(xs[1..3]), " ", s[1..3], " ", s[0], " ", ps[1].x); }
}
```

```text
5 2 el h 2
```

```aether @reject=TUP-001 id=G.Postfix.2
fn pair() -> (Int, Int) { ret (1, 2); }
fn main() -> Void {
    fx { println(pair().0); }
}
```

```aether @reject=FIELD-002 id=G.Postfix.3
type P { x: Int; }
fn main() -> Void {
    let p: P = new P { x: 1 };
    fx { println(p.z); }
}
```

```aether @ok id=G.Primary.1
type P { x: Int = 0; }
fn main() -> Void {
    let p: P = nil;
    fx { println((2 + 3) * 2, " ", p == nil, " ", true); }
}
```

```text
10 true true
```

```aether @reject=SYN-001 id=G.Primary.2
fn main() -> Void {
    let x: Int = ;
}
```

```aether @ok id=G.Call.1
fn sq(n: Int) -> Int { ret n * n; }
fn main() -> Void {
    fx { println(sq(3), " ", max(2, 9), " ", int_to_text(7), " ", parse_int("41") + 1); }
}
```

```text
9 9 7 42
```

```aether @reject=SCOPE-001 id=G.Call.2
fn main() -> Void {
    fx { println(frob(1)); }
}
```

```aether @reject=BUILT-002 id=G.Call.3
fn main() -> Void {
    fx { println(formatfloat(1.5, 0, 2)); }
}
```

```aether @ok id=G.Args.1
fn three(a: Int, b: Int, c: Int) -> Int { ret a * 100 + b * 10 + c; }
fn main() -> Void { fx { println(three(1, 2, 3)); } }
```

```text
123
```

```aether @reject=SYN-001 id=G.Args.2
fn main() -> Void {
    fx { println(1,,2); }
}
```

An argument's `:width` and `:width:precision` suffixes are allowed only in
`print`/`println`; they right-align in at least `width` columns and give a Real
`precision` decimals. Decision D33 makes the width a minimum for every type and
prints Bool in lower case; today a Text wider than its width is cut, and a Bool
with a width prints in capitals.

```aether @ok id=G.Arg.1
fn main() -> Void {
    fx { println(7:4, "|", 3.14159:0:2, "|", "ab":5, "|"); }
}
```

```text
   7|3.14|   ab|
```

```aether @reject=SYN-001 id=G.Arg.2
fn main() -> Void {
    let s: Text = 3.14159:0:2;
}
```

```aether @bug=D33 now=ok id=G.Arg.3
fn main() -> Void {
    fx { println("abcdef":3, "|", true:6, "|"); }
}
```

```text
abc|  TRUE|
```

```aether @ok id=G.ArrayLit.1
fn main() -> Void {
    let xs: Int[] = [];
    let ys: Int[] = [1, 2, 3];
    xs = xs + [4];
    fx { println(length(xs), length(ys)); }
}
```

```text
13
```

```aether @reject=SYN-001 id=G.ArrayLit.2
fn main() -> Void {
    let xs: Int[] = [1, 2;
}
```

Decision D7 accepts `[v; n]` (n copies of v); today it is rejected:

```aether @bug=D7 now=SYN-001 id=G.ArrayLit.3
fn main() -> Void {
    let xs: Int[] = [0; 3];
}
```

```aether @ok id=G.RecordLit.1
type P { x: Int; y: Int; }
fn sum(p: P) -> Int { ret p.x + p.y; }
fn main() -> Void {
    let q: P = P { x: 3, y: 4 };
    fx { println(sum(P { x: 1, y: 2 }), " ", q.y); }
}
```

```text
3 4
```

```aether @reject=FIELD-002 id=G.RecordLit.2
type P { x: Int; }
fn main() -> Void {
    let p: P = P { z: 1 };
}
```

```aether @ok id=G.NewExpr.1
type P { x: Int = 0; y: Int = 0; }
fn main() -> Void {
    let a: P = new P { x: 1, y: 2 };
    let b: P = new P();
    b.y = 5;
    fx { println(a.x, a.y, b.x, b.y); }
}
```

```text
1205
```

```aether @reject=SYN-001 id=G.NewExpr.2
type P { x: Int; }
fn main() -> Void {
    let p: P = new P(x: 1);
}
```

```aether @ok id=G.TupleLit.1
fn split(n: Int) -> (Int, Int) { ret (n div 10, n % 10); }
fn main() -> Void {
    let (tens, ones) = split(42);
    fx { println(tens, " ", ones); }
}
```

```text
4 2
```

```aether @reject=SYN-001 id=G.TupleLit.2
fn f() -> Int { ret (1, 2); }
fn main() -> Void { }
```

```aether @ok id=G.Literal.1
fn main() -> Void {
    fx { println(42, " ", 1.5, " ", "t", " ", false); }
}
```

```text
42 1.500000 t false
```

```aether @reject=SYN-001 id=G.Literal.2
fn main() -> Void {
    fx { println(0b11); }
}
```

---

## 3. Static semantics (stub)

To be written: the type rules, inference, scope and name resolution, effects
(`fx`, `@pure`) and contracts. Open rows that decide parts of it:

- **D4**, Int / Int: pending its probe (G.MulExpr.3 pins today's Real result).
- **D15**, homogeneous array-literal inference: accepted, lands in L2
  (G.LetDecl.2 is today's TYPE-001).
- **D17**, case-insensitive collisions become a coded error (L.ident.4).
- **D12**, `fx` stays an error unless the baseline shows FX-001 is at least 5% of
  first-attempt failures.
- **D5**, every diagnostic carries a code; **D23**, `length`/`toon_len`
  dispatch on the operand type.
- **D7**, the surface synonym classes; the registry in `tests/surface/` (W8-11)
  lists every accepted, rejected and tolerated spelling.

Text + number stringifies left to right with an annotated type, and decision D7
keeps it; the inferred form is still TYPE-001 today.

```aether @ok id=S.textplus.1
fn main() -> Void {
    let n: Int = 3;
    let s: Text = "n=" + n;
    fx { println(s); }
}
```

```text
n=3
```

```aether @ok id=S.textplus.3
fn main() -> Void {
    let s: Text = 1 + 2 + "a" + 1 + 2;
    fx { println(s); }
}
```

```text
3a12
```

```aether @bug=D7 now=TYPE-001 id=S.textplus.2
fn main() -> Void {
    let n: Int = 3;
    let s = "n=" + n;
}
```

A `Real` stored into an `Int` truncates, with the NARROW-001 warning (never an
error, by decision):

```aether @ok id=S.narrow.1 warn=NARROW-001
fn main() -> Void {
    let n: Int = 3.7;
    fx { println(n); }
}
```

```text
3
```

## 4. Dynamic semantics (stub)

To be written: evaluation order, the value model, integers, Text, printing and
run-time errors. Open rows:

- **D8**, array semantics (value copies today; V-strict vs by-ref pending).
- **D18**, range loops; **D19**, 64-bit integers (settled in L0).
- **D24**, record and tuple `==`: REC-001 from W4. Today `==` on two records
  compares identity.
- **D14**, `println(array)` and `int(Text)` become coded errors. Today both are
  silent.
- **D33**, print widths and Bool spelling; **D46**, `println` puts nothing
  between its arguments.

```aether @trap=ARR-003 id=D.index.1
fn main() -> Void {
    let xs: Int[] = [1];
    fx { println(xs[3]); }
}
```

```aether @bug=D24 now=ok id=D.receq.1
type P { x: Int; }
fn main() -> Void {
    let a: P = new P { x: 1 };
    let b: P = new P { x: 1 };
    fx { println(a == b); }
}
```

```text
false
```

```aether @bug=D14 now=ok id=D.inttext.1
fn main() -> Void {
    fx { println(int("42")); }
}
```

```text
0
```

`x:.2` is accepted by D7 with its exact meaning (two decimals); today it prints
in exponent form:

```aether @bug=D7 now=ok id=D.colonfmt.1
fn main() -> Void {
    let r: Real = 3.14159;
    fx { println(r:.2); }
}
```

```text
3.141590E+00
```

`exit(n)` ends the program with status `n`, from inside an `fx` block; a `main`
declared `-> Int` sets the status with its `ret`:

```aether @ok id=D.exit.1 rc=3
fn main() -> Void {
    fx { println("a"); exit(3); println("b"); }
}
```

```text
a
```

```aether @ok id=D.exit.2 rc=4
fn main() -> Int {
    fx { println("a"); }
    ret 4;
}
```

```text
a
```

## 5. Program structure and modules (stub)

To be written: module files and name visibility. Rows:

- **D16**, entry point (shipped): ENTRY-001 for an empty file, a file with
  neither `fn main` nor a top-level statement, a `fn main` beside top-level
  statements that never call it, and a `fn main` that takes parameters or
  returns something other than `Void` or `Int`. Script mode (top-level
  statements, no `fn main`) stays legal, and so does calling `main();` from
  the top level.
- **D25**, imports: IMP-001 when a module is missing. Today a missing module is
  ignored.

```aether @reject=ENTRY-001 id=P.entry.1
fn helper() -> Int { ret 1; }
```

```aether @reject=ENTRY-001 id=P.entry.2
fx { println("top"); }
fn main() -> Void { fx { println("main"); } }
```

```aether @ok id=P.entry.3
fx { println("top"); }
fn main() -> Void { fx { println("main"); } }
main();
```

```text
top
main
```

A qualified call to a name the module does not export fails without a code
today (decision D5 forbids uncoded diagnostics); the unqualified call is
SCOPE-001:

```aether @bug=D5 now=uncoded id=P.import.2
use "geometry";
fn main() -> Void { fx { println(Geometry.volume(2, 3)); } }
```

```aether @reject=SCOPE-001 id=P.import.3
use "geometry";
fn main() -> Void { fx { println(volume(2, 3)); } }
```

```aether @bug=D25 now=ok id=P.import.1
use "no_such_module";
fn main() -> Void { fx { println("ran"); } }
```

```text
ran
```

## 6. Leniencies

D30 makes the spec normative "with enumerated leniencies": forms the compiler
accepts with an exact meaning that the grammar above does not teach. Each is
listed here with its meaning; anything accepted and not in §2 or this list is a
bug. The surface registry (W8-11) will assign each one its synonym class.

| Leniency | Meaning |
|---|---|
| a missing `;` at the end of a line | as if the `;` were there |
| unparenthesised or parenthesised conditions, unbraced `if` bodies | `if (c) stmt` is `if c { stmt }` |
| `Float`, `String` | `Real`, `Text` |
| `let mut x` | `let x` (every `let` is mutable) |
| trailing comma in a parameter or argument list | ignored |
| `c ? a : b` | `if c { a } else { b }` |
| `T(f: v, ...)` as a `let` initializer | `new T { f: v, ... }` |
| `use name;` without quotes | `use "name";` |
| `xs.len`, `xs.len()`, `xs.length()` | `length(xs)` |
| `itoa(n)`, `realtostr(r)` | `int_to_text(n)`, six-decimal Real text |

```aether @ok id=LEN.semicolon.1
fn main() -> Void {
    let x: Int = 1
    fx { println(x) }
}
```

```text
1
```

```aether @ok id=LEN.ifparen.1
fn main() -> Void {
    let x: Int = 1;
    if (x > 0) fx { println("p"); }
}
```

```text
p
```

```aether @ok id=LEN.alias.1
fn main() -> Void {
    let r: Float = 1.5;
    let s: String = "x";
    let mut n: Int = 2;
    fx { println(r, s, n); }
}
```

```text
1.500000x2
```

```aether @ok id=LEN.trailing.1
fn add(a: Int, b: Int,) -> Int { ret a + b; }
fn main() -> Void { fx { println(add(1, 2,)); } }
```

```text
3
```

```aether @ok id=LEN.ternary.1
fn main() -> Void {
    let a: Int = 1;
    let b: Int = a > 0 ? 10 : 20;
    fx { println(b); }
}
```

```text
10
```

```aether @ok id=LEN.parenrec.1
type P { x: Int; y: Int; }
fn main() -> Void {
    let p: P = P(x: 5, y: 6);
    fx { println(p.x + p.y); }
}
```

```text
11
```

```aether @ok id=LEN.usebare.1
use geometry;
fn main() -> Void { fx { println(Geometry.perimeter(2, 3)); } }
```

```text
10
```

```aether @ok id=LEN.len.1
fn main() -> Void {
    let xs: Int[] = [1, 2];
    let s: Text = "abc";
    fx { println(xs.len, xs.len(), xs.length(), s.len, " ", itoa(8), " ", realtostr(1.5)); }
}
```

```text
2223 8 1.500000
```

---

## Changing this spec

A spec edit that changes an `@ok` stdout, turns an `@ok` into a `@reject` or the
reverse, or removes a leniency is a language change: it lands with the compiler
change, a `VERSION` bump and a `CHANGELOG.md` entry, and the CHANGELOG entry
names the example ids. When a `@bug` example flips, its fix lands the same way and
the example becomes `@ok` or `@reject`. Adding examples that pin behaviour the
compiler already has is not a language change.
