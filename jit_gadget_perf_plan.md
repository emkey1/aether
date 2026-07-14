# JIT gadget-level perf plan, all four guest arches (2026-07-10)

STATUS 2026-07-10 evening: P1 LANDED (riscv64 folds, 9b2dc66e on riscv,
~5-6% A/B). arm64 adrp+add fold LANDED (3e323315 on working, ~1-2%);
register-form cmp+b.cond audit: already covered. P2 i386 slice LANDED
(c5e87a01 on working, aarch64 host): busybox sh loop 8.0-9.1s ->
5.5-5.7s (~35%). Remaining: amd64 frontend fusion, x86_64-host fused
gadgets, 8-bit forms, riscv64 tier-2, P0 instrumentation, P4-P7.

LATE-DAY UPDATE: amd64 gzip gap SOLVED = emulator built at -O0 (CLI +
Xcode Debug config both; ISH_MESON_BUILDTYPE override added, 69c79b61).
At debugoptimized amd64 overtakes i386. The -O2 profile rewrites the
amd64 list: (1) RETRY block chaining (frontend round trip now dominates;
use the arm64/riscv64 tagged-word+chain_budget scheme; the old no-win
verdict was likely a -O0 measurement), (2) lazy-fy the eager PF/AF flag
deposit, (3) stop the 0F bridge re-decoding per execution, (4) reg-cache
churn shrinks if chaining lands. Benchmark ONLY debugoptimized builds.

FIRST ON-DEVICE -O2 RESULTS (2026-07-11): fork/exec is the standout --
~10-20x faster on i386/amd64/arm64, ~3-4x on riscv64 (it's ~pure kernel
C, almost no JIT-gadget time, so it's the workload most exposed to
emulator build quality, independent of gadget fusion). sh-loop moved
14-48% (amd64 biggest, now ties i386). i386 sha256/gzip flat as
predicted. amd64/arm64/riscv64 sha256/gzip/bmm from the same session
were measured AFTER 40-80+ min of continuous sweeping and are thermally
contaminated (reprobe read 2x the sweep-start value) -- NOT evidence of
anything, redo with a probe immediately before each compute leg. Full
numbers + device gotchas (devicectl launch can drop guest networking;
use setsid+</dev/null for long detached remote sweeps) in
project_jit_gadget_candidates.md.

Research pass, no code changes. Continues the ranked list in memory
(project_jit_gadget_candidates); supersedes parts of it based on code
facts verified today in `working` and the `riscv` worktree
(/Users/mke/git/ish-AOK-riscv).

Governing principle (proven by the arm64 -O0 pass, 3.9x): **dispatch
COUNT is the lever**, not per-gadget cost. Every estimate below is
reasoned as "dispatches removed per 100 guest instructions."

## Benchmark baseline (iPad M4, 2026-07-10)

| workload | i386 | amd64 | arm64 | riscv64 |
|---|---|---|---|---|
| sha256 64MB | 16.9s | 29.4s | 3.0s | 3.2s |
| sh-loop 200k | 43.8s | 62.4s | 13.3s | 15.4s |
| 1000 fork/exec | 28.5s | 24.8s | 26.0s | 30.0s |

Reading: the RISC engines (1 dispatch per ALU op, natively fused
branches) are 3-4x ahead of the x86 engines (load/op/store accumulator
model, ~3 dispatches per ALU op, lazy flags). riscv64 trails arm64 by
~15% with zero perf work done. The x86 gap is structural; the
candidates below chip at its worst cases.

## Facts established this session (corrections to prior beliefs)

1. **riscv64 ALREADY block-chains, forward AND backward.** The frontend
   (`cpu_step_to_interrupt_riscv64`, jit/jit.c:1543) is a mechanical
   clone of the arm64 one and patches tagged (bit-63) branch words both
   directions (jit/jit.c:1682-1691); `riscv64_branch_dispatch` +
   `riscv64_chain` (jit/guest-riscv64/core.S:56-79) gate loops with
   `frame->chain_budget = 8192` (jit/jit.c:1720). The comment in
   core.S:66-68 saying "nothing chains" is STALE; fix it. So the
   fork/exec anomaly is NOT missing chaining. Note this also updates
   the old "backward chaining forbidden" rule: it remains true for
   i386/amd64 (jit/jit.c:1205 skip), but the RISC engines chain loops
   safely via chain_budget.
