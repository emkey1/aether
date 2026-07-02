// AArch64 GUEST gadgets (native ARM64 guest code on ARM64 host). Distinct
// from jit/gadgets-aarch64/, which is the ARM64 HOST gadget set used by the
// i386/amd64 guest JIT — unrelated, see aarch64_guest_plan.md's naming note.
//
// Reuses jit/gadgets-aarch64/entry.S's jit_enter/gret/jit_exit completely
// unmodified: that entry point is guest-ABI-agnostic at the level that
// matters (x0=block, x1=cpu, x2=tlb -> _ip/_cpu/_tlb setup, then dispatch
// into whichever gadget array was compiled). Its unconditional i386
// load_regs/save_regs calls are inert for arm64 tasks — confirmed by
// checking how the existing amd64 frontend already relies on exactly this
// (jit/jit.c:1467 explicitly re-syncs frame->cpu.eip from amd64_rip right
// after jit_enter returns, because save_regs's i386-field write is known
// garbage for a non-i386 task; every downstream consumer is abi-gated and
// never reads those fields for a non-i386 task). No separate entry.S
// needed — do not create one.
//
// Register convention (adapted from OpenMinis/ish-arm64's
// asbestos/guest-arm64/gadgets-aarch64/gadgets.h, GPLv3, see
// /CREDITS-aarch64.md): _cpu=x1, _tlb=x2, _ip=x28 match iSH-AOK's own
// i386-guest convention exactly (same physical registers, verified above),
// so entry.S's setup is correct unmodified. Guest GPR access is
// memory-based (ldr/str against cpu_state per touch) rather than cached in
// host registers the way i386/amd64 cache their <=16 GPRs in x20-x27 —
// arm64 has 31 GPRs, too many to cache that way, and this is the same
// choice OpenMinis made. x20-x27 are left untouched by every arm64 guest
// gadget; whatever i386's load_regs put there is simply never read.

#include "../gadgets-generic.h"
#include "cpu-offsets.h"
#include "emu/interrupt.h"

// CPU_x0/CPU_sp/CPU_pc/CPU_nzcv alias this codebase's actual generated
// offset names (CPU_arm64_regs etc — see jit/offsets.c's arm64() function)
// to the bare names OpenMinis' ported gadget bodies use, so those bodies
// don't need per-line renaming. Defined once, here, not per-file.
//
// CPU_sp needs an #undef first: cpu-offsets.h already defines it for
// i386's own `sp` field (the 16-bit view of esp, see jit/offsets.c's
// OFFSET(CPU, cpu_state, sp)) — a real, caught-by-the-compiler collision,
// not a hypothetical one. Safe to shadow here: each .S file is a separate
// compilation unit, and only arm64-guest gadget files include this header,
// never i386's — this #undef has no effect on i386's own gadgets.h/files.
#define CPU_x0 CPU_arm64_regs
#undef CPU_sp
#define CPU_sp CPU_arm64_sp
#define CPU_pc CPU_arm64_pc
#define CPU_nzcv CPU_arm64_nzcv
#define CPU_excl_addr CPU_arm64_excl_addr
#define CPU_excl_val CPU_arm64_excl_val

_cpu    .req x1
_tlb    .req x2
_ip     .req x28

// Scratch registers for gadget use. Distinct from OpenMinis' _tmp/_tmp2/
// _addr naming (x0/x8/x7) only where needed to avoid clashing with this
// file's _ip/_cpu/_tlb assignments above — x0/x7/x8 don't collide with
// x1/x2/x28, so their choices are kept as-is for easier comparison against
// their source when porting further gadgets.
_tmp    .req x0
_tmp2   .req x8
_addr   .req x7

.extern jit_ret
.extern jit_exit

.macro .gadget name
    .global NAME(gadget_arm64_\()\name)
    .align 4
    NAME(gadget_arm64_\()\name) :
.endm

// Identical to jit/gadgets-aarch64/gadgets.h's gret (same entry.S/jit_ret
// contract) — reproduced here rather than shared via a common header
// because the i386 one is textually entangled with i386-specific
// PROFILE/debug hooks not relevant here. Keep in sync if entry.S's
// contract changes.
.macro gret pop=0
.if \pop == 0
    ldr x9, [_ip], #8
.else
    ldr x9, [_ip, \pop*8]!
    add _ip, _ip, 8
.endif
    dmb ishld /* matches jit/gadgets-aarch64/gadgets.h's ordering fix */
    cbnz x9, 0f
    b jit_ret
0:  br x9
.endm

// ---- Guest register access (memory-based; see file header) -------------

