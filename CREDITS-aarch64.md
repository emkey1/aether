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
  unsigned-immediate, MOVN/MOVZ/MOVK, ADD/SUB immediate, Logical immediate
  AND/ORR/EOR/ANDS, B/BL/B.cond/CBZ/CBNZ/TBZ/TBNZ/BR/BLR/RET, ADR/ADRP, SVC)
  are adapted from their `asbestos/guest-arm64/gen.c`, cited by source line
  number in the comment above each decode site in this file. Also includes
  `arm64_decode_bitmask_imm`, a direct translation of their
  `decode_bitmask_imm` (gen.c:1018) — itself a transcription of the ARM
  ARM's DecodeBitMasks pseudocode, so algorithmically an ISA fact, but the
  code structure (loop-based highest-set-bit search, variable naming) came
  from their file. Their file is a JIT code generator
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

  **Validated the adaptation was worth doing**: patch 5's real-hardware
  test binaries (`tests/arm64/*.s`) caught a genuine ordering bug — CAS's
  encoding also matches the broader load/store-exclusive mask, and this
  file's if-chain checked load/store-exclusive first, misclassifying every
  CAS instruction. OpenMinis' `gen.c` checks CAS before load/store-exclusive
  (line 2134 vs 2942) for exactly this reason; the masks I adapted from them
  were correct, but I got the ordering backwards when transcribing them into
  a flat if-chain instead of their structure. Fixed by matching their
  ordering. See `tests/arm64/README.md`.

## Independently written, informed by their design

- `kernel/abi/arm64.h`, the `GUEST_ABI_ARM64` enum value, and the
  `guest_abi_desc`/`guest_abi_vm_layout` cases in `kernel/abi.h` — written
  against iSH-AOK's existing `guest_abi` dispatch machinery (which
  OpenMinis' fork does not have; their arm64 support is a compile-time
  `#ifdef GUEST_ARM64` build variant, not a per-task runtime ABI). The
  48-bit VA size mirrors their choice; the *stack placement* within it does
  not (patch 2 initially copied their top-of-space placement to dodge a V8
  CodeRange collision, then reversed it once patch 4's syscall-table work
  made concrete that this codebase's narrow `dword_t`-arg `syscall_t` needs
  a low stack/heap the same way amd64's does — see the dedicated commit
  fixing this).
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

## Adapted (curation) / independently written (implementation): syscall table

`kernel/calls.c`'s `arm64_syscall_table` — Linux aarch64 syscall *numbers*
(asm-generic based) are ABI facts, not copyrightable expression, so the
numbering itself isn't "adapted from" anyone. What IS adapted from
OpenMinis' `kernel/arch/arm64/calls.c` is the **curation**: which ~120 of
~450 syscall slots are worth a real implementation versus a stub, reflecting
their real-world experience of what busybox/musl/glibc/Node/Python/git
actually call. That curation choice is credited even though the underlying
facts (numbers) aren't copyrightable — it's real engineering judgment, and
crediting it is simply accurate and fair.

The actual implementations wired into each slot are 100% iSH-AOK's own —
OpenMinis' function names don't exist in this codebase at all (different
kernel/ entirely). Every slot reuses an existing i386/amd64 `sys_*`
function, verified against the exact names already used in this file's
`i386_syscall_table`/`amd64_syscall_table` above it. Notably, the several
`_amd64`-suffixed functions reused here (`sys_stat_amd64`,
`sys_mmap_amd64`, `sys_newfstatat_amd64`, etc.) are named for amd64 but
implement the Linux 64-bit-ABI shape (64-bit stat, direct 64-bit pointer
args) generically, not anything x86-specific — safe and correct to reuse
verbatim for arm64's identical 64-bit-ABI requirements, once the stack/heap
placement fix above made that reuse's narrow-argument-marshalling
assumption hold for arm64 too.
