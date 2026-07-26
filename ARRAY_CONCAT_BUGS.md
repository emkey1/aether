# Working note: two array-concat front-end bugs (found 2026-07-25)

> ## ✅ BOTH LANDED — 2026-07-24, commit `8ebf8e7`
>
> Bug A and Bug B are both fixed, tested, and deployed. Regression fixtures:
> `tests/array_concat_return_pass.aether`,
> `tests/array_slice_concat_operand_pass.aether`, both wired into
> `tests/run.sh`. Verified against all 276 corpus candidates (baseline vs
> patched): zero semantic differences.
>
> **The open question below is resolved.** `buildLengthCall` does
> `copyAST(target)` internally (`ast_parser.c:3967`), and the shared contract
> is documented at 3986-3987: **`target` is only ever read, never owned.** The
> `copyAST` at 4463 is therefore consistent, not a hint of transferred
> ownership, and the decl path passing `var` (already a child of `decl`) is
> correct — no latent double-free. The new `ret` call site likewise passes a
> node already owned by the tree.
>
> **Bug A's real cause was not what this note guessed** (see the Bug A section
> below, which is superseded). It is nothing to do with slices as initializers.
> `parseStatement`'s hoist wrapper flattened the *hoists* but added the
> statement itself as a child; since `parseBlock` splices only ONE level, a
> let-position splice group nested under that wrapper survived as a real block
> and scoped the variable away. Fix: flatten the statement too. Both bugs did
> share a cause in the sense the note hoped — "array `+` lowers to statements
> that need a statement position" — but the *mechanisms* differed.
>
> Also fixed in passing: `buildArraySlice` leaked the `copyAST(base)` it fed to
> `inferLetTypeName` (which only reads its argument).
>
> Shapes fixed beyond the two reported: `ret a + [x];` and `ret a + [];` (the
> append shapes, which failed identically), and the `@post` variant, where the
> lowering is staged into `result` ahead of the guard.
>
> **Still open (deliberately deferred, see Sequencing):** the 0-based `Text`
> change, and the corpus simplifications that depend on these fixes. Nothing
> in the corpus was touched by this commit — the fixes only *widen* what
> compiles, so no corpus pass / dataset rebuild / retrain was required.
>
> **Known pre-existing, NOT fixed here:** chained concat `a + b + c` fails in
> *both* the decl and `ret` paths ("Got ARRAY and ARRAY") — the lowering does
> not recurse into a left operand that is itself an array `+`. Separate bug,
> separate fix.
>
> Delete this note once the 0-based `Text` batch below is also done.

Untracked scratch note for whoever picks this up.
Both reproduce on this repo at `04a8863` / `dcf0997`.

## The two bugs

**Bug B — array concat in a `ret` (runtime).**
```aether
fn f() -> Int[] {
    let a: Int[] = [1, 2];
    let b: Int[] = [3, 4];
    ret a + b;          // Runtime Error: ... Got ARRAY and ARRAY
}
```
Works if bound first: `let c: Int[] = a + b; ret c;`

**Bug A — slice expression as a direct `+` operand (compile time).**
```aether
let r: Int[] = a[1..3] + b;   // [SCOPE-001] identifier 'r' not in scope
```
Works if the slice is bound first: `let s: Int[] = a[1..3]; let r: Int[] = s + b;`
Note the error points at the *later use site*, not the failed declaration —
worth fixing that misdirection too if cheap.

## Root cause (confirmed)

Array `+` is **not** a VM operation. The front end rewrites it into spliced
statements — `setlength` + an indexed element-copy loop — via
`buildArrayConcatSteps` (definition at `src/aether/ast_parser.c:4427`).

Those spliced steps need a **statement position** to live in. A `let`
declaration provides one (`ast_parser.c:3888-3900` builds an `AST_COMPOUND`
with `i_val = 1` and appends the steps after the decl). A bare `ret` is an
**expression** position with nowhere to splice, so the raw `+` survives to
the VM and dies as "Got ARRAY and ARRAY".

Ruled out by experiment — do **not** re-tread these:
- NOT about slices (plain array vars reproduce Bug B).
- NOT about function parameters (locals reproduce it).
- `let c = a + b;` works everywhere tested: top level, inside `fx`, inside
  plain functions.

## Fix design for Bug B

