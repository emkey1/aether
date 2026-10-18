# Proposal: let a nested function assign its enclosing locals

Status: **accepted** by the owner, 2026-10-18: D64 option (c), assignable capture, and D65.
**Shipped** in language 2026-10-18-1 (route 1, by-reference capture parameters; see CHANGELOG).
The owner decided directly; the pre-registered board rule below is superseded (kept for the record).
Refines D62 (nested functions with read-only capture, shipped in language
2026-10-17-5) and D49. Two draft decision rows: D64 (assignable capture)
and D65 (annotations on a nested fn). D64 carries a decision rule written
before the Release B board reports, so that board can decide it. The evidence is in
[`results/decisions/nested_assign_2026-10-18.md`](../../results/decisions/nested_assign_2026-10-18.md).

## Why

D62 lets a nested fn read the enclosing function's parameters and earlier
locals, and makes assigning one FUNC-002 ("return the new value, or keep it in
a record field"). That rule was chosen before anyone checked what the nested
helpers in the archive actually do. They write state:

- An N-Queens `backtrack` bumps a solution `count` and pushes onto `cols`.
- A Dijkstra `addEdge` appends to `adj[u]`.
- A union-find `find` compresses paths in `parent`.
- A heap `insert` appends to `heap` and bumps `size`.

The models write that the way JavaScript, Go, Swift and Kotlin allow:
assignment straight to the enclosing variable. Python needs `nonlocal` for a
scalar, but not one attempt writes `nonlocal`. Aether's arrays are values, so
even the element write Python allows on a captured list (`cols[r] = c`) is an
assignment of the local here. That makes it FUNC-002 too.

The thesis (rationale §1) is to say what the model expects and lower it to what
the VM has. A nested helper that keeps the caller's state up to date is what the
model expects. Lambda lifting can lower that without a closure runtime, because
a nested fn is not a value and cannot outlive its host's activation.

## Evidence

The census covers every archived attempt that declares a nested fn, graded on
2026-10-17-5. Details, scripts and files are in the evidence document.

| | first attempts | all attempts | distinct sources |
|---|---|---|---|
| declare a nested fn | 19 | 55 | 37 (5 models; medium guide, card, no guide) |
| stop on FUNC-002 today | 9 | 21 | 15 |
| ... of those, pass if the assignments are legal | **7** | **15** | 10 |
| ... of those, run but print a wrong answer | 2 | 6 | 5 |
| stop on SYN-001 at `@pure` above a nested fn | 7 | 19 | 7 |
| ... of those, pass with the annotation accepted | 1 | 3 | 1 |

"If legal" is measured, not estimated. Each assigned local was moved to a
top-level `let`, and its declaration became an assignment. That is the same
program when the host is not re-entered while the nested fn runs, which holds
for every host here. The rewritten program was graded against the task.

- **Every writer assigns an array.** All 15 distinct FUNC-002 sources write an
  enclosing array (element write, append or slice); 7 also bump a scalar. A
  scalars-only relaxation would make none of them legal.
- **The 7 first attempts that would pass come from 3 models and 4 tasks:**
  the fine-tuned qwen3.8-27b on cs_nqueens, cs_dijkstra and
  algo_union_find_components (no guide); devstral-small-2 on hard2_heap_sort
  (medium, three repeats); prism-coder-7b on cs_nqueens (medium).
- **The 2 silent wrong first attempts** are devstral's own N-Queens bug: its
  diagonal test `(i - r) == (c - c)` compares against zero. Today they fail
  with FUNC-002, but the fix that hint asks for keeps the bug, so they fail
  either way. What changes is the class: coded error becomes silent wrong,
  the class the benchmark ranks worst.
- **No model in the archive has seen FUNC-002.** All 21 attempts predate the
  nested-fn hint and failed with `[SYN-001] expected ')' to close argument
  list`. Under that message, devstral kept the nested helper through all three
  attempts in all five of its cases. The tuned model resubmitted identical
  N-Queens source three times, and on Dijkstra and union-find dropped the
  helper on attempt 2 and passed. How models repair FUNC-002 is unmeasured.
  The Release B board started on 2026-10-17 and has not reported yet; it is
  the first measurement.

## Options

| | option | writers made legal (of 15) | cost |
|---|---|---|---|
| a | keep D62 read-only (FUNC-002 stands) | 0 | none; the repair needs a record type, or a top-level `let` |
| b | allow scalars only | 0 | small; every writer also assigns an array |
| c | **assignable capture**: assignments, element writes and appends change the enclosing variable | 15 | a lowering change (below); FUNC-002 retires |
| d | explicit `nonlocal x;` before assigning | 15, if models wrote it | a keyword no attempt wrote |
| e | (a), with a FUNC-002 hint naming the fix the evidence used: make the local a top-level `let` | 0 now; a cheaper repair | a hint change; teaches globals as the idiom |

Recommendation: **(c)**, decided by the rule in D64. Option (e) is the fallback
if the board shows FUNC-002 repairs well. It is a strict improvement on today's
hint whatever the decision on (c).

## What (c) means

- A nested fn may assign any enclosing parameter or local it can see (D62's
  visibility rules), write its elements and append to it. The enclosing
  variable changes: it is shared, not copied, exactly as with `nonlocal` in
  Python or a closure in JavaScript. A read sees the current value.
- Everything else in D62 stands: a nested fn is not a value (FUNC-001), nesting
  is one level deep, only free top-level functions host them, a nested fn is
  visible after its declaration in its block, and read-only captures are passed
  uncopied as today.
- Each activation of the host has its own variables, so a recursive host is
  correct.
- `par`: a nested fn that writes a capture, directly or through a sibling, may
  be called from at most one `par` branch. Two branches writing the same local
  race, the same hazard PAR-001 already rejects for a shared record. This needs
  a new PAR-001 case.
- FUNC-002 retires. The code stays reserved, unused, so archived diagnostics
  keep their meaning.

## Lowering for (c)

The D62 lift already computes, per nested fn, the enclosing locals it reads,
transitively through siblings. Add the ones it **writes**, with the same
transitive closure: a fn that calls a writer of `x` must pass `x` through
unchanged. Two ways to carry them:

1. **By-reference capture parameters (recommended).** A written capture
   becomes a `by_ref` parameter of the lifted fn; read-only captures stay as
   they are. Scalars, `Text` and records need nothing more: the VM's var
   parameters already work, recursion included. Arrays need the three shims of
   decision D8's `arrays=ref` arm, which are in `src/aether/experiment.c`
   today (`results/decisions/arrays_2026-10-07.md`):
   - Shim 1, an rvalue argument moved into a temp, never fires here: a capture
     argument is always a name.
   - Shim 2 normalises a literal-built (static) array with
     `setlength(v, length(v))` before a call whose callee resizes it. The D62
     call list says which calls those are.
   - Shim 3 turns a whole-value read of a by-ref array (`let c = xs;`,
     `ret xs;`) into an element-wise copy. D62 hit exactly this failure with a
     by-ref read-only capture before shipping its uncopied path
     (`results/decisions/d62_readonly_capture.md`).

   One known VM gap: under D8's arm, a method forwarding its by-ref array
   parameter to another method failed with `GET_INDIRECT requires an address
   on the stack` (`corpus/28_perceptron`). Nested fns forward written captures
   constantly, through recursion and sibling calls. D62's first, by-ref cut
   forwarded captured arrays exactly that way between free functions, and
   N-Queens printed the right count, so the gap looks method-specific. The
   fixtures must cover forwarding anyway, and a pscal-core fix is the price
   if they fail.
2. **An environment record.** Move written captures into a synthesized
   per-host record, rewrite the host's references to field accesses, and give
   nested fns the record. It uses only existing record lowerings and has no
   VM by-ref corners. It costs a synthesized type, a rewrite of every host
   reference (the D62 walk already resolves each reference to its
   declaration), and a field indirection on every host access. This is the
   fallback if a shim gap appears.

## Costs and risks

- **Coded errors become silent wrong answers** when a writer is also wrong:
  2 of the 9 first attempts. Neither passes today. The trade is 7 first
  attempts from coded error to pass against 2 from coded error to silent
  wrong.
- **D49 and D62 said "no mutable capture."** That kept FUNC-002 simple. It was
  never needed for safety: nested fns cannot escape their host, so a capture by
  reference cannot dangle, and no closure object or runtime is involved.
- **Guides get shorter.** The FUNC-002 rule, the repair line and the card's
  "never assign them" go, a net cut in all four (to be measured).
- **Training data:** the next fine-tune keeps the habit the tuned model already
  has, instead of unlearning it.

## Draft decision rows

| ID | Decision | Status | Options considered | Resolution | Evidence and decision rule |
|---|---|---|---|---|---|
| D64 | **A nested function may assign its enclosing locals** (refines D62 and D49) | accepted 2026-10-18 (owner; option (c) for D64, overriding the pre-registered board rule) | keep read-only (D62); scalars only; assignable capture; explicit `nonlocal`; read-only with a top-level-`let` hint | Assignable capture: a nested fn's assignments, element writes and appends change the enclosing variable. Lowered as by-reference capture parameters, with the D8 `arrays=ref` shims promoted for capture parameters (fallback: an environment record). PAR-001 rejects a capture-writing nested fn called from two `par` branches. FUNC-002 retires. The rest of D62 stands | Census (`results/decisions/nested_assign_2026-10-18.md`): 21 archived attempts stop on FUNC-002 (9 first); made legal, 15 pass and 6 print a wrong answer (first attempts: 7 pass, 2 silent wrong); every writer assigns an array. **Pre-registered rule (written before the Release B board reports):** adopt (c) if that board's first attempts show FUNC-002 on at least 2 distinct tasks, and fewer than half of those cases reach a pass within their repair attempts. Otherwise keep D62 read-only and ship the top-level-`let` FUNC-002 hint (option e); re-run the census after the next fine-tune's scoring |
| D65 | **Annotations on a nested function** | accepted 2026-10-18 (owner) | SYN-001 as today; ANN-001 with a hint to drop it; accept | Accept `@pure`, `@pre` and `@post` directly above a nested fn, checked as on any fn (parseFnDecl already builds the guards; statement position collects the annotations before a nested `fn`) | 19 archived attempts (7 distinct, 2 models) write `@pure` above a nested fn and get the generic `[SYN-001] unexpected token in block`. With it accepted, 3 pass; the other 16 fail later on unrelated errors (FX-001, TUP-001). Ships with D64 or alone; independent of the board |

## Rollout if accepted

One release, after the board decides:

- **Lowering:** by-ref written captures and the promoted shims; PAR-001's new
  case; D65's annotation collection.
- **Fixtures:** a counter, an element write, an append, a slice reassignment,
  sibling writes through a non-writing caller, a recursive host, a write seen
  by a later read in the host, `par` rejection, and annotated nested fns.
- **Registry:** `nested_fn_assign` flips from class 2 to class 1 with a
  top-level twin; a new `par` entry.
- **Spec:** `G.NestedFn.2` flips from `@reject=FUNC-002` to `@ok`.
- **Guides:** drop FUNC-002 from all four, paired with nothing, a net cut.
- **Drill:** `repair_nested_fn_assigns_local` inverts; retire or repoint it to
  the `par` case.
- **Shipping:** CHANGELOG, VERSION, census re-run (expected: the 15 pass on
  this archive), instrument check.

## Open questions for the owner

- Accept the pre-registered rule as written, or decide now without waiting for
  the board?
- Ship D65 now, on its own? It does not depend on the board.
