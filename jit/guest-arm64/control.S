// AArch64 guest control-flow gadgets. Adapted from OpenMinis/ish-arm64's
// asbestos/guest-arm64/gadgets-aarch64/control.S (GPLv3, see
// /CREDITS-aarch64.md). Only `svc` ported in this first slice (Phase A of
// the JIT gadget port, aarch64_guest_plan.md) -- branches/calls follow
// once this vertical slice (movz/adr/svc, enough for tests/arm64/
// arm64_hello.s) is validated end to end.
//
// The `svc` gadget's interrupt code was changed from their INT_SYSCALL
// (0x80, the i386/legacy syscall interrupt number) to INT_ARM64_SVC
// (emu/interrupt.h) -- a real adaptation, not a mechanical rename: this
// codebase already has a dedicated arm64 SVC interrupt code and dispatch
// path (handle_arm64_syscall_interrupt, kernel/calls.c, wired in patch 5)
// specifically so arm64 doesn't get routed through i386's syscall ABI by
// accident. `fiber_exit` -> `jit_exit` (this codebase's name for the same
// shared exit path, jit/gadgets-aarch64/entry.S, reused unmodified).

#include "gadgets.h"

.text

.extern jit_exit

/*
 * System call (SVC)
 * Code stream: [gadget addr][next_pc (64-bit)]
 * Sets guest PC to next instruction before exiting.
 */
.gadget svc
    ldr x8, [_ip]           // next instruction address (guest PC after SVC)
    str x8, [_cpu, #CPU_pc]
    mov w0, #INT_ARM64_SVC
    b jit_exit
