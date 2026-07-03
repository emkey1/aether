// AArch64 guest Logical (immediate) gadget: AND/ORR/EOR/ANDS. Independently
// written, NOT adapted from OpenMinis' equivalent gadgets. The bitmask
// immediate itself (immN:imms:immr -> a 64-bit pattern, ARM's
// "DecodeBitMasks") is decoded once at JIT-compile time by
// gen_step_arm64 calling arm64_decode_bitmask_imm (emu/arch/arm64/
// decode.h, shared with the interpreter) and packed directly into the
// code stream as a plain 64-bit value -- so this gadget never needs to
// reimplement DecodeBitMasks in assembly at all, unlike a real hardware
// decoder. ANDS's NZCV semantics (N,Z from the result; C,V always
// cleared) fall out for free from using the host's own native `ands`
// instruction on this same-architecture host -- no special-casing
// needed, the same reason same-architecture dispatch is fast throughout
// this port.
//
// Code stream: [gadget][rd | rn<<8 | opc<<16 | sf<<24][imm (64-bit)]

#include "gadgets.h"

.text

.gadget logical_imm
    ldr x8, [_ip]
    ldr x11, [_ip, #8]      // imm, already decoded at compile time
    add _ip, _ip, #16

    and x9, x8, #0x1f       // rd
    ubfx x10, x8, #8, #5    // rn
    ubfx x12, x8, #16, #2   // opc: 0=AND,1=ORR,2=EOR,3=ANDS
    ubfx x13, x8, #24, #1   // sf

    add x14, _cpu, #CPU_x0
    cmp x10, #31
    b.eq 1f
    ldr x15, [x14, x10, lsl #3]
    b 2f
1:  mov x15, #0             // XZR
2:
    cmp x12, #0
    b.eq logical_imm_and
    cmp x12, #1
    b.eq logical_imm_orr
    cmp x12, #2
    b.eq logical_imm_eor

    // ANDS
    cbz x13, logical_imm_ands32
    ands x16, x15, x11
    b logical_imm_store_flags
logical_imm_ands32:
    ands w16, w15, w11
logical_imm_store_flags:
    store_flags
    b logical_imm_store_result

logical_imm_and:
    cbz x13, logical_imm_and32
    and x16, x15, x11
    b logical_imm_store_result
logical_imm_and32:
    and w16, w15, w11
    b logical_imm_store_result

logical_imm_orr:
    cbz x13, logical_imm_orr32
    orr x16, x15, x11
    b logical_imm_store_result
logical_imm_orr32:
    orr w16, w15, w11
    b logical_imm_store_result

logical_imm_eor:
    cbz x13, logical_imm_eor32
    eor x16, x15, x11
    b logical_imm_store_result
logical_imm_eor32:
    eor w16, w15, w11

logical_imm_store_result:
    // Not SP-capable (unlike ADD/SUB immediate) -- Rd=31 is always XZR
    // discard here, never SP.
    cmp x9, #31
    b.eq 3f
    str x16, [x14, x9, lsl #3]
3:  gret

# vim: ft=gas
