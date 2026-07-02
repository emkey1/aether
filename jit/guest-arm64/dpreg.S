// AArch64 guest Data-Processing (register) gadgets: Logical (shifted
// register) and Add/subtract (shifted register). Independently written,
// not adapted from OpenMinis' equivalents.
//
// Unlike the immediate forms, the shifted OPERAND's value (Rm) is only
// known at runtime -- but the shift type and amount are still compile-time
// constants (encoded in the instruction), just not usable as immediate
// operands to a shared, reused gadget body. Rather than reimplementing
// LSL/LSR/ASR/ROR as generic runtime bit-twiddling, this uses AArch64's
// own *register-form* shift instructions (`lsl/lsr/asr/ror xD, xN, xM`,
// shift amount in a register, not an immediate) -- another instance of
// same-architecture dispatch letting the host CPU do the work natively
// instead of software-emulating it.
//
// Both gadgets always treat register 31 as XZR for Rd/Rn/Rm (never SP) --
// unlike the immediate forms (ADD/SUB immediate, which alias to SP for
// Rn=31/Rd=31), the *shifted-register* encoding of add/subtract does not
// support SP at all; SP is only reachable via the immediate or
// extended-register forms. Confirmed against the ARM ARM before writing
// this, not discovered by a failing test -- the LOCAL_value/x19 register-
// convention bugs from the LDP/STP gadgets (memory.S) made it clear this
// port needs to double-check ISA operand-register semantics up front
// rather than assume symmetry with the immediate forms.
//
// Code stream (both gadgets): [gadget][packed fields, one 64-bit word]
//   rd | rn<<5 | rm<<10 | shift_type<<15 | shift_amount<<17 | sf<<23 |
//   opc<<24 (2 bits for logical, 1 bit for add/sub) | N_or_S<<26

#include "gadgets.h"

.text

