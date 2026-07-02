// AArch64 guest AdvSIMD vector-shift-by-immediate completion and
// by-element (indexed) multiplies. Ported from OpenMinis/ish-arm64
// (make_rounding_shift, make_rounding_sra, make_shrn_vec, make_sat_shrn,
// elem_setup/elem_op_store/elem_accum_op_store; GPLv3, see
// /CREDITS-aarch64.md). The same core trick as the existing SSHR/USHR
// lowering applies throughout: host shift-immediate encodings can't take
// a runtime amount, so gen.c broadcasts the (signed) amount and the
// gadgets use the REGISTER-form shifts — negative amounts shift right,
// and the rounding/saturating register forms match the immediate forms'
// semantics exactly. Narrowing shifts shift on the wide arrangement
// (exact) then narrow with xtn/sqxtn/uqxtn/sqxtun. Fixed-point converts
// scale by an exact power-of-two constant computed at compile time.
//
// By-element ops read the selected element from the V file in memory
// (byte offset computed at compile time), dup it, and run the plain
// three-same operation.

#include "gadgets.h"

.text

// ---- rounding shifts (SRSHR/URSHR via negative amounts) ------------------
// Same shape/code stream as simd.S's vshift_gadget: [rd | rn<<8][amount]
.macro vshift2_gadget name, shiftop, arr, dupw
.gadget \name
    ldr x8, [_ip]
    ldr x9, [_ip, #8]
    add _ip, _ip, #16
    and x10, x8, #0x1f
    ubfx x11, x8, #8, #5
    add x12, _cpu, #CPU_v0
    add x13, x12, x11, lsl #4
    ldr q30, [x13]
    dup v29\arr, \dupw
    \shiftop v30\arr, v30\arr, v29\arr
    add x13, x12, x10, lsl #4
    str q30, [x13]
    gret
.endm

vshift2_gadget vrshl_s_8b,  srshl, .8b,  w9
vshift2_gadget vrshl_s_16b, srshl, .16b, w9
vshift2_gadget vrshl_s_4h,  srshl, .4h,  w9
vshift2_gadget vrshl_s_8h,  srshl, .8h,  w9
vshift2_gadget vrshl_s_2s,  srshl, .2s,  w9
vshift2_gadget vrshl_s_4s,  srshl, .4s,  w9
vshift2_gadget vrshl_s_2d,  srshl, .2d,  x9
vshift2_gadget vrshl_u_8b,  urshl, .8b,  w9
vshift2_gadget vrshl_u_16b, urshl, .16b, w9
vshift2_gadget vrshl_u_4h,  urshl, .4h,  w9
vshift2_gadget vrshl_u_8h,  urshl, .8h,  w9
vshift2_gadget vrshl_u_2s,  urshl, .2s,  w9
vshift2_gadget vrshl_u_4s,  urshl, .4s,  w9
vshift2_gadget vrshl_u_2d,  urshl, .2d,  x9
// saturating left shifts by immediate (SQSHL/UQSHL #imm): positive
// amounts through the saturating register forms
vshift2_gadget vqshli_s_8b,  sqshl, .8b,  w9
vshift2_gadget vqshli_s_16b, sqshl, .16b, w9
vshift2_gadget vqshli_s_4h,  sqshl, .4h,  w9
vshift2_gadget vqshli_s_8h,  sqshl, .8h,  w9
vshift2_gadget vqshli_s_2s,  sqshl, .2s,  w9
vshift2_gadget vqshli_s_4s,  sqshl, .4s,  w9
vshift2_gadget vqshli_s_2d,  sqshl, .2d,  x9
vshift2_gadget vqshli_u_8b,  uqshl, .8b,  w9
vshift2_gadget vqshli_u_16b, uqshl, .16b, w9
vshift2_gadget vqshli_u_4h,  uqshl, .4h,  w9
vshift2_gadget vqshli_u_8h,  uqshl, .8h,  w9
vshift2_gadget vqshli_u_2s,  uqshl, .2s,  w9
vshift2_gadget vqshli_u_4s,  uqshl, .4s,  w9
vshift2_gadget vqshli_u_2d,  uqshl, .2d,  x9

// ---- accumulating shifts (SSRA/USRA/SRSRA/URSRA) --------------------------
// Vd += (Vn >> shift): shift via the register form, then add into Vd.
.macro vsra_gadget name, shiftop, arr, dupw
.gadget \name
    ldr x8, [_ip]
    ldr x9, [_ip, #8]
    add _ip, _ip, #16
    and x10, x8, #0x1f
    ubfx x11, x8, #8, #5
    add x12, _cpu, #CPU_v0
    add x13, x12, x11, lsl #4
    ldr q30, [x13]
    dup v29\arr, \dupw
    \shiftop v30\arr, v30\arr, v29\arr
    add x13, x12, x10, lsl #4
    ldr q28, [x13]             // Vd (accumulator)
    add v28\arr, v28\arr, v30\arr
    str q28, [x13]
    gret
.endm

vsra_gadget vsra_s_8b,  sshl, .8b,  w9
vsra_gadget vsra_s_16b, sshl, .16b, w9
vsra_gadget vsra_s_4h,  sshl, .4h,  w9
vsra_gadget vsra_s_8h,  sshl, .8h,  w9
vsra_gadget vsra_s_2s,  sshl, .2s,  w9
vsra_gadget vsra_s_4s,  sshl, .4s,  w9
vsra_gadget vsra_s_2d,  sshl, .2d,  x9
vsra_gadget vsra_u_8b,  ushl, .8b,  w9
vsra_gadget vsra_u_16b, ushl, .16b, w9
vsra_gadget vsra_u_4h,  ushl, .4h,  w9
vsra_gadget vsra_u_8h,  ushl, .8h,  w9
vsra_gadget vsra_u_2s,  ushl, .2s,  w9
vsra_gadget vsra_u_4s,  ushl, .4s,  w9
vsra_gadget vsra_u_2d,  ushl, .2d,  x9
vsra_gadget vrsra_s_8b,  srshl, .8b,  w9
vsra_gadget vrsra_s_16b, srshl, .16b, w9
vsra_gadget vrsra_s_4h,  srshl, .4h,  w9
vsra_gadget vrsra_s_8h,  srshl, .8h,  w9
vsra_gadget vrsra_s_2s,  srshl, .2s,  w9
vsra_gadget vrsra_s_4s,  srshl, .4s,  w9
vsra_gadget vrsra_s_2d,  srshl, .2d,  x9
vsra_gadget vrsra_u_8b,  urshl, .8b,  w9
vsra_gadget vrsra_u_16b, urshl, .16b, w9
vsra_gadget vrsra_u_4h,  urshl, .4h,  w9
vsra_gadget vrsra_u_8h,  urshl, .8h,  w9
vsra_gadget vrsra_u_2s,  urshl, .2s,  w9
vsra_gadget vrsra_u_4s,  urshl, .4s,  w9
vsra_gadget vrsra_u_2d,  urshl, .2d,  x9

// ---- insert shifts (SLI/SRI) ----------------------------------------------
// result = (shifted_src & shift_mask) | (dst & ~shift_mask); the per-lane
// mask repeats within 64 bits, so one 64-bit pattern from the code stream
// (computed by gen.c from esize+shift) covers all arrangements.
// [gadget][rd | rn<<8][amount][mask64]
.macro vsl_insert name, shiftop, arr, dupw, zhi
.gadget \name
    ldr x8, [_ip]
    ldr x9, [_ip, #8]
    ldr x14, [_ip, #16]        // mask
    add _ip, _ip, #24
    and x10, x8, #0x1f
    ubfx x11, x8, #8, #5
    add x12, _cpu, #CPU_v0
    add x13, x12, x11, lsl #4
    ldr q30, [x13]
    dup v29\arr, \dupw
    \shiftop v30\arr, v30\arr, v29\arr
    dup v27.2d, x14            // selector: 1-bits take the shifted source
    add x13, x12, x10, lsl #4
    ldr q28, [x13]             // Vd
    bsl v27.16b, v30.16b, v28.16b
.if \zhi
    // Q=0 arrangements architecturally zero the upper half; bsl merged
    // Vd's old upper bits in (caught by the differential harness).
    fmov d27, d27
.endif
    str q27, [x13]
    gret
.endm

vsl_insert vsli_8b,  ushl, .8b,  w9, 1
vsl_insert vsli_16b, ushl, .16b, w9, 0
vsl_insert vsli_4h,  ushl, .4h,  w9, 1
vsl_insert vsli_8h,  ushl, .8h,  w9, 0
vsl_insert vsli_2s,  ushl, .2s,  w9, 1
vsl_insert vsli_4s,  ushl, .4s,  w9, 0
vsl_insert vsli_2d,  ushl, .2d,  x9, 0
// SRI shifts right: gen.c passes a negative amount + the right-shift mask.
vsl_insert vsri_8b,  ushl, .8b,  w9, 1
vsl_insert vsri_16b, ushl, .16b, w9, 0
vsl_insert vsri_4h,  ushl, .4h,  w9, 1
vsl_insert vsri_8h,  ushl, .8h,  w9, 0
vsl_insert vsri_2s,  ushl, .2s,  w9, 1
vsl_insert vsri_4s,  ushl, .4s,  w9, 0
vsl_insert vsri_2d,  ushl, .2d,  x9, 0

// ---- narrowing shifts ------------------------------------------------------
// SHRN/RSHRN and the saturating SQSHRN/UQSHRN/SQRSHRN/UQRSHRN/SQSHRUN/
// SQRSHRUN: shift on the WIDE arrangement (register form, exact), then
// narrow. The "2" (Q=1) forms merge into Vd's upper half, so Vd loads
// first. size = narrow element size. [gadget][rd | rn<<8 | size<<16 |
// q<<18][neg_amount]
.macro vshrn_family name, shiftop, xtnop
.gadget \name
    ldr x8, [_ip]
    ldr x9, [_ip, #8]          // negative shift amount (right shift)
    add _ip, _ip, #16
    and x10, x8, #0x1f
    ubfx x11, x8, #8, #5
    ubfx x12, x8, #16, #2      // narrow size: 0=b, 1=h, 2=s
    ubfx x13, x8, #18, #1      // q
    add x14, _cpu, #CPU_v0
    add x15, x14, x10, lsl #4
    ldr q28, [x15]             // Vd (merge target for the 2-forms)
    add x16, x14, x11, lsl #4
    ldr q30, [x16]             // Vn (wide)
    cmp x12, #0
    b.eq \name\()_b
    cmp x12, #1
    b.eq \name\()_h
    dup v29.2d, x9
    \shiftop v30.2d, v30.2d, v29.2d
    cbz x13, 3f
    \xtnop\()2 v28.4s, v30.2d
    b \name\()_st
3:  \xtnop v28.2s, v30.2d
    b \name\()_st
\name\()_b:
    dup v29.8h, w9
    \shiftop v30.8h, v30.8h, v29.8h
    cbz x13, 1f
    \xtnop\()2 v28.16b, v30.8h
    b \name\()_st
1:  \xtnop v28.8b, v30.8h
    b \name\()_st
\name\()_h:
    dup v29.4s, w9
    \shiftop v30.4s, v30.4s, v29.4s
    cbz x13, 2f
    \xtnop\()2 v28.8h, v30.4s
    b \name\()_st
2:  \xtnop v28.4h, v30.4s
\name\()_st:
    str q28, [x15]
    gret
.endm

vshrn_family vshrn,     ushl,  xtn
vshrn_family vrshrn,    urshl, xtn
vshrn_family vsqshrn,   sshl,  sqxtn
vshrn_family vsqrshrn,  srshl, sqxtn
vshrn_family vuqshrn,   ushl,  uqxtn
vshrn_family vuqrshrn,  urshl, uqxtn
vshrn_family vsqshrun,  sshl,  sqxtun
vshrn_family vsqrshrun, srshl, sqxtun

// ---- fixed-point conversions -----------------------------------------------
// SCVTF/UCVTF #fbits: convert, then multiply by the exact 2^-fbits the
// compiler passes as a raw FP bit pattern (power-of-two scaling is exact).
// FCVTZS/FCVTZU #fbits: multiply by exact 2^fbits (also exact — pure
// exponent change; overflow to inf converts to the same saturated int),
// then convert. sz picks .2s/.4s (float pattern) or .2d (double pattern).
// [gadget][rd | rn<<8 | sz<<16 | q<<18][scale_bits]
.macro vcvt_fixed name, cvtop, mul_first
.gadget \name
    ldr x8, [_ip]
    ldr x9, [_ip, #8]
    add _ip, _ip, #16
    and x10, x8, #0x1f
    ubfx x11, x8, #8, #5
    ubfx x12, x8, #16, #1      // sz
    ubfx x13, x8, #18, #1      // q
    add x14, _cpu, #CPU_v0
    add x16, x14, x11, lsl #4
    ldr q30, [x16]
    cbnz x12, \name\()_d
    dup v29.4s, w9
    cbz x13, 1f
.if \mul_first
    fmul v30.4s, v30.4s, v29.4s
    \cvtop v30.4s, v30.4s
.else
    \cvtop v30.4s, v30.4s
    fmul v30.4s, v30.4s, v29.4s
.endif
    b \name\()_st
1:
.if \mul_first
    fmul v30.2s, v30.2s, v29.2s
    \cvtop v30.2s, v30.2s
.else
    \cvtop v30.2s, v30.2s
    fmul v30.2s, v30.2s, v29.2s
.endif
    b \name\()_st
\name\()_d:
    dup v29.2d, x9
.if \mul_first
    fmul v30.2d, v30.2d, v29.2d
    \cvtop v30.2d, v30.2d
.else
    \cvtop v30.2d, v30.2d
    fmul v30.2d, v30.2d, v29.2d
.endif
\name\()_st:
    add x16, x14, x10, lsl #4
    str q30, [x16]
    gret
.endm

vcvt_fixed vscvtf_fix,  scvtf,  0
vcvt_fixed vucvtf_fix,  ucvtf,  0
vcvt_fixed vfcvtzs_fix, fcvtzs, 1
vcvt_fixed vfcvtzu_fix, fcvtzu, 1

// ---- scalar shift-imm completion (D size) ----------------------------------
// SRSHR/URSHR/SSRA/USRA/SRSRA/URSRA on the D scalar; the amount comes
// broadcast like the vector forms. [gadget][rd | rn<<8][amount]
.macro sshift_d name, shiftop, acc
.gadget \name
    ldr x8, [_ip]
    ldr x9, [_ip, #8]
    add _ip, _ip, #16
    and x10, x8, #0x1f
    ubfx x11, x8, #8, #5
    add x12, _cpu, #CPU_v0
    add x13, x12, x11, lsl #4
    ldr d30, [x13]
    fmov d29, x9
    \shiftop d30, d30, d29
    add x13, x12, x10, lsl #4
.if \acc
    ldr d28, [x13]
    add d28, d28, d30
    str q28, [x13]             // scalar write zeroes the upper lane
.else
    str q30, [x13]
.endif
    gret
.endm

sshift_d ssrshr_d, srshl, 0
sshift_d surshr_d, urshl, 0
sshift_d sssra_d,  sshl,  1
sshift_d susra_d,  ushl,  1
sshift_d ssrsra_d, srshl, 1
sshift_d sursra_d, urshl, 1

// Scalar saturating narrows (SQSHRN &c, b/h/s results): shift the wide
// scalar exactly, then saturating-narrow. [gadget][rd | rn<<8 |
// size<<16][neg_amount]
.macro sshrn_family name, shiftop, xtnop
.gadget \name
    ldr x8, [_ip]
    ldr x9, [_ip, #8]
    add _ip, _ip, #16
    and x10, x8, #0x1f
    ubfx x11, x8, #8, #5
    ubfx x12, x8, #16, #2      // narrow size: 0=b, 1=h, 2=s
    add x14, _cpu, #CPU_v0
    add x16, x14, x11, lsl #4
    ldr q30, [x16]
    cmp x12, #0
    b.eq \name\()_b
    cmp x12, #1
    b.eq \name\()_h
    fmov d29, x9
    \shiftop d30, d30, d29
    \xtnop s30, d30
    b \name\()_st
\name\()_b:
    fmov d29, x9
    \shiftop v30.4h, v30.4h, v29.4h
    \xtnop b30, h30
    b \name\()_st
\name\()_h:
    fmov d29, x9
    \shiftop v30.2s, v30.2s, v29.2s
    \xtnop h30, s30
\name\()_st:
    add x16, x14, x10, lsl #4
    str q30, [x16]
    gret
.endm

sshrn_family ssqshrn,   sshl,  sqxtn
sshrn_family ssqrshrn,  srshl, sqxtn
sshrn_family suqshrn,   ushl,  uqxtn
sshrn_family suqrshrn,  urshl, uqxtn
sshrn_family ssqshrun,  sshl,  sqxtun
sshrn_family ssqrshrun, srshl, sqxtun

// Scalar SQSHL/UQSHL #imm (b/h/s/d): broadcast positive amount into the
// matching lane width. [gadget][rd | rn<<8 | size<<16][amount]
.macro sqshli name, shiftop
.gadget \name
    ldr x8, [_ip]
    ldr x9, [_ip, #8]
    add _ip, _ip, #16
    and x10, x8, #0x1f
    ubfx x11, x8, #8, #5
    ubfx x12, x8, #16, #2
    add x14, _cpu, #CPU_v0
    add x16, x14, x11, lsl #4
    ldr q30, [x16]
    fmov d29, x9
    cmp x12, #0
    b.eq \name\()_b
    cmp x12, #1
    b.eq \name\()_h
    cmp x12, #2
    b.eq \name\()_s
    \shiftop d30, d30, d29
    b \name\()_st
\name\()_b:
    \shiftop b30, b30, b29
    b \name\()_st
\name\()_h:
    \shiftop h30, h30, h29
    b \name\()_st
\name\()_s:
    \shiftop s30, s30, s29
\name\()_st:
    add x16, x14, x10, lsl #4
    str q30, [x16]
    gret
.endm

sqshli ssqshli, sqshl
sqshli suqshli, uqshl

// ---- by-element (indexed) multiplies ---------------------------------------
// The element loads from the V file at a compile-time byte offset, gets
// dup'd across the arrangement, then the plain three-same op runs.
// [gadget][rd | rn<<8 | rm<<16 | size<<24 | q<<26 | byteoff<<27]
// size: 1=h, 2=s (int ops); FP uses sz via size 2=s, 3=d.
.macro elem_setup
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x9, x8, #0x1f          // rd
    ubfx x10, x8, #8, #5       // rn
    ubfx x11, x8, #16, #5      // rm
    ubfx x12, x8, #24, #2      // size
    ubfx x13, x8, #26, #1      // q
    ubfx x14, x8, #27, #4      // element byte offset / element size... packed below
    ubfx x15, x8, #27, #7      // byteoff (0-15 valid; 7 bits reserved)
    add x16, _cpu, #CPU_v0
    add x17, x16, x11, lsl #4
    add x17, x17, x15          // &Vm.elem
.endm

// integer H/S by-element: MUL/MLA/MLS + SQDMULH/SQRDMULH
.macro elem_int name, op, acc
.gadget \name
    elem_setup
    cmp x12, #1
    b.eq \name\()_h
    // S lanes
    ldr s31, [x17]
    add x17, x16, x10, lsl #4
    ldr q30, [x17]
.if \acc
    add x17, x16, x9, lsl #4
    ldr q28, [x17]
.endif
    cbz x13, 1f
    dup v31.4s, v31.s[0]
.if \acc
    \op v28.4s, v30.4s, v31.4s
.else
    \op v28.4s, v30.4s, v31.4s
.endif
    b \name\()_st
1:  dup v31.2s, v31.s[0]
    \op v28.2s, v30.2s, v31.2s
    b \name\()_st
\name\()_h:
    ldr h31, [x17]
    add x17, x16, x10, lsl #4
    ldr q30, [x17]
.if \acc
    add x17, x16, x9, lsl #4
    ldr q28, [x17]
.endif
    cbz x13, 2f
    dup v31.8h, v31.h[0]
    \op v28.8h, v30.8h, v31.8h
    b \name\()_st
2:  dup v31.4h, v31.h[0]
    \op v28.4h, v30.4h, v31.4h
\name\()_st:
    add x17, x16, x9, lsl #4
    str q28, [x17]
    gret
.endm

// non-accumulating instances write v28 outright via the op (dest v28)
elem_int velem_mul,      mul,     0
elem_int velem_sqdmulh,  sqdmulh, 0
elem_int velem_sqrdmulh, sqrdmulh, 0
elem_int velem_mla,      mla,     1
elem_int velem_mls,      mls,     1

// widening by-element: S/UMULL, S/UMLAL, S/UMLSL, SQDMULL/SQDMLAL/SQDMLSL
// (H->S / S->D); q picks the source half via the "2" mnemonic.
.macro elem_widen name, op, acc
.gadget \name
    elem_setup
    cmp x12, #1
    b.eq \name\()_h
    ldr s31, [x17]
    add x17, x16, x10, lsl #4
    ldr q30, [x17]
.if \acc
    add x17, x16, x9, lsl #4
    ldr q28, [x17]
.endif
    cbz x13, 1f
    dup v31.4s, v31.s[0]
    \op\()2 v28.2d, v30.4s, v31.4s
    b \name\()_st
1:  dup v31.2s, v31.s[0]
    \op v28.2d, v30.2s, v31.2s
    b \name\()_st
\name\()_h:
    ldr h31, [x17]
    add x17, x16, x10, lsl #4
    ldr q30, [x17]
.if \acc
    add x17, x16, x9, lsl #4
    ldr q28, [x17]
.endif
    cbz x13, 2f
    dup v31.8h, v31.h[0]
    \op\()2 v28.4s, v30.8h, v31.8h
    b \name\()_st
2:  dup v31.4h, v31.h[0]
    \op v28.4s, v30.4h, v31.4h
\name\()_st:
    add x17, x16, x9, lsl #4
    str q28, [x17]
    gret
.endm

elem_widen velem_smull,   smull,   0
elem_widen velem_umull,   umull,   0
elem_widen velem_sqdmull, sqdmull, 0
elem_widen velem_smlal,   smlal,   1
elem_widen velem_umlal,   umlal,   1
elem_widen velem_smlsl,   smlsl,   1
elem_widen velem_umlsl,   umlsl,   1
elem_widen velem_sqdmlal, sqdmlal, 1
elem_widen velem_sqdmlsl, sqdmlsl, 1

// FP by-element: FMUL/FMULX/FMLA/FMLS. size: 2=s, 3=d.
.macro elem_fp name, op, acc
.gadget \name
    elem_setup
    cmp x12, #2
    b.eq \name\()_s
    ldr d31, [x17]
    add x17, x16, x10, lsl #4
    ldr q30, [x17]
.if \acc
    add x17, x16, x9, lsl #4
    ldr q28, [x17]
.endif
    dup v31.2d, v31.d[0]
    \op v28.2d, v30.2d, v31.2d
    b \name\()_st
\name\()_s:
    ldr s31, [x17]
    add x17, x16, x10, lsl #4
    ldr q30, [x17]
.if \acc
    add x17, x16, x9, lsl #4
    ldr q28, [x17]
.endif
    cbz x13, 1f
    dup v31.4s, v31.s[0]
    \op v28.4s, v30.4s, v31.4s
    b \name\()_st
1:  dup v31.2s, v31.s[0]
    \op v28.2s, v30.2s, v31.2s
\name\()_st:
    add x17, x16, x9, lsl #4
    str q28, [x17]
    gret
.endm

elem_fp velem_fmul,  fmul,  0
elem_fp velem_fmulx, fmulx, 0
elem_fp velem_fmla,  fmla,  1
elem_fp velem_fmls,  fmls,  1

// scalar FP by-element (FMUL/FMULX Sd/Dd, Vm.T[i]).
// [gadget][rd | rn<<8 | rm<<16 | size<<24 | byteoff<<27]
.macro selem_fp name, op
.gadget \name
    elem_setup
    cmp x12, #2
    b.eq \name\()_s
    ldr d31, [x17]
    add x17, x16, x10, lsl #4
    ldr d30, [x17]
    \op d28, d30, d31
    b \name\()_st
\name\()_s:
    ldr s31, [x17]
    add x17, x16, x10, lsl #4
    ldr s30, [x17]
    \op s28, s30, s31
\name\()_st:
    add x17, x16, x9, lsl #4
    str q28, [x17]
    gret
.endm

selem_fp selem_fmul,  fmul
selem_fp selem_fmulx, fmulx

// scalar fused FMLA/FMLS by element: scalar NEON has no 3-op fmla form,
// so lower to the (equally fused) fmadd/fmsub. fmsub computes a - n*m,
// which IS the FMLS semantic.
.macro selem_fma name, op
.gadget \name
    elem_setup
    cmp x12, #2
    b.eq \name\()_s
    ldr d31, [x17]
    add x17, x16, x10, lsl #4
    ldr d30, [x17]
    add x17, x16, x9, lsl #4
    ldr d28, [x17]
    \op d28, d30, d31, d28
    b \name\()_st
\name\()_s:
    ldr s31, [x17]
    add x17, x16, x10, lsl #4
    ldr s30, [x17]
    add x17, x16, x9, lsl #4
    ldr s28, [x17]
    \op s28, s30, s31, s28
\name\()_st:
    add x17, x16, x9, lsl #4
    str q28, [x17]
    gret
.endm

selem_fma selem_fmla, fmadd
selem_fma selem_fmls, fmsub

# vim: ft=gas
