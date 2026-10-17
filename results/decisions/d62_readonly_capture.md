# D62 (nested functions): read-only capture without a copy

Base: aether 2026-10-17-5 (this release), Release build, one machine, best of
three wall-clock runs per program (`run.sh` below). The question D62 left open:
a nested fn reads enclosing arrays through extra parameters of its lifted
function; does that cost a copy per call, as a declared array parameter does?

## How a captured array is passed

A declared array parameter gets a prologue copy (`setlength(p, length(p))`,
parseFnDecl) so writes inside the callee never reach the caller. A capture
parameter is added after parsing and gets no prologue: the VM passes an array
value by sharing it, and FUNC-002 guarantees the nested fn never writes it. It
is deliberately **not** `by_ref`: a whole-value read of a by-reference
parameter of a literal-built array (`let c = xs;`, `ret xs;`) fails its
un-alias step with "SetLength expects a string or dynamic array target", the
same failure the D8 `arrays=ref` arm needed shim 3 for
(`arrays_2026-10-07.md`). A first cut used `by_ref` and hit exactly that;
`tests/fx/lower/nested_fn_shared_copy_pass` pins the fix.

## Results

| program | what | nested (capture) | top-level (copied parameter) |
|---|---|---|---|
| `qn_nested` / `qn_top` | N-Queens n=8, three runs, a 20,000-element read-only `weights` array read on every `safe` call | 0.70 s | 30.17 s |
| `qn_nested_small` / `qn_top_small` | the same with an 8-element `weights` | 0.18 s | 0.22 s |
| `depth_nested` / `depth_top` | linear recursion reading a captured / passed 3-element array, depth 100,000 | 100001 | 100001 |
| the same | depth 1,000,000 | VM call stack overflow | VM call stack overflow |

Both N-Queens versions print 276 (92 solutions, three runs). The copy cost of
the top-level version is the prologue copy of `weights` on each of the ~16,000
calls; the nested version reads the host's array. Recursion depth is the VM's
call-stack limit either way: lifting adds parameters, not frames.

## Programs

`qn_nested.aether` (`qn_nested_small`: `0..8` in place of `0..20000`):

```aether
fn queens(n: Int, weights: Int[]) -> Int {
    fn safe(row: Int, col: Int, board: Int[]) -> Bool {
        loop r in 0..row {
            let c = board[r];
            if c == col || abs(c - col) == row - r { ret false; }
        }
        ret weights[col] >= 0;
    }
    fn place(row: Int, board: Int[]) -> Int {
        if row == n { ret 1; }
        let found = 0;
        loop col in 0..n {
            if safe(row, col, board) {
                let next = board;
                next[row] = col;
                found = found + place(row + 1, next);
            }
        }
        ret found;
    }
    let start: Int[] = [];
    loop i in 0..n { start = start + [0]; }
    ret place(0, start);
}
fn main() -> Void {
    let w: Int[] = [];
    loop i in 0..20000 { w = w + [i]; }
    let total = 0;
    loop rep in 0..3 { total = total + queens(8, w); }
    fx { println(total); }
}
```

`qn_top.aether`:

```aether
fn safe(row: Int, col: Int, board: Int[], weights: Int[]) -> Bool {
    loop r in 0..row {
        let c = board[r];
        if c == col || abs(c - col) == row - r { ret false; }
    }
    ret weights[col] >= 0;
}
fn place(row: Int, board: Int[], n: Int, weights: Int[]) -> Int {
    if row == n { ret 1; }
    let found = 0;
    loop col in 0..n {
        if safe(row, col, board, weights) {
            let next = board;
            next[row] = col;
            found = found + place(row + 1, next, n, weights);
        }
    }
    ret found;
}
fn queens(n: Int, weights: Int[]) -> Int {
    let start: Int[] = [];
    loop i in 0..n { start = start + [0]; }
    ret place(0, start, n, weights);
}
fn main() -> Void {
    let w: Int[] = [];
    loop i in 0..20000 { w = w + [i]; }
    let total = 0;
    loop rep in 0..3 { total = total + queens(8, w); }
    fx { println(total); }
}
```

`depth_nested.aether` / `depth_top.aether` (depth is the first argument):

```aether
fn run(n: Int) -> Int {
    let pad: Int[] = [1, 2, 3];
    fn go(k: Int) -> Int {
        if k == 0 { ret pad[0]; }
        ret 1 + go(k - 1);
    }
    ret go(n);
}
fn main() -> Void {
    fx { println(run(parse_int(args()[0]))); }
}
```

```aether
fn go(k: Int, pad: Int[]) -> Int {
    if k == 0 { ret pad[0]; }
    ret 1 + go(k - 1, pad);
}
fn main() -> Void {
    let pad: Int[] = [1, 2, 3];
    fx { println(go(parse_int(args()[0]), pad)); }
}
```

`run.sh BINARY` runs each N-Queens program three times with `--no-cache`,
keeps the fastest, then the two depths.
