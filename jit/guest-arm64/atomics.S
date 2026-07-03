// AArch64 guest atomic gadgets: LDXR/STXR (load/store exclusive) and CAS
// (LSE compare-and-swap). Independently written, not adapted from
// OpenMinis' equivalents. Semantics deliberately mirror this codebase's
// own interpreter handlers (emu/arm64_interp.c's exclusive/CAS blocks)
// exactly, including their documented limitations:
//
//   - The exclusive monitor is emulated via cpu->arm64_excl_addr/
//     arm64_excl_val (single-guest-thread-step semantics), NOT via host
//     ldxr/stxr. Host exclusives would be unsound here: gadget bodies
//     store to cpu_state between the guest's LDXR and STXR, and a host
//     store by the same PE is architecturally allowed to clear the local
//     monitor -- a livelock risk, not a correctness optimization.
//   - STXR succeeds iff the monitor address matches AND the memory value
//     still equals the value read by LDXR (a CAS-style check, same as
//     the interpreter). Cross-thread atomicity of the final store is not
//     modeled -- the same "differential-testing follow-up" caveat as the
//     interpreter's, inherited rather than widened.
//   - Acquire/release bits (LDAXR/STLXR's o0, CAS's A/R) are accepted
//     and ignored, matching the interpreter's decode: barriers aren't
//     modeled. (Non-exclusive LDAR/STLR never reach these gadgets at
//     all -- gen_step_arm64 lowers them to the plain single-register
//     load/store gadgets in memory.S, which is exactly their semantics
//     minus the unmodeled barrier.)
//
// Naturally-aligned atomics can never straddle a page (size <= 8 and the
// prep macros' boundary check is end-exclusive), so the crosspage paths
// below are only reachable for architecturally-misaligned atomics, which
// real hardware faults on anyway; like the interpreter, no alignment
// check is performed, and a misaligned atomic just takes the (slow but
// functionally consistent) crosspage path.
//
// Code stream: [gadget][rt | rn<<8 | rs<<16]. rs is unused by LDXR
// (gen_step_arm64 packs it anyway; the field is ignored here).

#include "gadgets.h"

.text

// LDXR: load, then arm the monitor with the address and observed value.
.macro ldxr_gadget name, bits, ldinsn, reg
.gadget \name
    ldr x8, [_ip]
    add _ip, _ip, #16         // params + trailing orig_ip (fault restart)
    and x20, x8, #0x1f        // rt
    ubfx x22, x8, #8, #5      // rn (31 = SP)

    add x9, _cpu, #CPU_x0
    cmp x22, #31
    b.eq 1f
    add x26, x9, x22, lsl #3
    ldr x21, [x26]
    b 2f