.macro load_guest_reg host_reg, guest_idx
    ldr \host_reg, [_cpu, #(CPU_x0 + \guest_idx * 8)]
.endm

.macro store_guest_reg host_reg, guest_idx
    str \host_reg, [_cpu, #(CPU_x0 + \guest_idx * 8)]
.endm

.macro load_guest_sp host_reg
    ldr \host_reg, [_cpu, #CPU_sp]
.endm

.macro store_guest_sp host_reg
    str \host_reg, [_cpu, #CPU_sp]
.endm

.macro load_guest_pc host_reg
    ldr \host_reg, [_cpu, #CPU_pc]
.endm

.macro store_guest_pc host_reg
    str \host_reg, [_cpu, #CPU_pc]
.endm

// NZCV lives packed in cpu->arm64_nzcv (bits 31:28), matching AArch64
// PSTATE exactly (emu/cpu.h) — load/store it directly into the host NZCV
// flags register with mrs/msr, avoiding a decode/encode round-trip per
// flag-setting instruction.
.macro load_flags
    ldr w9, [_cpu, #CPU_nzcv]
    msr nzcv, x9
.endm

.macro store_flags
    mrs x9, nzcv
    str w9, [_cpu, #CPU_nzcv]
.endm

// ---- Memory access (TLB fast path) --------------------------------------
// Adapted from jit/gadgets-aarch64/gadgets.h's own read_prep/write_prep —
// NOT from OpenMinis' version, which assumes TLB_BITS=13 and a 32-byte
// tlb_entry stride (`lsl x9,x9,#5`). This codebase's actual layout is
// TLB_BITS=10 (jit/offsets.c's new MACRO(TLB_BITS) emits the real value)
// and a 24-byte tlb_entry (three 8-byte fields, MACRO(TLB_ENTRY_SIZE)) —
// i386's own gadgets.h already gets this exactly right (`ubfx x9,_xaddr,
// 12,10` / `eor x9,x9,_xaddr,lsr 22` / `mov w10,TLB_ENTRY_SIZE; madd x9,
// x9,w10,_tlb`), so this macro is a straight copy of that proven-correct
// pattern with register names adjusted for this file's _addr (x7, not
// i386's w3/x3) and _tlb already pointing at tlb->entries (same
// convention, set up once in entry.S, not per-gadget).
.irp type, read,write

.macro \type\()_prep size, id
    and w9, w7, #0xfff
    cmp x9, #(0x1000-(\size/8))
    b.hi crosspage_load_\id
    .ifc \type,write
        ldr x10, [_tlb, #(-TLB_entries+TLB_mmu)]
        ldr x9, [_tlb, #(-TLB_entries+TLB_mem_changes)]
        ldr x11, [x10, #MMU_changes]
        cmp x9, x11
        b.ne slow_write_\id
        ldrb w10, [x10, #MMU_requires_write_revalidate]
        cbz w10, fast_write_\id
slow_write_\id :
        bl resolve_write_ptr
        b back_\id
fast_write_\id :
    .endif
    and w9, w7, #0xfffff000
    .ifc \type,write
        str w9, [_tlb, #(-TLB_entries+TLB_dirty_page)]
    .endif
    ubfx x10, x7, #12, #10
    eor x10, x10, x7, lsr #22
    mov w11, #TLB_ENTRY_SIZE
    madd x10, x10, x11, _tlb
    .ifc \type,read
        ldr w11, [x10, #TLB_ENTRY_page]
    .else
        ldr w11, [x10, #TLB_ENTRY_page_if_writable]
    .endif
    cmp w9, w11
    b.ne handle_miss_\id
    ldr x11, [x10, #TLB_ENTRY_data_minus_addr]
    add x7, x11, x7, uxtx
back_\id:
.endm

.macro \type\()_bullshit size, id
handle_miss_\id :
    bl handle_\type\()_miss
    b back_\id
crosspage_load_\id :
    mov x19, #(\size/8)
    bl crosspage_load
    b back_\id
.ifc \type,write
crosspage_store_\id :
    mov x19, #(\size/8)
    bl crosspage_store
    b back_write_done_\id
.endif
.endm

.endr

.macro write_done size, id
    add x9, _cpu, #LOCAL_value
    cmp x9, x7
    b.eq crosspage_store_\id
back_write_done_\id :
.endm

.macro save_c
    stp x0, x1, [sp, -0xa0]!
    stp x2, x3, [sp, 0x10]
    stp x4, x5, [sp, 0x20]
    stp x6, x7, [sp, 0x30]
    stp x8, x9, [sp, 0x40]
    stp x10, x11, [sp, 0x50]
    stp x12, x13, [sp, 0x60]
    stp x14, x15, [sp, 0x70]
    stp x16, x17, [sp, 0x80]
    stp x18, lr, [sp, 0x90]
.endm

.macro restore_c
    ldp x18, lr, [sp, 0x90]
    ldp x16, x17, [sp, 0x80]
    ldp x14, x15, [sp, 0x70]
    ldp x12, x13, [sp, 0x60]
    ldp x10, x11, [sp, 0x50]
    ldp x8, x9, [sp, 0x40]
    ldp x6, x7, [sp, 0x30]
    ldp x4, x5, [sp, 0x20]
    ldp x2, x3, [sp, 0x10]
    ldp x0, x1, [sp], 0xa0
.endm

# vim: ft=gas
