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

// CASP (LSE compare-and-swap pair): host-atomic 2*sz-byte CAS via the
// arm64_casp C helper (emu/tlb.c) — same shape as the cas gadget above,
// with the pair operands marshalled through a stack scratch block because
// four 64-bit values plus two result slots don't fit the register-only
// C ABI. Rs/Rs+1 always receive the observed old pair (there is no
// status register; the LL/SC-style status is CAS-implicit). gen_step_arm64
// guarantees Rs and Rt are even (odd encodings are UNDEFINED), so Rs/Rt
// themselves are never 31; Rs+1/Rt+1 can be 31 only for Rs/Rt = 30, where
// register 31 is XZR (reads 0, writes discarded).
// [gadget][rt | rn<<8 | rs<<16][orig_ip]
.macro casp_gadget name, sizelog
.gadget \name
    ldr x8, [_ip]
    ldr x19, [_ip, #8]       // orig_ip (fault restart), callee-saved
    add _ip, _ip, #16
    and x20, x8, #0x1f       // rt (desired lo; rt+1 = desired hi)
    ubfx x22, x8, #8, #5     // rn (31 = SP)
    ubfx x23, x8, #16, #5    // rs (expected in / old out; rs+1 = hi)

    add x9, _cpu, #CPU_x0
    cmp x22, #31
    b.eq 1f
    add x10, x9, x22, lsl #3
    ldr x21, [x10]           // guest address
    b 2f
1:  ldr x21, [_cpu, #CPU_sp]
2:
    ldr x24, [x9, x23, lsl #3]  // expected lo (Rs, even, never 31)
    add x11, x23, #1
    mov x25, #0
    cmp x11, #31
    b.eq 3f
    ldr x25, [x9, x11, lsl #3]  // expected hi (Rs+1; 31 = XZR)
3:
    ldr x26, [x9, x20, lsl #3]  // desired lo (Rt, even, never 31)
    add x11, x20, #1
    mov x12, #0
    cmp x11, #31
    b.eq 4f
    ldr x12, [x9, x11, lsl #3]  // desired hi (Rt+1; 31 = XZR)
4:
    sub sp, sp, #64          // scratch (survives save_c's 0xa0 frame):
                             // [0]=old_lo [8]=old_hi [16]=swapped (unused)
                             // [32]=exp_lo [40]=exp_hi [48]=des_lo [56]=des_hi
    stp x24, x25, [sp, #32]
    stp x26, x12, [sp, #48]
    save_c
    mov x0, x1               // cpu (x1 still intact)
    sub x1, x2, #TLB_entries // struct tlb *
    mov x2, x21              // addr
    mov x3, #(1 << \sizelog) // sz: bytes per register half
    add x4, sp, #(0xa0+32)   // expected[2]
    add x5, sp, #(0xa0+48)   // desired[2]
    add x6, sp, #0xa0        // old_out[2]
    add x7, sp, #(0xa0+16)   // &swapped
    bl NAME(arm64_casp)
    mov x27, x0              // result (0 / INT_PF / INT_GPF)
    restore_c
    ldp x8, x9, [sp]         // observed old pair
    add sp, sp, #64
    cbnz x27, casp_fault_\name
    add x10, _cpu, #CPU_x0
    str x8, [x10, x23, lsl #3]  // old lo -> Rs
    add x11, x23, #1
    cmp x11, #31                // Rs+1 = 31 (Rs = 30) discards the hi half
    b.eq casp_done_\name
    str x9, [x10, x11, lsl #3]  // old hi -> Rs+1
casp_done_\name:
    gret
casp_fault_\name:
    str x19, [_cpu, #CPU_pc]
    mov w0, w27
    b jit_exit
.endm

casp_gadget casp32, 2
casp_gadget casp64, 3

// LDXP (load exclusive pair): load {lo, hi} and arm the monitor, all via
// the arm64_ldxp C helper (emu/tlb.c) — the helper does the alignment
// check, the (possibly torn; see its header comment) pair load, AND arms
// excl_addr/excl_val/excl_val_hi itself, so the gadget only unpacks the
// two results into Rt/Rt2 (31 = discard; Rt==Rt2 is CONSTRAINED
// UNPREDICTABLE and simply stores hi last). LDAXP's acquire bit (o0) is
// accepted and ignored, same as every other atomic gadget here.
// [gadget][rt | rn<<8 | rt2<<16][orig_ip]
.macro ldxp_gadget name, sizelog
.gadget \name
    ldr x8, [_ip]
    ldr x19, [_ip, #8]       // orig_ip (fault restart), callee-saved
    add _ip, _ip, #16
    and x20, x8, #0x1f       // rt (lo out)
    ubfx x22, x8, #8, #5     // rn (31 = SP)
    ubfx x23, x8, #16, #5    // rt2 (hi out)

    add x9, _cpu, #CPU_x0
    cmp x22, #31
    b.eq 1f
    add x10, x9, x22, lsl #3
    ldr x21, [x10]           // guest address
    b 2f
1:  ldr x21, [_cpu, #CPU_sp]
2:
    sub sp, sp, #16          // val_out[2] (survives save_c's 0xa0 frame)
    save_c
    mov x0, x1               // cpu (x1 still intact)
    sub x1, x2, #TLB_entries // struct tlb *
    mov x2, x21              // addr
    mov x3, #(1 << \sizelog) // sz: bytes per register half
    add x4, sp, #0xa0        // val_out[2]
    bl NAME(arm64_ldxp)
    mov x27, x0              // result (0 / INT_PF / INT_GPF)
    restore_c
    ldp x8, x9, [sp]         // loaded pair {lo, hi}
    add sp, sp, #16
    cbnz x27, ldxp_pf_\name
    add x10, _cpu, #CPU_x0
    cmp x20, #31             // Rt = 31 discards the lo half
    b.eq 3f
    str x8, [x10, x20, lsl #3]
3:  cmp x23, #31             // Rt2 = 31 discards the hi half
    b.eq 4f
    str x9, [x10, x23, lsl #3]
4:  gret
ldxp_pf_\name:
    str x19, [_cpu, #CPU_pc]
    mov w0, w27
    b jit_exit
.endm

ldxp_gadget ldxp32, 2
ldxp_gadget ldxp64, 3

// STXP (store exclusive pair): the arm64_stxp C helper (emu/tlb.c) does
// the whole monitor-check + host-atomic pair CAS + monitor-clear; the
// gadget marshals the desired pair from Rt/Rt2 (31 = XZR = 0) and writes
// the returned status to Rs (0 = stored, 1 = failed; 31 = discard).
// STLXP's release bit (o0) is accepted and ignored. On a fault the
// monitor is preserved (helper contract) so the restarted instruction
// can succeed after e.g. a COW break — same as the stxr gadget.
// [gadget][rt | rn<<8 | rs<<16 | rt2<<24][orig_ip]
.macro stxp_gadget name, sizelog
.gadget \name
    ldr x8, [_ip]
    ldr x19, [_ip, #8]       // orig_ip (fault restart), callee-saved
    add _ip, _ip, #16
    and x20, x8, #0x1f       // rt (desired lo)
    ubfx x22, x8, #8, #5     // rn (31 = SP)
    ubfx x23, x8, #16, #5    // rs (status out)
    ubfx x24, x8, #24, #5    // rt2 (desired hi)

    add x9, _cpu, #CPU_x0
    cmp x22, #31
    b.eq 1f
    add x10, x9, x22, lsl #3
    ldr x21, [x10]           // guest address
    b 2f
1:  ldr x21, [_cpu, #CPU_sp]
2:
    mov x25, #0              // desired lo (Rt; 31 = XZR = 0)
    cmp x20, #31
    b.eq 3f
    ldr x25, [x9, x20, lsl #3]
3:  mov x26, #0              // desired hi (Rt2; 31 = XZR = 0)
    cmp x24, #31
    b.eq 4f
    ldr x26, [x9, x24, lsl #3]
4:
    sub sp, sp, #16          // [0]=status out (survives save_c's frame)
    save_c
    mov x0, x1               // cpu (x1 still intact)
    sub x1, x2, #TLB_entries // struct tlb *
    mov x2, x21              // addr
    mov x3, #(1 << \sizelog) // sz: bytes per register half
    mov x4, x25              // desired lo
    mov x5, x26              // desired hi
    add x6, sp, #0xa0        // &status
    bl NAME(arm64_stxp)
    mov x27, x0              // result (0 / INT_PF / INT_GPF)
    restore_c
    ldr w8, [sp]             // status (0 = stored, 1 = failed)
    add sp, sp, #16
    cbnz x27, stxp_pf_\name
    cmp x23, #31             // Rs = 31 discards the status
    b.eq 5f
    add x9, _cpu, #CPU_x0
    str x8, [x9, x23, lsl #3]
5:  gret
stxp_pf_\name:
    str x19, [_cpu, #CPU_pc]
    mov w0, w27
    b jit_exit
.endm

stxp_gadget stxp32, 2
stxp_gadget stxp64, 3