1:  ldr x21, [_cpu, #CPU_sp]
2:  mov x7, x21               // keep the guest address in x21: read_prep
                              // translates x7 in place, and the monitor
                              // must record the GUEST address
    read_prep \bits, \name
    \ldinsn \reg, [_addr]
    add x9, _cpu, #CPU_x0
    cmp x20, #31
    b.eq 3f
    add x26, x9, x20, lsl #3
    str x17, [x26]
3:  str x21, [_cpu, #CPU_excl_addr]
    str x17, [_cpu, #CPU_excl_val]
    gret
    read_bullshit \bits, \name
.endm

// STXR: fails fast (status 1, monitor cleared) if the monitor isn't
// armed for this exact address; otherwise re-reads memory through the
// write-validated pointer, compares against the armed value, and stores
// only on match. Rs gets 0 on success, 1 on failure. The monitor clears
// on every STXR, success or not (architected behavior, and what the
// interpreter does).
// STXR: host-atomic store-conditional. Fails fast (status 1) if the
// monitor isn't armed for this guest address; otherwise attempts a host
// CAS(addr: excl_val -> Rt) via arm64_cas (emu/tlb.c). A won CAS is
// STXR success (status 0); a lost CAS (another agent changed memory)
// is failure, so the guest LL/SC loop retries. Replaces the old
// load-compare-store, which raced (two threads could both pass the
// compare and both store, losing an update) -- the arm64/atomics64.c
// stress mix and a threaded atomic counter both exposed it.
// [gadget][rt | rn<<8 | rs<<16][orig_ip]
.macro stxr_gadget name, sizelog
.gadget \name
    ldr x8, [_ip]
    ldr x19, [_ip, #8]       // orig_ip (fault restart), callee-saved
    add _ip, _ip, #16
    and x20, x8, #0x1f       // rt (value to store)
    ubfx x22, x8, #8, #5     // rn (31 = SP)
    ubfx x23, x8, #16, #5    // rs (status out)

    add x9, _cpu, #CPU_x0
    cmp x22, #31
    b.eq 1f
    add x10, x9, x22, lsl #3
    ldr x21, [x10]           // guest address
    b 2f
1:  ldr x21, [_cpu, #CPU_sp]
2:
    ldr x9, [_cpu, #CPU_excl_addr]
    cmp x9, x21
    b.ne stxr_fail_\name

    ldr x24, [_cpu, #CPU_excl_val] // expected (typed CAS truncates to width)
    mov x25, #0                    // desired (Rt), 31 = XZR = 0
    cmp x20, #31
    b.eq 3f
    add x10, _cpu, #CPU_x0
    add x10, x10, x20, lsl #3
    ldr x25, [x10]
3:
    mov x26, #(1 << \sizelog)      // size_bytes
    sub sp, sp, #16                // [sp]=old_out, [sp+8]=swapped
    save_c
    mov x3, x26
    mov x4, x24
    mov x5, x25
    add x6, sp, #0xa0
    add x7, sp, #0xa8
    mov x0, x1
    sub x1, x2, #TLB_entries
    mov x2, x21
    bl NAME(arm64_cas)
    mov x27, x0                    // result (0 / INT_PF)
    restore_c
    ldr w9, [sp, #8]               // swapped
    add sp, sp, #16
    cbnz x27, stxr_pf_\name
    eor x11, x9, #1                // swapped=1 -> status 0 (success); 0 -> 1
    b stxr_status_\name
stxr_fail_\name:
    mov x11, #1
stxr_status_\name:
    mov x9, #-1                    // monitor clears on any STXR
    str x9, [_cpu, #CPU_excl_addr]
    cmp x23, #31
    b.eq 4f
    add x9, _cpu, #CPU_x0
    add x9, x9, x23, lsl #3
    str x11, [x9]
4:  gret
stxr_pf_\name:
    str x19, [_cpu, #CPU_pc]
    mov w0, w27
    b jit_exit
.endm

ldxr_gadget ldxr8,   8, ldrb, w17
ldxr_gadget ldxr16, 16, ldrh, w17
ldxr_gadget ldxr32, 32, ldr,  w17
ldxr_gadget ldxr64, 64, ldr,  x17

stxr_gadget stxr8,  0
stxr_gadget stxr16, 1
stxr_gadget stxr32, 2
stxr_gadget stxr64, 3

// CAS: host-atomic compare-and-swap via arm64_cas. Rs always receives the
// observed old value (zero-extended). Was load-compare-store (same race
// as STXR). [gadget][rt | rn<<8 | rs<<16][orig_ip]
.macro cas_gadget name, sizelog
.gadget \name
    ldr x8, [_ip]
    ldr x19, [_ip, #8]       // orig_ip
    add _ip, _ip, #16
    and x20, x8, #0x1f       // rt (new/desired)
    ubfx x22, x8, #8, #5     // rn (31 = SP)
    ubfx x23, x8, #16, #5    // rs (expected in, old out)

    add x9, _cpu, #CPU_x0
    cmp x22, #31
    b.eq 1f
    add x10, x9, x22, lsl #3
    ldr x21, [x10]           // guest address
    b 2f
1:  ldr x21, [_cpu, #CPU_sp]
2:
    mov x24, #0              // expected (Rs), 31 = XZR
    cmp x23, #31
    b.eq 3f
    add x10, _cpu, #CPU_x0
    add x10, x10, x23, lsl #3
    ldr x24, [x10]
3:
    mov x25, #0             // desired (Rt)
    cmp x20, #31
    b.eq 4f
    add x10, _cpu, #CPU_x0
    add x10, x10, x20, lsl #3
    ldr x25, [x10]
4:
    mov x26, #(1 << \sizelog)
    sub sp, sp, #16
    save_c
    mov x3, x26
    mov x4, x24
    mov x5, x25
    add x6, sp, #0xa0       // &old_out
    add x7, sp, #0xa8       // &swapped (unused)
    mov x0, x1
    sub x1, x2, #TLB_entries
    mov x2, x21
    bl NAME(arm64_cas)
    mov x27, x0
    restore_c
    ldr x8, [sp]           // observed old value
    add sp, sp, #16
    cbnz x27, cas_pf_\name
    cmp x23, #31           // Rs=31 discards
    b.eq cas_done_\name
    add x9, _cpu, #CPU_x0
    str x8, [x9, x23, lsl #3]
cas_done_\name:
    gret
cas_pf_\name:
    str x19, [_cpu, #CPU_pc]
    mov w0, w27
    b jit_exit
.endm

cas_gadget cas8,  0
cas_gadget cas16, 1
cas_gadget cas32, 2
cas_gadget cas64, 3

# vim: ft=gas

// CLREX: drop the exclusive reservation. Was mis-decoded into the barrier
// space (0xd503305f matches the DMB/DSB mask with CRm free), so a signal-
// handler or context-switch CLREX left the monitor armed and a following
// STXR could spuriously SUCCEED — found by arm64/atomics64.c's clrex check.
.gadget clrex
    mov x8, #-1
    str x8, [_cpu, #CPU_excl_addr]
    gret

// LSE atomic RMW (LDADD/LDCLR/LDEOR/LDSET/LDSMAX/LDSMIN/LDUMAX/LDUMIN
// and SWP), via the arm64_lse_rmw C helper (emu/tlb.c) for correct
// width/signedness handling. Same shape as simd.S's ld1st1_multi. The
// operand comes from Rs (31 = XZR = 0), the old value is written to Rt
// (31 = discard). Not host-atomic — matches the CAS/exclusive model.
// [gadget][rt | rn<<8 | rs<<16 | size<<24 | op<<26][orig_ip]
.gadget lse_rmw
    ldr x8, [_ip]
    ldr x19, [_ip, #8]        // orig_ip (fault restart), callee-saved
    add _ip, _ip, #16
    and x20, x8, #0x1f        // rt (old out)
    ubfx x21, x8, #8, #5      // rn
    ubfx x22, x8, #16, #5     // rs (operand)
    ubfx x23, x8, #24, #2     // size log2: 0=B 1=H 2=W 3=X
    ubfx x24, x8, #26, #4     // op

    add x9, _cpu, #CPU_x0
    cmp x21, #31
    b.eq 1f
    add x10, x9, x21, lsl #3
    ldr x7, [x10]            // addr
    b 2f
1:  ldr x7, [_cpu, #CPU_sp]
2:
    mov x25, #0
    cmp x22, #31
    b.eq 3f
    add x10, x9, x22, lsl #3
    ldr x25, [x10]          // operand
3:
    mov x26, #1
    lsl x26, x26, x23       // size_bytes

    sub sp, sp, #16         // reserve old_out slot (survives save_c's frame)
    save_c
    mov x3, x26             // size_bytes
    mov x4, x24             // op
    mov x5, x25             // operand
    add x6, sp, #0xa0       // &old_out (the reserved slot, above the save_c frame)
    mov x0, x1             // cpu  (x1 still intact)
    sub x1, x2, #TLB_entries // struct tlb *
    mov x2, x7             // addr
    bl NAME(arm64_lse_rmw)
    mov x27, x0            // result (0 / INT_PF), callee-saved
    restore_c
    ldr x8, [sp]          // old value
    add sp, sp, #16

    cbnz x27, lse_rmw_fault

    cmp x20, #31          // Rt=31 discards the old value
    b.eq lse_rmw_done
    add x9, _cpu, #CPU_x0
    str x8, [x9, x20, lsl #3] // Wt zero-extends via the size-limited read
lse_rmw_done:
    gret

lse_rmw_fault:
    str x19, [_cpu, #CPU_pc]
    mov w0, w27
    b jit_exit
