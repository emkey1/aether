// AArch64 guest control-flow gadgets. `svc` adapted from OpenMinis/
// ish-arm64's asbestos/guest-arm64/gadgets-aarch64/control.S (GPLv3, see
// /CREDITS-aarch64.md; see below for its INT_SYSCALL->INT_ARM64_SVC fix).
//
// Everything else here (b/bl/br/blr/ret/cbz/cbnz/tbz/tbnz/b.cond) is
// independently written, NOT adapted from their equivalents (which use a
// differently-shaped inline_chain/fake_ip scheme).
//
// BLOCK CHAINING (the port's first deliberate perf change, added after
// the cross-arch benchmark showed branch-heavy code ~40% behind the x86
// guests on exit-to-C round trips): compile-time-known branch targets
// are emitted into the code stream TAGGED with bit 63 set
// (target | 1<<63), and gen_step_arm64 records their stream indices in
// state->jump_ip[]. When the C loop later executes destination blocks,
// it PATCHES those words in place with the destination block's host
// `code` pointer (bit 63 clear — a userspace host pointer). Branch
// gadgets load their chosen target word into _ip and jump to
// arm64_branch_dispatch: tag still set -> strip it, store CPU_pc, exit
// to C as before; tag clear -> the word IS the next block's gadget
// array, so after a poke check (signals/timers must not starve — the
// same protection amd64's jit_ret_chain has) dispatch straight into it
// with zero C round trips. The C loop only chains FORWARD edges
// (block->addr > last_block->addr), so every loop's backward edge still
// exits to C — the amd64-era "loop chaining starves jetsam" lesson,
// inherited rather than relearned. Dynamic-target branches (BR/BLR/RET)
// stay unchained.
//
// This is the amd64 frontend's exact tagged-word design
// (gadget_amd64_jmp / jit_ret_chain, jit/gadgets-aarch64/), re-expressed
// for this port's register convention and 64-bit guest PC.
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
// Common branch tail: _ip holds the loaded target word. Tagged (bit 63
// set) = unchained guest address; clear = patched host code pointer for
// direct block-to-block dispatch. See the file header.
.global arm64_branch_dispatch
arm64_branch_dispatch:
    tbz _ip, #63, arm64_chain
    and x8, _ip, #0x7fffffffffffffff
    str x8, [_cpu, #CPU_pc]
    b jit_ret
arm64_chain:
    ldr x8, [_cpu, #CPU_poked_ptr]
    ldrb w8, [x8]
    cbnz w8, arm64_chain_poked
    // Chain budget: bounded time-in-JIT is what makes chaining BACKWARD
    // edges (loops) safe — see jit_frame.chain_budget. Expiry exits to C
    // exactly like a poke, at the destination block's start.
    ldr x8, [_cpu, #LOCAL_chain_budget]
    subs x8, x8, #1
    str x8, [_cpu, #LOCAL_chain_budget]
    b.le arm64_chain_poked
    sub x8, _ip, #JIT_BLOCK_code       // recover the block pointer for the
    str x8, [_cpu, #LOCAL_last_block]  // C loop's chaining bookkeeping
    gret
arm64_chain_poked:
    // Poked or out of budget mid-chain: resume at the DESTINATION block.
    ldur x8, [_ip, #(JIT_BLOCK_addr - JIT_BLOCK_code)]
    str x8, [_cpu, #CPU_pc]
    b jit_ret

.gadget b
    ldr _ip, [_ip]
    b arm64_branch_dispatch

.gadget bl
    ldr x9, [_ip, #8]
    add x10, _cpu, #CPU_x0
    str x9, [x10, #(30*8)]  // X30 = return address
    ldr _ip, [_ip]
    b arm64_branch_dispatch

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
    ldr _ip, [_ip, #8]
    b arm64_branch_dispatch
not_taken_cbz:
    ldr _ip, [_ip, #16]
    b arm64_branch_dispatch

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
    ldr _ip, [_ip, #8]
    b arm64_branch_dispatch
not_taken_cbnz:
    ldr _ip, [_ip, #16]
    b arm64_branch_dispatch

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
    ldr _ip, [_ip, #8]
    b arm64_branch_dispatch
not_taken_tbz:
    ldr _ip, [_ip, #16]
    b arm64_branch_dispatch

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
    ldr _ip, [_ip, #8]
    b arm64_branch_dispatch
not_taken_tbnz:
    ldr _ip, [_ip, #16]
    b arm64_branch_dispatch

// B.cond family — code stream: [gadget][taken_pc][fallthrough_pc]. One
// gadget per condition code; gen_step_arm64 picks the right one at
// compile time based on the decoded cond field.
.macro bcond_gadget name, cc
.gadget \name
    load_flags
    b.\cc 1f
    ldr _ip, [_ip, #8]
    b arm64_branch_dispatch
1:  ldr _ip, [_ip]
    b arm64_branch_dispatch
.endm

// bcond_nf_* — B.cond WITHOUT the load_flags reload, for use directly
// after a fast flag-setting gadget (compare+branch fusion): the host
// NZCV is still live from that gadget's adds/subs/ands, because nothing
// in its tail (store_flags is just mrs+str) nor in gret's dispatch
// sequence (ldr/dmb/cbnz/br) writes flags. `msr nzcv` is serializing on
// Apple cores, so skipping it (plus the flags load) is the single
// biggest per-iteration saving in compare-branch loops. gen_step_arm64
// only emits these when the immediately preceding instruction in the
// SAME block set the flags via a fast gadget (state->arm64_flags_live);
// everything else keeps the reloading bcond_* family above.
.macro bcond_nf_gadget name, cc
.gadget \name
    b.\cc 1f
    ldr _ip, [_ip, #8]
    b arm64_branch_dispatch
1:  ldr _ip, [_ip]
    b arm64_branch_dispatch
.endm

bcond_nf_gadget bcond_nf_eq, eq
bcond_nf_gadget bcond_nf_ne, ne
bcond_nf_gadget bcond_nf_cs, cs
bcond_nf_gadget bcond_nf_cc, cc
bcond_nf_gadget bcond_nf_mi, mi
bcond_nf_gadget bcond_nf_pl, pl
bcond_nf_gadget bcond_nf_vs, vs
bcond_nf_gadget bcond_nf_vc, vc
bcond_nf_gadget bcond_nf_hi, hi
bcond_nf_gadget bcond_nf_ls, ls
bcond_nf_gadget bcond_nf_ge, ge
bcond_nf_gadget bcond_nf_lt, lt
bcond_nf_gadget bcond_nf_gt, gt
bcond_nf_gadget bcond_nf_le, le

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

// ---- Single-gadget compare+branch fusion ---------------------------------
// One dispatch instead of two for the compare-and-branch pairs that
// dominate loop code. The compare executes natively, guest NZCV is still
// stored (later instructions may read the flags), and the branch tests
// the live host flags — no msr reload, no second gadget dispatch.
// gen_step_arm64 peeks at the next instruction when emitting a fast
// compare; on a B.cond it emits one of these instead of the pair.
//
// Per-condition gadget POINTER TABLES are exported from here (one .quad
// table per form) so gen.c indexes extern arrays instead of declaring
// 84 gadget symbols.
//
// Streams:
//   fused_cmpi:  [gadget][rn][imm][taken][fallthrough]
//   fused_cmpr:  [gadget][rn | rm<<8][taken][fallthrough]
//   fused_subsi: [gadget][rd | rn<<8][imm][taken][fallthrough]

.macro fused_cmpi name, cmpop, zr, r15, r11, cc
.gadget \name
    ldr x10, [_ip]
    ldr x11, [_ip, #8]
    add _ip, _ip, #16
    add x14, _cpu, #CPU_x0
    ldr x15, [x14, x10, lsl #3]
    \cmpop \zr, \r15, \r11
    store_flags
    b.\cc 1f
    ldr _ip, [_ip, #8]
    b arm64_branch_dispatch
1:  ldr _ip, [_ip]
    b arm64_branch_dispatch
.endm

.macro fused_cmpr name, cmpop, zr, r15, r13, cc
.gadget \name
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x10, x8, #0x1f
    ubfx x12, x8, #8, #5
    add x14, _cpu, #CPU_x0
    ldr x15, [x14, x10, lsl #3]
    ldr x13, [x14, x12, lsl #3]
    \cmpop \zr, \r15, \r13
    store_flags
    b.\cc 1f
    ldr _ip, [_ip, #8]
    b arm64_branch_dispatch
1:  ldr _ip, [_ip]
    b arm64_branch_dispatch
.endm

.macro fused_subsi name, subop, r16, r15, r11, cc
.gadget \name
    ldr x8, [_ip]
    ldr x11, [_ip, #8]
    add _ip, _ip, #16
    and x9, x8, #0x1f
    ubfx x10, x8, #8, #5
    add x14, _cpu, #CPU_x0
    ldr x15, [x14, x10, lsl #3]
    \subop \r16, \r15, \r11
    store_flags
    str x16, [x14, x9, lsl #3]
    b.\cc 1f
    ldr _ip, [_ip, #8]
    b arm64_branch_dispatch
1:  ldr _ip, [_ip]
    b arm64_branch_dispatch
.endm

#define CC_LIST eq,ne,cs,cc,mi,pl,vs,vc,hi,ls,ge,lt,gt,le

.irp cc, CC_LIST
fused_cmpi  fused_cmpi64_\cc,  subs, xzr, x15, x11, \cc
fused_cmpi  fused_cmpi32_\cc,  subs, wzr, w15, w11, \cc
fused_cmpr  fused_cmpr64_\cc,  subs, xzr, x15, x13, \cc
fused_cmpr  fused_cmpr32_\cc,  subs, wzr, w15, w13, \cc
fused_subsi fused_subsi64_\cc, subs, x16, x15, x11, \cc
fused_subsi fused_subsi32_\cc, subs, w16, w15, w11, \cc
.endr

// Condition order in every table matches enum arm64_cond (decode.h):
// EQ,NE,CS,CC,MI,PL,VS,VC,HI,LS,GE,LT,GT,LE.
// Pointer tables live in .data — putting .quad symbol references in
// .text is an illegal text relocation on Mach-O.
.data
.macro fused_table name, prefix
    .global NAME(\name)
    .balign 8
NAME(\name) :
    .irp cc, CC_LIST
    .quad NAME(gadget_arm64_\prefix\()_\cc)
    .endr
.endm

fused_table arm64_fused_cmpi64_table,  fused_cmpi64
fused_table arm64_fused_cmpi32_table,  fused_cmpi32
fused_table arm64_fused_cmpr64_table,  fused_cmpr64
fused_table arm64_fused_cmpr32_table,  fused_cmpr32
fused_table arm64_fused_subsi64_table, fused_subsi64
fused_table arm64_fused_subsi32_table, fused_subsi32
.text
