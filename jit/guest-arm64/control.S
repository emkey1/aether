// AArch64 guest control-flow gadgets. `svc` adapted from OpenMinis/
// ish-arm64's asbestos/guest-arm64/gadgets-aarch64/control.S (GPLv3, see
// /CREDITS-aarch64.md; see below for its INT_SYSCALL->INT_ARM64_SVC fix).
//
// Everything else here (b/bl/br/blr/ret/cbz/cbnz/tbz/tbnz/b.cond) is
// independently written, NOT adapted from their equivalents. Their
// versions use `inline_chain`/fake_ip block-linking (a compile-time-known
// target either resolves to a direct code pointer for same-JIT-call
// continuation, or falls back to the main loop) -- a real optimization,
// but one that requires teaching gen_step_arm64 their fake_ip packing
// scheme and how it interacts with this codebase's own, differently-
// shaped block-chaining machinery (the jump_ip[]/jumps_from[] patching
// already wired into cpu_step_to_interrupt_arm64, jit/jit.c -- ported
// from i386's precedent, not from OpenMinis). Reconciling two different
// chaining schemes correctly is exactly the kind of subtle-bug risk
// worth avoiding on a first pass: every gadget here instead stores its
// target into CPU_pc and unconditionally exits via jit_ret (shared,
// already proven by i386), letting the existing block-lookup/compile/
// cache loop re-dispatch normally. Slower per-branch (an exit-to-C round
// trip every time, no inline chaining) but simple and easy to get right.
// Revisit once broader gadget coverage is correctness-proven and chaining
// is worth the complexity -- same "correctness first" discipline used
// throughout this port.
//
// bcond_* gadgets use the host's own b.<cond> instruction after loading
// NZCV into the host flags register (load_flags, gadgets.h) -- one gadget
// per condition code, selected at gen_step_arm64 compile time, rather
// than decoding the condition and testing it generically in assembly.

#include "gadgets.h"

.text

.extern jit_ret
.extern jit_exit

/*
 * System call (SVC)
 * Code stream: [gadget addr][next_pc (64-bit)]
 * Sets guest PC to next instruction before exiting.
 *
 * Interrupt code changed from OpenMinis' INT_SYSCALL (0x80, i386's
 * syscall interrupt number) to INT_ARM64_SVC (emu/interrupt.h) -- this
 * codebase has a dedicated arm64 SVC interrupt code and dispatch path
 * (handle_arm64_syscall_interrupt, kernel/calls.c, patch 5) specifically
 * so arm64 doesn't get routed through i386's syscall ABI by accident.
 * fiber_exit -> jit_exit (this codebase's name for the same shared exit
 * path, jit/gadgets-aarch64/entry.S, reused unmodified).
 */