Desugar in `parseRet` (`ast_parser.c:4955`), mirroring the existing
`ret T { ... }` object-init precedent (`buildReturnObjectInit`, template at
`ast_parser.c:4575-4619`, dispatched from `parseRet` at ~4990):

```
ret src + other;
  =>
{                                    // AST_COMPOUND, i_val = 1
    let __aether_ret_concat_<line>: T[] = src;
    <buildArrayConcatSteps appends setlength + copy loop for `other`>
    return __aether_ret_concat_<line>;
}
```

Insertion point: after `if (p->hadError) return NULL;` (~line 5011), before
the `@post` block at 5015. Guard on `!p->currentPostExpr` and fall through
otherwise — the object-init case at 4977 already uses that same guard, and
the `@post` path stages `result` itself.

Detection condition — copy it from the declaration path at
`ast_parser.c:3768-3772`, which is known-good:
```c
value->type == AST_BINARY_OP && value->token &&
value->token->type == TOKEN_PLUS &&
value->right && value->right->type != AST_ARRAY_LITERAL &&
/* then: */ inferLetTypeName(p, value->right) is array per aetherTypeNameIsArray()
```

### ⚠ OPEN QUESTION — resolve before writing the patch

`buildArrayConcatSteps`'s ownership contract for its `target` argument is
ambiguous, and getting it wrong means a double-free in the compiler:

- line 4456: `setLeft(baseDecl, buildLengthCall(target, line));`
- line 4463: `addChild(setlen, copyAST(target));`

The explicit `copyAST` at 4463 implies `buildLengthCall` at 4456 **takes
ownership** of `target`. But the existing decl-path call site (line 3895)
passes `var`, which is *already a child of `decl`* via `addChild(decl, var)`
— which would be double ownership. So either `buildLengthCall` copies
internally (and the 4463 `copyAST` is redundant), or there's a latent bug.

**Read `buildLengthCall` first and settle this.** Then decide whether the new
call site passes a fresh `buildVarRef(tmpName, ...)` or a shared node.

## Bug A

Less diagnosed. The detection at 3768 keys off the **right** operand, so for
`a[1..3] + b` it *does* fire (right operand `b` infers as an array) and `init`
becomes the slice `a[1..3]`. So the concat machinery engages; the failure is
downstream in how the declaration handles a *slice expression* as its
initializer while also carrying the concat splice. `let s = a[1..3];` alone is
fine, so it is specifically the combination. Start there.

Check whether both bugs share a root cause; if so fix together.

## Sequencing

Batch these with the queued **0-based `Text` indexing** change (plus `pos()`
returning `-1` instead of `0` for "not found") so there is ONE compiler
change, ONE corpus pass, ONE dataset rebuild, ONE model retrain — not three.
Measured blast radius for the indexing change: ~70 of 742 corpus files, 6
repo files outside the corpus, 3 guide lines.

## Deploy hazard

**Corrected 2026-07-24 — the chain is one hop longer than described here.**
This repo's post-commit hook (`.git/hooks/post-commit`) pushes to origin and
then runs `/Users/mke/PBuild/tools/sync_aether_canonical_repo.sh`, which bumps
PBuild's `components/aether` gitlink and commits *there*. PBuild's own
post-commit hook sees `components/aether` change and only *then* fires
`tools/deploy_aether_to_claws.sh`, which rebuilds `/home/claw/aether-current`
on claw1/claw2/claw3. Net effect is the same — a commit here reaches the claws
— but nothing named `deploy_aether_to_claws.sh` exists in this repo's `tools/`,
so don't go looking for it here. Disable with `AETHER_CANONICAL_SYNC=0` (stops
at push) or `AETHER_AUTODEPLOY=0` (stops before the claws). **Never deploy while a benchmark is running on the
claws** — it swaps the grading binary mid-run and silently corrupts results.
claw2 is currently down pending GRUB recovery (kernel panic, `VFS: unable to
mount root fs`, needs the older `6.17.0-1021-nvidia` selected at GRUB).

## Follow-up once Bug B lands

`PBuild/Tests/aether_specialization/corpus_candidates/78_rotate_via_slices.aether`
uses the bind-then-return workaround (`let rotated: Int[] = tail + head; ret
rotated;`) purely to dodge Bug B. It can be simplified to `ret tail + head;`.