2. **fork/exec children are cold on every arch**: `mm_copy` calls
   `mem_init` which allocates a fresh jit (kernel/mmap.c:151,
   emu/memory.c:373). exec also builds a new mm. So a 1000-spawn bench
   is largely a compile-path benchmark. riscv64's compile fetch is the
   slowest of the four RISC-shaped paths: up to two `tlb_read`s per
   instruction (2-byte halfword fetch, jit/gen.c:4809-4833) plus the
   RVC expansion switch; arm64 does one 4-byte `tlb_read`.
3. **x86 guests have zero fusion** (confirmed): `cmp reg,imm; jne` is
   3 dispatches (load reg, sub_imm, jmp_cond) with lazy-flag state
   round-tripped through `CPU_res`/`CPU_flags_res`; `do_jump` reads it
   inline in asm (no C call). The CMP/TEST macros (jit/gen.c:9442-9443)
   and J_REL (9465) are the natural fusion hooks; `gen_arm64_peek_bcond`
   (jit/gen.c:725-739) is the transplantable template, including the
   fits-in-page guard and the "fused pair ends the block, a jump into
   the swallowed second insn just starts its own block" trick.
4. **x86 CISC RMW is not gadget-folded**: `add [rbp-N], imm` emits ~5
   dispatches with the address generated TWICE and TWO independent TLB
   preps (read_prep + write_prep). Better than the 3-instruction -O0
   form (~9 dispatches) but leaves the arm64 `rmw_*_fast`-class win on
   the table.
5. **SSE 128-bit moves always call a C helper**: `movdqu` load or store
   is 1 dispatch but pays addr-gen + TLB prep + C helper call per 16
   bytes, no fast path (jit/gen.c:9684-9739, gadgets-*/misc.S). An SSE
   memcpy loop is helper-call-bound.
6. **x86 call/ret is already return-cache-chained on both hosts**
   (LOCAL_ret_cache, gadgets-{x86_64,aarch64}/control.S). Off the
   candidate list. But the arm64 GUEST and riscv64 engines have NO
   return cache: riscv64 `jalr` (jit/guest-riscv64/alu.S:219-229) and
   arm64-guest RET exit to C + hash lookup on every indirect return.
   Direct `jal` calls chain fine.
7. **riscv64 static pair statistics** (llvm-objdump over Alpine riscv64
   busybox + musl ld.so + apk, 327,850 insns, fall-through pairs only,
   dependency-verified where stated):
   - `ld->ld` 6.7% of pairs / `sd->sd` 5.1%; of those, same-base
     adjacent-offset (fusable as a pair gadget): 12,374 ld + 7,814 sd
     pairs = pairs covering ~12% of all insns. Register save/restore
     runs (prologue/epilogue), hot in call-heavy code.
   - `auipc+addi` (dep) 5.4% of insns; `auipc+load` (dep) 3.6%;
     `lui+addi` 0.66%. All fold to compile-time constants (gen knows
     guest pc).
   - `load+branch` (dep) 3.0%; `li+branch` 1.9%; `alu+store` 2.5%;
     `load+alu` 1.9%.
   - **`slli+add` is only 0.65%** of insns; the hypothesized indexing
     idiom is real but small. Deprioritized.
   - Caveat: static counts over-weight cold code. The dynamic histogram
     (P0) decides final ordering of tier-2 fusions.
