# aarch64 Guest Port — Credits

The aarch64 (native ARM64 guest) work on this branch is motivated by, and in
places directly adapted from, [OpenMinis/ish-arm64](https://github.com/OpenMinis/ish-arm64),
the fork of `ish-app/ish` powering the Open Minis app's "Linux shell" feature.
Both `ish-AOK` and `ish-arm64` are GPLv3 derivatives of the same upstream
(`ish-app/ish`), so attribution here is required by license and is also just
the right thing to do — they did real engineering work identifying that
same-architecture (arm64 guest on arm64 host) dispatch avoids the
cross-architecture translation cost that dominates i386/amd64 emulation.

This file tracks, at the file level, which parts of this port are adapted
from their source versus independently written against iSH-AOK's own
conventions. Every commit that adapts their code names the source file(s) in
its commit message in addition to the entry added here.

See [aarch64_guest_plan.md](aarch64_guest_plan.md) for the overall plan this
credits file supports.

## Adapted / closely modeled on OpenMinis/ish-arm64

- `emu/arch/arm64/decode.h` — adapted near-verbatim from their
  `emu/arch/arm64/decode.h`: condition-code enum and evaluation
  (`arm64_cond_check`), instruction classification, sign extension, and
  branch/ADR immediate extraction. Field names renamed with an `arm64_`
  prefix (`cpu->arm64_nf` instead of `cpu->nf`) because this codebase's
  `struct cpu_state` is shared across i386/amd64/arm64 tasks and bare
  `zf`/`cf` already name the i386 eflags-bitfield fields; `addr_t` (32-bit,
  i386-specific here) swapped for `guest_addr_t` (64-bit) in
  `arm64_read_insn`; added a `guest_abi_range_valid` bounds check that has
  no equivalent in their arm64-only build.
- `emu/arm64_interp.c` — the bit-field masks and layouts for the trickier
  encodings (load/store exclusive LDXR/STXR, CAS, LDP/STP, LDR/STR
  unsigned-immediate, MOVN/MOVZ/MOVK, ADD/SUB immediate, B/BL/B.cond/CBZ/
  CBNZ/TBZ/TBNZ/BR/BLR/RET, ADR/ADRP, SVC) are adapted from their
  `asbestos/guest-arm64/gen.c`, cited by source line number in the comment
  above each decode site in this file. Their file is a JIT code generator
  (decode and gadget-emission fused together); only the decode/field-layout
  logic is adapted here — the actual arithmetic and flag-computation
  semantics are original, since a same-architecture JIT has no portable-C
  expression of instruction semantics to adapt (the host CPU executes the
  real instruction natively; there's nothing written down in C to copy).
  Their exclusive-monitor comment ("For single-threaded emulation, we can
  treat these as simple loads/stores") is explicitly NOT followed — this
  codebase is SMP-aware, so LDXR/STXR here implement local-monitor-plus-
  value-check semantics instead, flagged in comments as needing real
  differential testing once multiple guest threads are exercised.

## Independently written, informed by their design

- `kernel/abi/arm64.h`, the `GUEST_ABI_ARM64` enum value, and the
  `guest_abi_desc`/`guest_abi_vm_layout` cases in `kernel/abi.h` — written
  against iSH-AOK's existing `guest_abi` dispatch machinery (which
  OpenMinis' fork does not have; their arm64 support is a compile-time
  `#ifdef GUEST_ARM64` build variant, not a per-task runtime ABI). The
  48-bit VA / top-of-address-space stack placement choice mirrors their
  rationale (avoiding a V8 CodeRange collision) but the mechanism differs
  because the surrounding infrastructure differs.
- `struct cpu_state`'s arm64 register fields (`emu/cpu.h`) — structurally
  similar to their `emu/arch/arm64/cpu.h` (same register set, same choice to
  reuse a 128-bit union for V0-V31) but written as siblings within a shared
  multi-ABI struct rather than a per-arch-build struct, and deliberately
  without their redundant packed-`nzcv`-kept-in-sync-with-decoded-flags
  representation (one representation here, no sync-drift risk).
- `emu/arm64_interp.c`'s overall structure (register/memory access helpers,
  step-and-dispatch loop, `cpu_run_to_interrupt_arm64` driver) mirrors this
  codebase's own `emu/amd64_interp.c`, not OpenMinis' code — they have no
  interpreter (their arm64 support is JIT-only; this codebase does
  interpreter-first, JIT-later per `aarch64_guest_plan.md`).

## Syscall numbering reference

Linux aarch64 syscall numbers (asm-generic based) are ABI facts, not
copyrightable expression — using the standard numbering is not "adapted
from" anyone. OpenMinis' `kernel/arch/arm64/calls.c` is still worth
consulting as a reference for table shape and stub choices when patch 4
(syscall table) lands, and will be credited here if any structure beyond the
numbers themselves is reused.
