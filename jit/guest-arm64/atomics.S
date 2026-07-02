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
    add _ip, _ip, #8
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
.macro stxr_gadget name, bits, ldinsn, stinsn, reg, valmask
.gadget \name
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x20, x8, #0x1f        // rt (value to store)
    ubfx x22, x8, #8, #5      // rn (31 = SP)
    ubfx x19, x8, #16, #5     // rs (status out)

    add x9, _cpu, #CPU_x0
    cmp x22, #31
    b.eq 1f
    add x26, x9, x22, lsl #3
    ldr x21, [x26]
    b 2f
1:  ldr x21, [_cpu, #CPU_sp]
2:
    ldr x9, [_cpu, #CPU_excl_addr]
    cmp x9, x21
    b.ne stxr_fail_\name

    mov x7, x21
    write_prep \bits, \name
    \ldinsn \reg, [_addr]     // current memory value (write-validated ptr is readable)
    ldr x10, [_cpu, #CPU_excl_val]
    .ifnb \valmask
    and x10, x10, \valmask    // armed value may be from a wider LDXR; compare at THIS width
    .endif
    cmp x17, x10
    b.ne stxr_fail_\name

    add x9, _cpu, #CPU_x0
    mov x17, #0
    cmp x20, #31
    b.eq 3f
    add x26, x9, x20, lsl #3
    ldr x17, [x26]
3:  \stinsn \reg, [_addr]
    write_done \bits, \name
    mov x11, #0               // status: success
    b stxr_done_\name
stxr_fail_\name :
    mov x11, #1               // status: failure
stxr_done_\name :
    mov x9, #-1               // monitor clears on any STXR
    str x9, [_cpu, #CPU_excl_addr]
    cmp x19, #31
    b.eq 4f
    add x9, _cpu, #CPU_x0
    add x26, x9, x19, lsl #3
    str x11, [x26]
4:  gret
    write_bullshit \bits, \name
.endm

// CAS: compare memory against Rs; store Rt on match; Rs always receives
// the observed old value (zero-extended to the access width), matching
// the interpreter and the architecture.
.macro cas_gadget name, bits, ldinsn, stinsn, reg, valmask
.gadget \name
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x20, x8, #0x1f        // rt (new value)
    ubfx x22, x8, #8, #5      // rn (31 = SP)
    ubfx x19, x8, #16, #5     // rs (expected in, old value out)

    add x9, _cpu, #CPU_x0
    cmp x22, #31
    b.eq 1f
    add x26, x9, x22, lsl #3
    ldr x7, [x26]
    b 2f
1:  ldr x7, [_cpu, #CPU_sp]
2:
    write_prep \bits, \name
    \ldinsn \reg, [_addr]     // observed old value, zero-extended
    add x9, _cpu, #CPU_x0
    mov x10, #0
    cmp x19, #31
    b.eq 3f
    add x26, x9, x19, lsl #3
    ldr x10, [x26]            // expected
3:  .ifnb \valmask
    and x10, x10, \valmask
    .endif
    cmp x17, x10
    b.ne cas_done_\name
    mov x11, #0
    cmp x20, #31
    b.eq 4f
    add x26, x9, x20, lsl #3
    ldr x11, [x26]            // new value
4:  mov x21, x17              // x17 doubles as the store staging register
                              // below; the observed old value must survive
    mov x17, x11
    \stinsn \reg, [_addr]
    write_done \bits, \name
    mov x17, x21
cas_done_\name :
    cmp x19, #31
    b.eq 5f
    add x9, _cpu, #CPU_x0
    add x26, x9, x19, lsl #3
    str x17, [x26]            // old value out, zero-extended
5:  gret
    write_bullshit \bits, \name
.endm

ldxr_gadget ldxr8,   8, ldrb, w17
ldxr_gadget ldxr16, 16, ldrh, w17
ldxr_gadget ldxr32, 32, ldr,  w17
ldxr_gadget ldxr64, 64, ldr,  x17

stxr_gadget stxr8,   8, ldrb, strb, w17, #0xff
stxr_gadget stxr16, 16, ldrh, strh, w17, #0xffff
stxr_gadget stxr32, 32, ldr,  str,  w17, #0xffffffff
stxr_gadget stxr64, 64, ldr,  str,  x17

cas_gadget cas8,   8, ldrb, strb, w17, #0xff
cas_gadget cas16, 16, ldrh, strh, w17, #0xffff
cas_gadget cas32, 32, ldr,  str,  w17, #0xffffffff
cas_gadget cas64, 64, ldr,  str,  x17

# vim: ft=gas