8. **x86 static pair statistics** (same method, Alpine i386 + x86_64
   busybox and musl ld.so, Intel-syntax llvm-objdump, ~590k insns):
   - jcc is 8-11% of instructions, and **93-95% of all jcc are
     IMMEDIATELY preceded by their flag setter** (86-89% by cmp/test,
     another 4-7% by flag-setting ALU ops like dec/sub/and). The
     cmp+jcc fusion window is essentially the entire conditional-branch
     population; almost nothing schedules between setter and branch.
   - Memory-destination RMW ALU ops: 1.45% of i386 busybox insns, 0.3%
     in musl (both -O2). The P6 case rests on -O0 guest-compiled code
     (cc1 output), where stack-slot RMW dominates; -O2 system binaries
     barely use it.
   - SSE 128-bit moves: 1.25% of amd64 musl (memcpy/strlen), ~0% in
     i386 Alpine (musl i386 doesn't use SSE string code). P7 is an
     amd64-first candidate and a dynamic-share question (memcpy-bound
     workloads will be far above the static number).
9. **arm64 static stats + engine coverage check** (Alpine aarch64
   busybox + musl, ~270k insns): ldp/stp are 6-7.7% of insns but the
   engine ALREADY handles them as one gadget with a single double-width
   prep (memory.S ldp_gadget, bits=128); cbz/cbnz (3.6-3.9%) are
   already single gadgets; adrp already folds to a compile-time
   constant (gen.c ADR/ADRP at compile time). Remaining measured gaps:
   `adrp+add/ldr` adjacency 1.8-3.8% of insns (foldable like riscv64
   auipc pairs), cmp-class->b.cond adjacency 4.5-4.7% (the -O0 pass
   fused the immediate forms; audit whether register-form SUBS/CMP is
   covered), tbz/tbnz ~1%, ldp->ldp runs 1.4% (low value, ldp already
   1 dispatch).

## Prioritized plan

### P0. Measurement instrumentation (do first; gates everything)
Arches: all. Size: small (~1 day).
- Add an env-gated emitted-pair histogram to `gen_step_riscv64` and the
  i386/amd64 emitters, pattern-copying `amd64_jit_dump_fallback_histogram`
  (jit/jit.c:300, `ISH_TRACE_AMD64_JIT_STATS`). Static-emitted counts
  weighted by block execution would be ideal; a cheap proxy is counting
  pairs at gen time plus sampling.
- Profile with macOS `sample` (CLI build) on three workloads per arch:
  sh-loop, sha256, 1000-spawn. Gadget symbols give the dispatch mix;
  `gen_step_*`/`gen_end` vs `jit_enter` buckets split compile vs run
  time. This is the same timer-pc technique that found the riscv64 JALR
  bug.
- Acceptance: a table of "top 20 dispatched gadgets per workload" and a
  compile/run split for the spawn bench, riscv64 vs arm64.
- Also split the compile-time bucket into decode/analysis vs. final
  gadget-array emission (not just compile vs run). This decides whether
  a prospective cross-exec JIT cache (parking lot, below) should
  serialize the decoded/IR form or the raw gadget-pointer array.

### P1. riscv64 tier-1 constant folds (gen.c only, no new asm)
Arches: riscv64. Expected: ~4-6% dispatch cut general, more in ld.so
and PLT-heavy startup, so it also helps the fork/exec number.
Size: small (~100-150 lines in gen_step_riscv64 + lookahead helper).
- `auipc+addi` (same rd, dep) -> single `mov_const` of the final value.
  5.4% of insns, saves 1 of 2 dispatches per pair (~2.7% of dispatches).
- `lui+addi` -> `mov_const` (0.66%, free once the peek exists).
- `auipc+ld/lw/...` -> the EXISTING load gadget with rs1 = the
  always-zero x0 slot and the 64-bit stream imm = absolute address.
  Zero new asm; saves a dispatch and the add (~1.8% of dispatches);
  GOT loads. Fault contract unchanged (single access, orig_ip must be
  the SECOND insn's pc; on restart the pair re-decodes and re-folds to
  the same value, same argument as arm64's reordered mov_const).
- Copy the block-boundary rules from arm64: fits-in-page guard before
  consuming (gen_arm64_fits_block analog), swallowed-insn addresses can
  still be entered as fresh blocks, so no target bookkeeping needed.
- Validate: P0 histogram before/after (pair count goes to ~0), sh-loop
  + spawn bench, tests/riscv64 + Alpine boot + apk.

### P2. x86 cmp/test+jcc fusion (the biggest untapped gap)
Arches: i386 first (exists on BOTH hosts, so x86_64 AND aarch64 gadget
asm); amd64 second (aarch64 host only, eager-flags model, separate
emitter hook + gadget family). Expected: pair goes 3 dispatches -> 1
and drops the CPU_res/flags_res store+reload. MEASURED (fact 8): jcc is
8-11% of static insns and 93-95% of them are adjacent to their setter,
so the fusion window covers nearly every conditional branch. With
~2.5-3 dispatches/insn average, removing 2 dispatches at ~9% of insns
is roughly a 7-10% dispatch cut on general code, plausibly 10-20% on
branch-dense loops (sh-loop at 43.8s/62.4s has the headroom). Not a
3.9x; the arm64 number came from stacking several idioms. Design the
peek so the flag-setting-ALU->jcc pairs (dec/sub/and + jcc, another
4-7% of jcc) can be added later with the same gadget family.
Size: medium-large. Table-driven fused gadget family (14 conditions,
like `arm64_fused_cmpi*_table`) in gadgets-x86_64/ and
gadgets-aarch64/, plus a peek hook in the CMP/TEST macros.
- Semantics constraint: x86 allows one cmp to feed MULTIPLE jcc
  (`cmp; je; jl` is common), so the fused gadget must still deposit the
  full lazy-flag state (setf_oc + setf_zsp + setf_a) AND branch on the
  live host flags from its own sub. That keeps correctness local: any
  later jcc still reads valid state. Still saves 2 dispatches.
- Respect the existing 15-byte-slack/page-cap rule when consuming the
  jcc (jit/gen.c:718 comment); fused pair ends the block, mirroring
  arm64.
- Measure first (P0): confirm cmp/test->jcc adjacency rate on gcc,
  busybox sh, apk under the histogram, and confirm via `sample` that
  load/sub/jmp_cond gadgets carry the predicted share.
- Validate: the remote differential harness (tests/remote/, conductor.py)
  is the oracle for flag corners; plus amd64_regress.c, sh-loop A/B.

### P3. riscv64 tier-2 fusions (new gadgets, after P0 confirms)
Arches: riscv64 (templates reusable for arm64-guest ldp/stp later).
Expected: another ~6-10% dispatch cut static-weighted; prologue pairs
are call-path-hot so the spawn and sh-loop benches should both move.
Size: medium; the hard part is the staged fault contract.
Order within the tier (re-rank by P0 dynamic data):
1. **Paired ld/ld and sd/sd** (same base, adjacent offsets): one gadget,
   two stream offsets, one TLB prep when both land in the same page
   (check `(addr1 ^ addr2) >> 12 == 0` on the fast path, else fall back
   to two preps). Fault contract per the arm64 "Fused -O0 idioms" header:
   one orig_ip word per faultable access, _ip advanced in stages, first
   result committed to cpu_state before the second access, aliasing
   guards (rd1 == base etc.) rejected at gen time.
2. **load+branch** (3.0%) and **li+branch** (1.9%): riscv64 analog of
   arm64 `fused_ldcmpr`; the branch gadget already takes two reg
   offsets, a fused form takes base/imm/orig_ip plus the two targets.
3. **alu+store** (2.5%) / **load+alu** (1.9%) if the histogram says the
   remaining pairs are hot (compiler-shaped, so likely -O2 body code).
- `slli+add`: measured small (0.65%), skip unless the dynamic histogram
  disagrees.

### P4. riscv64 fork/exec: compile-path fetch (pending P0 split)
Arches: riscv64 (fetch), all (cold-fork fact is universal).
Expected: if the spawn bench is compile-bound as suspected, halving
fetch TLB traffic is the cheapest lever; combined with P1 (fewer
gadgets to emit) it should close most of the 30.0 vs 26.0 gap.
Size: tiny to small.
- Tiny: fetch 4 bytes in ONE tlb_read when `(ip & (PAGE_SIZE-1)) <=
  PAGE_SIZE-4`, using the low half for RVC; keep the 2-byte path only
  for the page-tail case (the current split exists solely to avoid
  faulting the next page, jit/gen.c:4809-4812).
- Small (optional, measure first): cache the host page pointer across
  the block-compile loop instead of a tlb_read per instruction.
- Do NOT chase chaining here; it exists (fact 1).

### P5. Indirect-return cache for riscv64 and arm64 guests
Arches: riscv64 + arm64 guest. Expected: returns are ~1-2% of dynamic
instructions but each currently costs a full jit_ret-to-C round trip +
hash lookup + frame re-entry, easily 10-50x a dispatch; on call-dense
code (shell, ld.so) several percent. Measure the exit rate first with
an env-gated counter in the frontends.
Size: medium. Copy the proven LOCAL_ret_cache design from
gadgets-aarch64/control.S:3-77 (call stashes _ip keyed by return
address; ret verifies and chains). `frame->ret_cache` already exists
and is memset by the riscv64 frontend (jit/jit.c:1668) but unused.
Constraint: a predicted return re-enters a block without passing C, so
it must decrement chain_budget / honor the poke check exactly like
riscv64_chain does, or a call/return loop starves jetsam.

### P6. x86 -O0 stack RMW single-prep gadget
Arches: i386 + amd64. Expected: `add [rbp+disp], imm` 5 dispatches +
2 addr-gens + 2 TLB preps -> 1-2 dispatches + 1 addr + 1 prep (use
write_prep; a writable page is readable). Measured only 0.3-1.5% static
in -O2 system binaries (fact 8), so this is squarely a cc1/-O0
guest-compile candidate; measure the -O0 dispatch mix first (P0) and
only build it if that confirms. Size: medium.

### P7. SSE 128-bit load/store inline fast path
Arches: i386 + amd64. Expected: removes a C helper call per 16 bytes on
TLB-hit; SSE memcpy loops are helper-bound today, and the rep-movs fix
(29x) showed how much the string paths were leaving on the table.
Size: medium, two host gadget sets to touch. Validate with the SIMD
diff harness plus memcpy microbench A/B.

## amd64 cmp+jcc fusion design notes (gathered 2026-07-10, not yet built)

- amd64 jcc today: gen_amd64_jcc (gen.c:399) emits one of 8
  amd64_jcc_<cond> gadgets; negation = swapped targets at emission. The
  gadget re-derives the condition from EAGER eflags (amd64_do_jump,
  control.S:220 -- never CPU_res), stores CPU_amd64_rip/eip, exits via
  jit_ret with NO chaining (jump_ip not set; chaining measured no-win on
  amd64). So the fused twin's tail = compute cond from live NZCV, store
  rip from [_ip,off]/[_ip,off+8], b jit_ret.
- cmp/test emission sites that end an instruction with a bare flag-op
  gadget (fusable by the same look-behind rewrite as i386):
  cached_arith_reg_reg op==7 (gen.c:8577, packed word),
  arith_reg_reg op==7 flush-style for r8-r15 (gen.c:8589),
  cached_arith_reg_imm op==7 (gen.c:8191, 2 operand words),
  cached_logic_reg_imm if TEST routes there (verify TEST 0x84/0x85 and
  F6/F7 /0 handling first), w16 forms skipped.
- Complication vs i386: gen_amd64_jcc calls gen_amd64_flush_reg_cache
  FIRST, which may emit gadget_amd64_store_low8_reg_cache (single word,
  no operands) BETWEEN the cmp and the branch. Fix: at fuse time, if a
  flush is needed, rewind past the noted cmp words, emit the flush word,
  re-emit the fused twin. Runtime-safe: flushing before the cached cmp
  only stores the cache back; the host cache regs still hold correct
  values and the fused branch ends the block.
- deferred-rip: cmp defers, jcc invalidates without emitting set_rip
  (the branch gadget writes rip itself) -- fused twin same, nothing
  lands between. Fused twin must still deposit the full EAGER flag set
  exactly like the unfused op gadget (eflags bits 6/7/2 + CPU_cf/CPU_of
  bytes; see the amd64 setf block in math.S ~304).
- Also fix while there: gate the leftover ungated cc1_trace in the amd64
  frontend (jit.c ~1487, noted in project_amd64_block_chaining).

## Parking lot / strategic notes

- **The x86 3-dispatch accumulator model is the real ceiling**: riscv64
  proves a two-operand byte-offset gadget scheme runs 1 dispatch per
  ALU op. Retrofitting that into the x86 gadget set is an engine
  rewrite, not a candidate for this round; recorded so nobody mistakes
  P2/P6 for the endgame.
- **amd64 slower than i386 on sha256 (29.4 vs 16.9)** despite the
  native-gadget work is unexplained and worth a `sample` while P0 is
  set up. (Different frontends: i386 uses the shared x86-on-aarch64
  engine; amd64 has its own.)
- **Stale comment fix**: jit/guest-riscv64/core.S:66-68 ("nothing
  chains") should be corrected whenever a riscv64 patch next lands.
- **Cross-exec JIT cache** (raised 2026-07-11): fork/exec is already
  known to be compile-bound (fact 2), so caching translations across
  process launches, not just within one process's jit_block table,
  could cut latency for short-lived/frequently re-exec'd programs
  (shell utilities, compiler passes, package-manager sub-processes --
  exactly what Linux userlands hammer the emulator with). Blocked on
  relocation: `gen()` emits a flat, untagged stream mixing raw host
  gadget/helper pointers with plain immediates, so caching the final
  gadget-pointer array would need a load-time rebase pass (host-ASLR
  slide for gadget/helper pointers, possibly a second guest-ASLR slide
  for branch-target immediates) that doesn't exist today. Caching the
  decoded/analyzed IR instead of the final array sidesteps ASLR
  entirely (emission stays fresh each run, always resolving live
  addresses) and may capture most of the win if per-instruction
  emission cost is small relative to decode/analysis. The P0 profiling
  split (decode/analysis vs. emission, added above) should settle
  which design is worth building before either is attempted.
- Extension-hook / instrumentation gadget (plan 5b) remains the
  flexibility item; P0's histogram covers the measurement need for now.
- arm64-guest next tier REVISED by fact 9: the old list's CBZ/CBNZ and
  ldp/stp items are already 1-dispatch (done); the data-supported
  remainder is (a) adrp+add -> fold into the adr constant (pure gen.c),
  (b) adrp+ldr -> absolute-address load (small gadget variant, mirrors
  riscv64 P1's auipc+ld), (c) audit register-form CMP/SUBS + b.cond
  fusion coverage (the -O0 pass tables are immediate-form; measured
  cmp-class adjacency is 4.5-4.7% of insns), (d) tbz/tbnz fusion ~1%.
  The P3 riscv64 gadget templates should be written to be liftable.

## Constraints checklist for ANY new fused gadget (history-proven)

- Mid-block fault restart: guest pc as the LAST code-stream word per
  faultable access; _ip advanced in stages so `[_ip,#-8]` always names
  the faulting sub-instruction; every sub-result committed before the
  next faultable access; gen-time aliasing guards for replay safety.
- x18 never holds live state (Apple platform register); x17 scratch
  discipline per guest-arm64/gadgets.h:149.
- AAPCS64 hazard in helper-calling gadgets: x0/x1/x2 ARE _tmp/_cpu/_tlb;
  consume x2 first, write x1 last (guest-riscv64/memory.S:116-133).
  Values live across a prep call go in x19-x26 (x27 scratch for riscv64
  helpers).
- Backward edges: only via the budgeted chain path (chain_budget), never
  an unbudgeted loop inside gadgets; on i386/amd64 backward linking
  stays forbidden entirely.
- Fused pairs end the block; never let lookahead push the decoded range
  past block_start + PAGE_SIZE (gen_arm64_fits_block analog).