.gadget svc
    ldr x8, [_ip]           // next instruction address (guest PC after SVC)
    str x8, [_cpu, #CPU_pc]
    mov w0, #INT_ARM64_SVC
    b jit_exit

// B, BL — unconditional branch (target is compile-time-known, computed by
// gen_step_arm64 since PC is known at compile time). Code stream for b:
// [gadget][target_pc]. For bl: [gadget][target_pc][return_addr] -- return
// address is written to X30 before branching.
.gadget b
    ldr x8, [_ip]
    str x8, [_cpu, #CPU_pc]
    b jit_ret

.gadget bl
    ldr x8, [_ip]
    ldr x9, [_ip, #8]
    add x10, _cpu, #CPU_x0
    str x9, [x10, #(30*8)]  // X30 = return address
    str x8, [_cpu, #CPU_pc]
    b jit_ret

// BR, BLR, RET — branch to a register value. Code stream: [gadget][rn].
// For blr, code stream also has [return_addr] after rn, same as bl.
.gadget br
    ldr x8, [_ip]
    and x8, x8, #0x1f
    add x9, _cpu, #CPU_x0
    cmp x8, #31
    b.eq 1f
    ldr x10, [x9, x8, lsl #3]
    b 2f
1:  mov x10, #0
2:  str x10, [_cpu, #CPU_pc]
    b jit_ret

.gadget blr
    ldr x8, [_ip]
    and x8, x8, #0x1f
    add x9, _cpu, #CPU_x0
    cmp x8, #31
    b.eq 1f
    ldr x10, [x9, x8, lsl #3]
    b 2f
1:  mov x10, #0
2:  ldr x11, [_ip, #8]      // return address
    str x11, [x9, #(30*8)]  // X30 = return address
    str x10, [_cpu, #CPU_pc]
    b jit_ret

// RET has identical semantics to BR (branch to register value) -- same
// gadget body, just invoked for a different guest mnemonic/encoding.
.gadget ret
    ldr x8, [_ip]
    and x8, x8, #0x1f
    add x9, _cpu, #CPU_x0
    cmp x8, #31
    b.eq 1f
    ldr x10, [x9, x8, lsl #3]
    b 2f
1:  mov x10, #0
2:  str x10, [_cpu, #CPU_pc]
    b jit_ret

// CBZ, CBNZ — code stream: [gadget][rt | sf<<8][taken_pc][fallthrough_pc]
.gadget cbz
    ldr x8, [_ip]
    and w9, w8, #0x1f        // rt
    ubfx w10, w8, #8, #1     // sf
    add x11, _cpu, #CPU_x0
    cmp x9, #31
    b.eq 1f
    ldr x12, [x11, x9, lsl #3]
    b 2f
1:  mov x12, #0
2:  cbz w10, 3f
    cbz x12, taken_cbz
    b not_taken_cbz
3:  cbz w12, taken_cbz
    b not_taken_cbz
taken_cbz:
    ldr x8, [_ip, #8]
    str x8, [_cpu, #CPU_pc]
    b jit_ret
not_taken_cbz:
    ldr x8, [_ip, #16]
    str x8, [_cpu, #CPU_pc]
    b jit_ret

.gadget cbnz
    ldr x8, [_ip]
    and w9, w8, #0x1f        // rt
    ubfx w10, w8, #8, #1     // sf
    add x11, _cpu, #CPU_x0
    cmp x9, #31
    b.eq 1f
    ldr x12, [x11, x9, lsl #3]
    b 2f
1:  mov x12, #0
2:  cbz w10, 3f
    cbnz x12, taken_cbnz
    b not_taken_cbnz
3:  cbnz w12, taken_cbnz
    b not_taken_cbnz
taken_cbnz:
    ldr x8, [_ip, #8]
    str x8, [_cpu, #CPU_pc]
    b jit_ret
not_taken_cbnz:
    ldr x8, [_ip, #16]
    str x8, [_cpu, #CPU_pc]
    b jit_ret

// TBZ, TBNZ — code stream: [gadget][rt | bit_pos<<8][taken_pc][fallthrough_pc]
.gadget tbz
    ldr x8, [_ip]
    and w9, w8, #0x1f        // rt
    ubfx w10, w8, #8, #6     // bit position (0-63)
    add x11, _cpu, #CPU_x0
    cmp x9, #31
    b.eq 1f
    ldr x12, [x11, x9, lsl #3]
    b 2f
1:  mov x12, #0
2:  lsr x12, x12, x10
    tbz x12, #0, taken_tbz
    b not_taken_tbz
taken_tbz:
    ldr x8, [_ip, #8]
    str x8, [_cpu, #CPU_pc]
    b jit_ret
not_taken_tbz:
    ldr x8, [_ip, #16]
    str x8, [_cpu, #CPU_pc]
    b jit_ret

.gadget tbnz
    ldr x8, [_ip]
    and w9, w8, #0x1f        // rt
    ubfx w10, w8, #8, #6     // bit position (0-63)
    add x11, _cpu, #CPU_x0
    cmp x9, #31
    b.eq 1f
    ldr x12, [x11, x9, lsl #3]
    b 2f
1:  mov x12, #0
2:  lsr x12, x12, x10
    tbnz x12, #0, taken_tbnz
    b not_taken_tbnz
taken_tbnz:
    ldr x8, [_ip, #8]
    str x8, [_cpu, #CPU_pc]
    b jit_ret
not_taken_tbnz:
    ldr x8, [_ip, #16]
    str x8, [_cpu, #CPU_pc]
    b jit_ret

// B.cond family — code stream: [gadget][taken_pc][fallthrough_pc]. One
// gadget per condition code; gen_step_arm64 picks the right one at
// compile time based on the decoded cond field.
.macro bcond_gadget name, cc
.gadget \name
    load_flags
    b.\cc 1f
    ldr x8, [_ip, #8]
    str x8, [_cpu, #CPU_pc]
    b jit_ret
1:  ldr x8, [_ip]
    str x8, [_cpu, #CPU_pc]
    b jit_ret
.endm

bcond_gadget bcond_eq, eq
bcond_gadget bcond_ne, ne
bcond_gadget bcond_cs, cs
bcond_gadget bcond_cc, cc
bcond_gadget bcond_mi, mi
bcond_gadget bcond_pl, pl
bcond_gadget bcond_vs, vs
bcond_gadget bcond_vc, vc
bcond_gadget bcond_hi, hi
bcond_gadget bcond_ls, ls
bcond_gadget bcond_ge, ge
bcond_gadget bcond_lt, lt
bcond_gadget bcond_gt, gt
bcond_gadget bcond_le, le