// ---- Logical (shifted register): AND/ORR/EOR/ANDS, with N=1 giving the
// NOT variants (BIC/ORN/EON/BICS). ORR with Rn=XZR and shift_amount=0 is
// the "MOV (register)" alias -- handled for free, no special-casing, same
// pattern as the immediate form's MOV-alias.
.gadget logical_reg
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x9, x8, #0x1f          // rd
    ubfx x10, x8, #5, #5       // rn
    ubfx x11, x8, #10, #5      // rm
    ubfx x12, x8, #15, #2      // shift_type: 0=LSL,1=LSR,2=ASR,3=ROR
    ubfx x13, x8, #17, #6      // shift amount
    ubfx x14, x8, #23, #1      // sf
    ubfx x15, x8, #24, #2      // opc: 0=AND,1=ORR,2=EOR,3=ANDS
    ubfx x16, x8, #26, #1      // N (invert shifted Rm before the op)

    add x17, _cpu, #CPU_x0
    cmp x10, #31
    b.eq 1f
    ldr x22, [x17, x10, lsl #3]
    b 2f
1:  mov x22, #0
2:
    cmp x11, #31
    b.eq 3f
    ldr x19, [x17, x11, lsl #3]
    b 4f
3:  mov x19, #0
4:
    cbz x14, logical_reg_shift32
    cmp x12, #0
    b.eq logical_reg_lsl64
    cmp x12, #1
    b.eq logical_reg_lsr64
    cmp x12, #2
    b.eq logical_reg_asr64
    ror x20, x19, x13
    b logical_reg_shifted
logical_reg_lsl64:
    lsl x20, x19, x13
    b logical_reg_shifted
logical_reg_lsr64:
    lsr x20, x19, x13
    b logical_reg_shifted
logical_reg_asr64:
    asr x20, x19, x13
    b logical_reg_shifted
logical_reg_shift32:
    cmp x12, #0
    b.eq logical_reg_lsl32
    cmp x12, #1
    b.eq logical_reg_lsr32
    cmp x12, #2
    b.eq logical_reg_asr32
    ror w20, w19, w13
    b logical_reg_shifted
logical_reg_lsl32:
    lsl w20, w19, w13
    b logical_reg_shifted
logical_reg_lsr32:
    lsr w20, w19, w13
    b logical_reg_shifted
logical_reg_asr32:
    asr w20, w19, w13
logical_reg_shifted:
    // mvn always operates on the full 64-bit x20; harmless when sf=0
    // since every downstream consumer of a 32-bit result reads only the
    // low 32 bits via w-register operations, which mask automatically.
    cbz x16, logical_reg_noinvert
    mvn x20, x20
logical_reg_noinvert:

    cmp x15, #0
    b.eq logical_reg_and
    cmp x15, #1
    b.eq logical_reg_orr
    cmp x15, #2
    b.eq logical_reg_eor
    // ANDS / BICS
    cbz x14, logical_reg_ands32
    ands x21, x22, x20
    b logical_reg_store_flags
logical_reg_ands32:
    ands w21, w22, w20
logical_reg_store_flags:
    store_flags
    // Real bug caught by testing (host EXC_BAD_ACCESS in busybox's
    // ld.so, not by the hand-written smoke tests, which never exercised
    // a flag-setting shifted-register op): store_flags uses x17 as its
    // scratch (gadgets.h), destroying the register-file base this gadget
    // parked there -- the exact "if a future gadget needs x17 live
    // across a flags macro, revisit" hazard gadgets.h's comment called
    // out. Re-derive the base after every store_flags in this file.
    add x17, _cpu, #CPU_x0
    b logical_reg_store
logical_reg_and:
    cbz x14, logical_reg_and32
    and x21, x22, x20
    b logical_reg_store
logical_reg_and32:
    and w21, w22, w20
    b logical_reg_store
logical_reg_orr:
    cbz x14, logical_reg_orr32
    orr x21, x22, x20
    b logical_reg_store
logical_reg_orr32:
    orr w21, w22, w20
    b logical_reg_store
logical_reg_eor:
    cbz x14, logical_reg_eor32
    eor x21, x22, x20
    b logical_reg_store
logical_reg_eor32:
    eor w21, w22, w20
logical_reg_store:
    cmp x9, #31
    b.eq 5f
    str x21, [x17, x9, lsl #3]
5:  gret

// ---- Add/subtract (shifted register): ADD/SUB/ADDS/SUBS. Same shift
// handling as logical_reg above; no N/invert field (that's logical-only).
.gadget addsub_reg
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x9, x8, #0x1f          // rd
    ubfx x10, x8, #5, #5       // rn
    ubfx x11, x8, #10, #5      // rm
    ubfx x12, x8, #15, #2      // shift_type
    ubfx x13, x8, #17, #6      // shift amount
    ubfx x14, x8, #23, #1      // sf
    ubfx x15, x8, #24, #1      // op: 0=add,1=sub
    ubfx x16, x8, #26, #1      // S (set flags)

    add x17, _cpu, #CPU_x0
    cmp x10, #31
    b.eq 1f
    ldr x22, [x17, x10, lsl #3]
    b 2f
1:  mov x22, #0
2:
    cmp x11, #31
    b.eq 3f
    ldr x19, [x17, x11, lsl #3]
    b 4f
3:  mov x19, #0
4:
    cbz x14, addsub_reg_shift32
    cmp x12, #0
    b.eq addsub_reg_lsl64
    cmp x12, #1
    b.eq addsub_reg_lsr64
    cmp x12, #2
    b.eq addsub_reg_asr64
    ror x20, x19, x13
    b addsub_reg_shifted
addsub_reg_lsl64:
    lsl x20, x19, x13
    b addsub_reg_shifted
addsub_reg_lsr64:
    lsr x20, x19, x13
    b addsub_reg_shifted
addsub_reg_asr64:
    asr x20, x19, x13
    b addsub_reg_shifted
addsub_reg_shift32:
    cmp x12, #0
    b.eq addsub_reg_lsl32
    cmp x12, #1
    b.eq addsub_reg_lsr32
    cmp x12, #2
    b.eq addsub_reg_asr32
    ror w20, w19, w13
    b addsub_reg_shifted
addsub_reg_lsl32:
    lsl w20, w19, w13
    b addsub_reg_shifted
addsub_reg_lsr32:
    lsr w20, w19, w13
    b addsub_reg_shifted
addsub_reg_asr32:
    asr w20, w19, w13
addsub_reg_shifted:

    cbz x16, addsub_reg_no_flags
    cbz x14, addsub_reg_flags32
    cbz x15, addsub_reg_adds64
    subs x21, x22, x20
    b addsub_reg_store_flags
addsub_reg_adds64:
    adds x21, x22, x20
    b addsub_reg_store_flags
addsub_reg_flags32:
    cbz x15, addsub_reg_adds32
    subs w21, w22, w20
    b addsub_reg_store_flags
addsub_reg_adds32:
    adds w21, w22, w20
addsub_reg_store_flags:
    store_flags
    add x17, _cpu, #CPU_x0    // store_flags clobbers x17 (see logical_reg above)
    b addsub_reg_store_result

addsub_reg_no_flags:
    cbz x14, addsub_reg_noflag32
    cbz x15, addsub_reg_add64
    sub x21, x22, x20
    b addsub_reg_store_result
addsub_reg_add64:
    add x21, x22, x20
    b addsub_reg_store_result
addsub_reg_noflag32:
    cbz x15, addsub_reg_add32
    sub w21, w22, w20
    b addsub_reg_store_result
addsub_reg_add32:
    add w21, w22, w20

addsub_reg_store_result:
    cmp x9, #31
    b.eq 6f
    str x21, [x17, x9, lsl #3]
6:  gret

# vim: ft=gas

// ---- Add/subtract (extended register): ADD/SUB/ADDS/SUBS with an
// extended+shifted Rm operand. Unlike the shifted-register form above,
// Rn and Rd ARE SP-capable here (Rd only when S=0; ADDS/SUBS with Rd=31
// discards to ZR) -- this is THE encoding compilers use for
// `sub sp, sp, x2` style dynamic stack adjustment, which is exactly the
// instruction (0xcb2263ff in Alpine busybox) that motivated adding it.
// Rm is always ZR for 31.
//
// Code stream: [gadget][rd | rn<<5 | rm<<10 | option<<15 | imm3<<18 |
//                       sf<<21 | op<<22 | S<<23]
// option: bit2 = signed extend, bits1:0 = width (0=B, 1=H, 2=W, 3=X).
.gadget addsub_ext
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x9, x8, #0x1f          // rd
    ubfx x10, x8, #5, #5       // rn
    ubfx x11, x8, #10, #5      // rm
    ubfx x12, x8, #15, #3      // option
    ubfx x13, x8, #18, #3      // imm3 (left shift, 0-4)
    ubfx x14, x8, #21, #1      // sf
    ubfx x15, x8, #22, #1      // op: 0=add, 1=sub
    ubfx x16, x8, #23, #1      // S (set flags)

    add x17, _cpu, #CPU_x0
    // rn: SP-capable
    cmp x10, #31
    b.eq 1f
    ldr x22, [x17, x10, lsl #3]
    b 2f
1:  ldr x22, [_cpu, #CPU_sp]
2:  // rm: always ZR for 31
    cmp x11, #31
    b.eq 3f
    ldr x19, [x17, x11, lsl #3]
    b 4f
3:  mov x19, #0
4:  // extend rm per option
    and x20, x12, #3
    cmp x20, #0
    b.eq ext_b
    cmp x20, #1
    b.eq ext_h
    cmp x20, #2
    b.eq ext_w
    b ext_done                 // width X: SXTX/UXTX are both identity
ext_b:
    tbnz x12, #2, 5f
    and x19, x19, #0xff
    b ext_done
5:  sxtb x19, w19
    b ext_done
ext_h:
    tbnz x12, #2, 6f
    and x19, x19, #0xffff
    b ext_done
6:  sxth x19, w19
    b ext_done
ext_w:
    tbnz x12, #2, 7f
    mov w19, w19               // UXTW
    b ext_done
7:  sxtw x19, w19
ext_done:
    lsl x19, x19, x13

    cbz x16, addsub_ext_no_flags
    cbz x14, addsub_ext_flags32
    cbz x15, 8f
    subs x21, x22, x19
    b addsub_ext_store_flags
8:  adds x21, x22, x19
    b addsub_ext_store_flags
addsub_ext_flags32:
    cbz x15, 9f
    subs w21, w22, w19
    b addsub_ext_store_flags
9:  adds w21, w22, w19
addsub_ext_store_flags:
    store_flags
    add x17, _cpu, #CPU_x0    // store_flags clobbers x17 (see logical_reg above)
    // Flag-setting variant: Rd=31 is ZR (discard), never SP
    cmp x9, #31
    b.eq addsub_ext_out
    str x21, [x17, x9, lsl #3]
    b addsub_ext_out

addsub_ext_no_flags:
    cbz x14, addsub_ext_noflag32
    cbz x15, 10f
    sub x21, x22, x19
    b addsub_ext_store
10: add x21, x22, x19
    b addsub_ext_store
addsub_ext_noflag32:
    cbz x15, 11f
    sub w21, w22, w19
    b addsub_ext_store
11: add w21, w22, w19
addsub_ext_store:
    // Non-flag-setting variant: Rd=31 is SP
    cmp x9, #31
    b.eq 12f
    str x21, [x17, x9, lsl #3]
    b addsub_ext_out
12: str x21, [_cpu, #CPU_sp]
addsub_ext_out:
    gret
