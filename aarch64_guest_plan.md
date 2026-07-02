# iSH-AOK aarch64 Guest Port Plan

Date: 2026-07-01
Branch: `aarch64`

## Motivation

Open Minis (App Store id 6759188481) ships a fork of upstream iSH,
[`OpenMinis/ish-arm64`](https://github.com/OpenMinis/ish-arm64), that adds a
**native AArch64 guest backend** alongside the existing i386/amd64 guest
support. Because their host (iOS/Apple Silicon) and that guest architecture
match, guest instructions map close to 1:1 onto host gadgets instead of going
through full x86-semantics translation. Their own numbers: 7-12x faster than
x86 emulation on compute-heavy workloads (`int_arith_2M` 12x, `fib(30)` 9.2x,
`sum(1M)` 10.2x, `seq+awk 100K` 7.2x). That's the entire "Open Minis feels
faster" effect users are reporting — not a better JIT, just skipping
cross-architecture translation for aarch64 binaries.

Both projects are GPLv3 derivatives of the same upstream (`ish-app/ish`), so
this plan explicitly leverages their public source as a design reference and,
where practical, ports code directly with attribution — see
[Attribution](#attribution) below. This is not a clean-room reimplementation;
where their `asbestos/guest-arm64/` and `kernel/arch/arm64/` code is
GPLv3-compatible and fits our tree, we adapt it and credit it.

## Why This Port Is Narrower Than the amd64 Port

`amd64_port_plan.md` had to build the ABI-split infrastructure from scratch:
per-task ABI enum, 64-bit-capable MM, ELF64 loading, a second syscall table
mechanism. All of that now exists and is proven (see the conversation that
produced this plan for full file:line citations):

- `enum guest_abi` on `struct task` (`kernel/abi.h:9`, `kernel/task.h:30`),
  re-derived per `execve()` from the ELF header (`kernel/exec.c:59-69`,
  `kernel/exec.c:547`).
- `struct syscall_abi_dispatch` selecting a table + marshalling functions per
  ABI (`kernel/calls.c:1211-1220`, `kernel/calls.c:1522-1529`).
- ELF64 parsing already exists (used for amd64) and needs only a new
  `EM_AARCH64` case, not a new struct.
- 64-bit sparse MM, canonical-address validation, and non-4GB VM layout
  already exist (`guest_abi_vm_layout()`, `kernel/abi.h:70-98`).
- The interpreter/PIE-load-bias and `PT_INTERP` arch-consistency check already
  branch on `abi` (`kernel/exec.c:520-523`, `565-575`) — a third arch is a
  new `case`, not new logic.

What's genuinely new: the AArch64 register file, decoder, native gadget
engine, and syscall table content. That's still substantial, but it's adding
a third parallel engine (following the amd64-frontend precedent —
`emu/amd64_interp.c` / `jit_block_compile_amd64`, dispatched via
`current->abi == GUEST_ABI_AMD64` at `jit/jit.c:1500-1515`) rather than
re-deriving the whole dispatch skeleton.

## Attribution

When patches in this series adapt or port code from `OpenMinis/ish-arm64`,
each commit message must name the source file(s) and note "adapted from
OpenMinis/ish-arm64, GPLv3." A `CREDITS-aarch64.md` file (added in Patch 1)
tracks this at the file level. The top-level README gets an Acknowledgments
line pointing to their repo once the branch has real content. Do not silently
vendor their assembly or C files without a credit line in the file header —
GPLv3 requires preserving notices, and it's simply the right thing to do
given they did real engineering work here.

Likely direct-port or close-adaptation candidates (verify license header
compatibility file-by-file before copying, since GPLv3 requires attribution,
not permission — that's already satisfied, but each file should still carry
a provenance comment):

- `kernel/arch/arm64/calls.c` — aarch64 Linux syscall numbering/table shape
  (numbers/ABI facts aren't copyrightable, but the table structure and stub
  choices are useful reference).
- `vdso/arm64/{vdso.S,vdso.c,vdso.lds}` — sigreturn trampoline boilerplate is
  largely mechanical; strong candidate for near-direct adaptation.
- `asbestos/guest-arm64/gadgets-aarch64/*.S` — instruction-class organization
  (bits/control/crypto/entry/math/memory) as a structural template even if
  we write our own gadget bodies to match iSH-AOK's existing gadget/tlb
  conventions.
- `emu/arch/arm64/{cpu.h,decode.h}` — register file and decode-context shape.

## Non-Goals for Initial Bring-Up

- SVE/SVE2, MTE, PAC/BTI enforcement.
- Full NEON crypto (AES/SHA/CRC32) — stub or trap-and-emulate until a
  static/basic userland boots.
- Mixed-arch same-process-tree exec (i386/amd64 parent execing an arm64
  child, or vice versa) — architecturally unblocked by the existing `abi`
  redetection in `execve()`, but treat as its own validation milestone after
  arm64 alone is stable, not a day-1 requirement.
- Multiarch rootfs (`dpkg --add-architecture arm64` inside a Devuan guest) —
  a rootfs/provisioning concern once the kernel-level support works, not part
  of this patch series.

## Concrete Patch Series

### 1. ABI Scaffolding for a Third Architecture

Files: `kernel/abi.h`, `kernel/task.h`, `kernel/exec.c`, `kernel/elf.h`,
new `CREDITS-aarch64.md`

Work:
- Add `GUEST_ABI_ARM64` to `enum guest_abi`.
- Add `EM_AARCH64` to `kernel/elf.h` constants; extend `elf_abi_detect()`
  (`kernel/exec.c:76-88`) with the `ELF_64BIT && EM_AARCH64` case.
- Extend the `PT_INTERP` abi-consistency check (`kernel/exec.c:520-523`) —
  already generic, just needs the new enum value to fall through correctly.
- Extend `guest_abi_vm_layout()`/`guest_abi_desc()` (`kernel/abi.h:70-98`)
  with an arm64 entry (48-bit VA, matching OpenMinis' choice and avoiding
  V8-style high-address collisions the amd64 path already had to solve).
- `CREDITS-aarch64.md` scaffold per [Attribution](#attribution).

Exit criteria: tree still builds; i386/amd64 unaffected; `elf_abi_detect`
correctly classifies a real aarch64 ELF without touching execution.

### 2. AArch64 Register File and CPU State

Files: `emu/cpu.h`, `emu/regid.h`, `kernel/task.h`

Work:
- Add arm64 fields to `struct cpu_state` as siblings (following the existing
  `amd64_regs[]`/`amd64_rip` pattern, not a union): `arm64_regs[31]` (X0-X30),
  `arm64_sp`, `arm64_pc`, `arm64_pstate`, NEON/FP `arm64_v[32]` (128-bit each).
- Audit save/restore, clone, ptrace snapshot, and crash-log paths for the new
  fields (mirrors amd64 port step 4's audit list).

Exit criteria: arm64 register state can be initialized, copied on fork, and
dumped in crash logs, with i386/amd64 unaffected.

### 3. AArch64 Decoder — DONE (scope-cut), see below

Files: `emu/arch/arm64/decode.h`, `emu/arm64_interp.c`

Landed: ADR/ADRP, ADD/SUB (immediate, with SP source/dest handling),
MOVN/MOVZ/MOVK, B/BL, B.cond, CBZ/CBNZ, TBZ/TBNZ, BR/BLR/RET, SVC (traps to
`INT_ARM64_SVC`, not yet dispatched — patch 4), LDR/STR (unsigned immediate,
GPR only), LDP/STP (post/pre/signed-offset, GPR only), LDXR/STXR (register,
non-pair), CAS. Decode masks for the trickier encodings adapted from
OpenMinis' `asbestos/guest-arm64/gen.c` — see `CREDITS-aarch64.md`.
Pure-logic pieces (sign extension, branch-immediate decode, ADD/SUB flag
computation, condition evaluation) verified against hand-computed values in
a standalone scratch check.

**Explicitly deferred** (raises `INT_UNDEFINED`, does not silently
misexecute):
- ~~Logical (immediate): AND/ORR/EOR/ANDS~~ — **added post-patch-5**, see
  below. Turned out to be far more urgent than "substantial enough to
  warrant its own pass" suggested.
- Data-processing (register): logical-shifted-register (incl. the
  register-form MOV alias), add/subtract shifted/extended register,
  conditional select, 1-/2-source ops (MUL, UDIV/SDIV, CLZ, etc). **Now
  confirmed as the actual next blocker** — see patch 5's real-rootfs
  findings below and `tests/arm64/README.md`.
- LDXP/STXP (pair exclusives), STLR/LDAR (non-exclusive acquire/release),
  LSE atomic RMW beyond CAS (LDADD/LDCLR/etc).
- LDR (literal, PC-relative), sign-extending LDRSB/LDRSH/LDRSW.
- All SIMD/FP instructions.

**Was "not yet exercised by any test" — now is, and testing paid off
immediately.** Once patch 5 lifted the `ENOEXEC` guard and wired real
execution end-to-end, three hand-assembled test binaries (`tests/arm64/`)
validated the memory-touching paths for real and caught a genuine bug (CAS
mask-ordering, see patch 5 below). Testing against the real Alpine aarch64
rootfs went further and found that Logical (immediate) — deferred here as
lower-priority — is literally the first instruction musl's dynamic linker
executes. Added `arm64_decode_bitmask_imm` (adapted from OpenMinis'
`decode_bitmask_imm`) and full AND/ORR/EOR/ANDS support in response,
validated with a fourth test (`arm64_logical.s`) and confirmed to unblock
2 more real instructions of `ld.so` before hitting the next (DP_REG)
blocker. This is the priority-inversion risk of planning instruction
coverage from the ISA manual instead of from what real code actually
executes first — corrected as soon as real data was available.

Revised exit criteria (the original "hand-written asm tests pass" bar
assumed a test harness that doesn't fit this codebase's conventions):
build passes; pure-logic decode/flag functions verified against hand-
computed values; full instruction-level correctness deferred to differential
testing against the new oracle VM (see Testing Strategy) once patches 4-5
make real guest execution possible.

### 4. AArch64 Syscall Table — DONE (scaffolding; not yet reachable)

Files: `kernel/calls.c`

Landed: `arm64_syscall_table[450]`, `arm64_syscall_number/args/result`
(X8/X0-X5/X0, no rcx/r11-clobber — that's an x86-SYSCALL-instruction side
effect, not an ARM SVC one), and an `arm64_syscall_dispatch` entry wired
into `syscall_dispatch_for_abi`. ~120 real slots, curation adapted from
OpenMinis (see `CREDITS-aarch64.md`), implementations 100% reused from
existing i386/amd64 `sys_*` functions — cross-checked against the exact
names already used in this file's own tables, not typed from memory. Build
resolved every reused name on the first real compile after fixing a
definition-ordering bug (arm64_syscall_result referenced
`syscall_result_is_errno`/`syscall_result_errno` before their definitions).

**Correctness note, not a formality**: this table only works because of
the patch-2-reversal commit above — `kernel/calls.c`'s `syscall_t` is
`dword_t(*)(dword_t x5)`, so every reused `sys_*_amd64` function's pointer
arguments get silently truncated to 32 bits unless the actual guest address
fits in 32 bits, which is only true because the arm64 stack/heap are now
kept low. This is the exact same constraint amd64 lives under for its own
narrow-table entries; genuine 64-bit-pointer safety for syscalls that need
it (large mmap offsets, etc.) requires the same kind of hand-written
`qword_t`-safe special-casing amd64 has in `handle_syscall_interrupt`
(the `sys_open_guest`/`sys_write_guest`/etc. dispatch switch) — not
attempted for arm64 yet, scoped as a follow-up once real execution exists
to motivate which syscalls actually need it.

**Still unreachable**: `kernel/exec.c`'s patch-1 `ENOEXEC` guard still
blocks arm64 exec, so this table has zero live callers yet. That's
intentional — patch 5 (ELF loading) is next, and lifting the guard is that
patch's job, not this one's.

Exit criteria (unchanged, still pending patch 5): tiny arm64 asm syscall
tests pass for `write`, `exit`, `openat`, `read`, `mmap`, `mprotect`,
`brk`, `clone`.

### 5. ELF64 aarch64 Loading and Stack Bootstrap — DONE, exit criteria MET

Files: `kernel/exec.c`, `jit/jit.c`, `kernel/calls.c`, `emu/cpu.h`, `emu/arm64_interp.c`

Landed, smaller than expected: exec.c's ELF64/stack/auxv machinery
(`is_64bit`, `task_abi_desc().elf_platform`, `guest_abi_vm_layout()`) was
already fully abi-generic from the amd64 port — `AT_PLATFORM` and
`uname -m` needed zero changes, they just work via patch 1's
`guest_abi_desc()`. What patch 5 actually needed:

1. Removed the patch-1 `ENOEXEC` guard in `kernel/exec.c`.
2. Wired `cpu_run_to_interrupt_arm64` into `jit/jit.c`'s
   `cpu_run_to_interrupt()` — this was originally scoped to patch 8
   ("Ptrace, Then Native Gadget Engine") but that's wrong: patch 8 is about
   the *native gadget engine*, and the plain interpreter dispatch needed to
   exist much earlier for any arm64 code to run at all. Moved here.
3. Added the unconditional arm64 register-init block in `elf_exec()`
   (mirrors the existing unconditional i386/amd64 blocks — cpu_state's
   arm64 fields are always-present siblings, so initializing them for a
   non-arm64 task is harmless).
4. Wired `INT_ARM64_SVC` into `kernel/calls.c`'s `handle_interrupt()`
   switch via a new `handle_arm64_syscall_interrupt()` — **this was missing
   entirely** and is why the first real test run exited with code 2 instead
   of running anything: `INT_ARM64_SVC` (258) fell through to
   `handle_interrupt`'s `default:` case, which does `sys_exit(interrupt)`,
   and `258 & 0xff == 2`. Not caught by any earlier patch's build check
   because nothing had ever actually triggered an SVC through the full
   stack before.

**Exit criteria verified for real**, not just "compiles clean" — three
hand-assembled test binaries (`tests/arm64/*.s`, built with
`clang -target aarch64-linux-gnu` + `lld`, no libc) run through the full
CLI `ish -r / <binary>` path:
- `arm64_hello.s`: write(2)+exit(2) via SVC — prints "hi", exits 42.
- `arm64_prologue.s`: STP/LDP prologue+epilogue, BL/RET, ADD-immediate,
  SUBS-immediate, B.cond, CBZ — exits 0.
- `arm64_atomics.s`: LDXR/STXR, CAS (success and mismatch/failure cases) —
  exits 0.

**A real bug found and fixed by this testing**: `arm64_atomics.s` initially
hit `INT_UNDEFINED` on the CAS instruction. CAS's encoding also matches the
broader load/store-exclusive family's mask (same top-level "atomic"
encoding group), and since that check ran first in `arm64_execute`'s
if-chain, it intercepted CAS and misclassified it. OpenMinis' `gen.c`
checks CAS before load/store-exclusive for exactly this reason (line 2134
vs 2942) — I had the masks right but the ordering backwards. Fixed by
reordering the checks; see `tests/arm64/README.md` for the full story.
This is precisely the class of bug patch 3's documentation warned "no
test coverage" would leave undetected — confirmed by finding one the
moment real testing happened.

### 6. VDSO

Files: new `vdso/arm64/`, `kernel/vdso.c`, `kernel/exec.c`

Work:
- Adapt OpenMinis' `vdso/arm64/{vdso.S,vdso.c,vdso.lds}` (sigreturn trampoline
  is close to boilerplate) rather than hand-rolling from scratch.
- Follow the amd64 port's precedent of not blocking bring-up on this if it
  turns out to be more involved than expected (amd64 port step 10 allowed
  deferring `AT_SYSINFO*`).

Exit criteria: signal return works without relying on the i386/amd64 VDSO
image.

### 7. TLS, Threads, Signals

Files: `kernel/tls.c`, `kernel/fork.c`, `kernel/signal.c`, `kernel/signal.h`

Work:
- arm64 TLS uses `TPIDR_EL0`, set via a dedicated syscall path (no
  `arch_prctl`-style multiplexer needed — simpler than the amd64 FS-base
  story).
- arm64 signal frame layout (`ucontext`/`sigcontext` shape differs
  substantially from x86 — this is genuinely new work, not reuse).

Exit criteria: dynamic glibc/musl arm64 binaries start; `pthread_create`,
`sigaltstack`, basic signal delivery work.

### 8. Ptrace, Then Native Gadget Engine

Files: `kernel/ptrace.c`, new `jit/guest-arm64/` (gadgets + gen logic for the
new guest backend), `jit/jit.c`

**Naming note**: iSH-AOK's `jit/gadgets-aarch64/` is the *host*-CPU gadget
set (Apple Silicon host, any guest ABI) — completely orthogonal to a new
*guest*-arm64 backend. OpenMinis hit the same collision and resolved it by
splitting into `asbestos/guest-x86/` vs `asbestos/guest-arm64/` top-level
dirs. `jit/guest-arm64/` is the confirmed name for the new guest backend —
do not overload the existing `gadgets-aarch64` name (that stays the host
gadget set, unrelated to this port).

Work:
- Ptrace register set support for arm64 (`NT_PRSTATUS` aarch64 shape) first,
  interpreter-only, before any native gadget work — mirrors amd64 port step
  12's ordering ("interpreter-only userland is stable before the JIT
  starts").
- Then the native gadget engine: since host == guest arch here (unlike the
  amd64-on-arm64 cross-arch case), most gadgets are near-1:1 passthrough —
  reference OpenMinis' `asbestos/guest-arm64/gadgets-aarch64/{bits,control,
  entry,math,memory}.S` for instruction-class coverage and organization,
  write gadget bodies against iSH-AOK's own TLB/gadget-frame conventions
  (see `jit/gen.c`'s existing `gen_step`/`gen_step_amd64` split as the
  pattern a `gen_step_arm64` would follow).

Exit criteria: interpreter-only arm64 userland stable; native gadget engine
validated against the interpreter on the same binaries before being trusted
as the default execution path.

## Testing Strategy — New Oracle Required

`mint` (the existing x86_64 Intel oracle host, see memory
`reference_mint_oracle.md`) is **not usable** for this port — it's the wrong
guest architecture. The session's own host machine is Apple Silicon
(`arm64`, confirmed via `uname -m`), which makes it the natural new oracle:
a local Linux aarch64 VM (Lima or UTM, same tooling already used for `mint`)
gives real-Linux-on-real-arm64 ground truth without cross-arch emulation
noise. Set this up as an early, parallel task — differential testing against
a real kernel is how the amd64 port's regressions got caught, and this port
should not skip that step just because it's "easier" (same-arch dispatch
still has plenty of room for subtle bugs in flag semantics, atomics, and
signal frame layout).

### Milestone 1: Static asm smoke tests
`_start`, `write`/`exit`, `SVC` transitions, LDXR/STXR atomics, branch
coverage — differential against the new local aarch64 VM oracle.

### Milestone 2: Static userland
tiny musl arm64 binaries; `uname`, `mmap`, `mprotect`, `clone`, TLS smoke.

### Milestone 3: Dynamic loader bring-up
`ld-linux-aarch64.so.1 --help`, auxv validation, relocation coverage.

### Milestone 4: Basic shell
`/bin/sh`, coreutils smoke, pipes, signals, `wait4`.

### Milestone 5: Minimal distro
Alpine aarch64 or Devuan arm64 minbase, `apk`/`dpkg`, `apt`, Python.

### Milestone 6: Mixed-arch validation (deferred, see Non-Goals)
Once arm64-alone is stable: exec an arm64 binary from an i386/amd64 parent
task in the same rootfs, and vice versa. Confirm the existing
`interp_header.abi != header.abi` check (`kernel/exec.c:520-523`) correctly
rejects a mismatched interpreter for the new arch, and that `copy_task()`
(`kernel/fork.c:65`) inheriting `abi` at fork + re-derivation at `execve()`
produces correct behavior for a fork'd child that execs a different-arch
binary. This scenario is architecturally supported today but was flagged in
research as never having been exercised for i386/amd64 either — worth a
regression test that covers both the existing pair and the new arm64 case.

### Milestone 7: Performance
Differential vs the interpreter, vs the existing i386/amd64 JIT paths on
equivalent workloads, and vs OpenMinis' published numbers as a sanity check
(not a target to beat blindly — their numbers are for a compile-time-only
single-arch build; a correctness-first, runtime-multiplexed build carries
some overhead they don't have to pay).

## Priority Order

1. ABI scaffolding (patch 1) — DONE
2. Register file + CPU state (patch 2) — DONE
3. Decoder + interpreter core (patch 3) — DONE (scope-cut)
4. Syscall table (patch 4) — DONE (scaffolding)
5. ELF64 loading + stack bootstrap, interpreter wired into jit.c dispatch,
   INT_ARM64_SVC wired into handle_interrupt (patch 5) — DONE, exit
   criteria verified with real hand-assembled test binaries
6. TLS/threads/signals (patch 7) — VDSO (patch 6) can interleave or lag
7. Dynamic userland bring-up (milestone 3-4) — needs an arm64 rootfs, not
   yet built
8. Ptrace (originally scoped here; the interpreter-dispatch wiring itself
   moved to patch 5 once it became clear ptrace needs something to observe,
   which needs real execution, which needs that wiring)
9. Native gadget engine (still patch 8)
10. Mixed-arch validation (milestone 6)

## Notes

- First implementation commit on this branch should cover patch 1 only,
  plus `CREDITS-aarch64.md`, and should not touch execution behavior at all.
- Interpreter-only arm64 is the first release-quality milestone, same
  discipline as the amd64 port.
- If forced to choose between early native-gadget work and correct
  glibc/musl + TLS + signal semantics, always choose the latter — same rule
  as the amd64 plan, doubly true here since the whole point of this port is
  "does it actually run real userland," not just synthetic benchmarks.
