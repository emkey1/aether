// AArch64 guest AdvSIMD "three same" gadgets (integer + FP vector).
// Ported from OpenMinis/ish-arm64 gadgets-aarch64/math.S (GPLv3, see
// /CREDITS-aarch64.md): make_vec_shift_three_same, make_threesame_native,
// make_threesame_accum, make_threesame_native_hs, make_vec_three_same_noD,
// make_vec_aba, make_fp_vec_three_same(+_accum) and the expanded add_vec/
// shadd_vec/... bodies. Their file spells these out as several near-identical
// macros plus fully expanded per-op bodies; this port collapses them to two
// integer macros (plain/accumulating) with per-size validity flags and two
// FP macros, with identical logic — dispatch on the (size, Q) arrangement
// at runtime and use the host's own vector instruction.
//
// A 64-bit (Q=0) arrangement zeroes the destination's upper 64 bits on real
// hardware, so after a Q=0 host op `str q0` stores the architecturally
// correct 128 bits with no extra masking. (OpenMinis stores d0 + str xzr,
// same effect.)
//
// Sizes an op doesn't support are assembly-time-guarded; gen_step_arm64
// never emits them (it raises INT_UNDEFINED instead — unlike OpenMinis,
// whose gadgets silently zero Vd on an invalid size).
//
// Code stream: [gadget][rd | rn<<8 | rm<<16 | size<<24 | Q<<26]

#include "gadgets.h"

.text

// Plain three-same: Vd = Vn <op> Vm. bv/hv/sv/dv say which element sizes
// exist for the op (pmul: b only; sqdmulh: h/s; mul/smax/...: b/h/s;
// add/sqadd/sshl/cmXX/...: b/h/s/d).
.macro three_same_int name, op, bv=1, hv=1, sv=1, dv=0
.gadget \name
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x9, x8, #0x1f          // rd
    ubfx x10, x8, #8, #5       // rn
    ubfx x11, x8, #16, #5      // rm
    ubfx x12, x8, #24, #2      // size
    ubfx x13, x8, #26, #1      // q
    add x14, _cpu, #CPU_v0
    add x15, x14, x10, lsl #4
    ldr q0, [x15]
    add x16, x14, x11, lsl #4
    ldr q1, [x16]
.if \bv
    cmp x12, #0
    b.eq \name\()_b
.endif
.if \hv
    cmp x12, #1
    b.eq \name\()_h
.endif
.if \sv
    cmp x12, #2
    b.eq \name\()_s
.endif
.if \dv
    \op v0.2d, v0.2d, v1.2d    // size 3: gen only emits this with Q=1
.endif
    b \name\()_st
.if \bv
\name\()_b:
    cbz x13, 1f
    \op v0.16b, v0.16b, v1.16b
    b \name\()_st
1:  \op v0.8b, v0.8b, v1.8b
    b \name\()_st
.endif
.if \hv
\name\()_h:
    cbz x13, 2f
    \op v0.8h, v0.8h, v1.8h
    b \name\()_st
2:  \op v0.4h, v0.4h, v1.4h
    b \name\()_st
.endif
.if \sv
\name\()_s:
    cbz x13, 3f
    \op v0.4s, v0.4s, v1.4s
    b \name\()_st
3:  \op v0.2s, v0.2s, v1.2s
.endif
\name\()_st:
    add x15, x14, x9, lsl #4
    str q0, [x15]
    gret
.endm

// Accumulating three-same: Vd = Vd <op-combine> (Vn, Vm) — MLA/MLS/SABA/
// UABA. The destination is a THIRD source, so load it into the op's
// accumulator (v0) and the sources into v1/v2. For a Q=0 arrangement the
// host op reads only v0's low half and zeroes the result's upper 64 bits,
// which matches the architecture. No d size exists for any of these.
.macro three_same_acc name, op
.gadget \name
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x9, x8, #0x1f          // rd
    ubfx x10, x8, #8, #5       // rn
    ubfx x11, x8, #16, #5      // rm
    ubfx x12, x8, #24, #2      // size
    ubfx x13, x8, #26, #1      // q
    add x14, _cpu, #CPU_v0
    add x15, x14, x9, lsl #4
    ldr q0, [x15]              // Vd (accumulator)
    add x16, x14, x10, lsl #4
    ldr q1, [x16]              // Vn
    add x16, x14, x11, lsl #4
    ldr q2, [x16]              // Vm
    cmp x12, #0
    b.eq \name\()_b
    cmp x12, #1
    b.eq \name\()_h
    cbz x13, 3f
    \op v0.4s, v1.4s, v2.4s
    b \name\()_st
3:  \op v0.2s, v1.2s, v2.2s
    b \name\()_st
\name\()_b:
    cbz x13, 1f
    \op v0.16b, v1.16b, v2.16b
    b \name\()_st
1:  \op v0.8b, v1.8b, v2.8b
    b \name\()_st
\name\()_h:
    cbz x13, 2f
    \op v0.8h, v1.8h, v2.8h
    b \name\()_st
2:  \op v0.4h, v1.4h, v2.4h
\name\()_st:
    // Q=0: the host op zeroed v0's upper 64 bits (its dest was a 64-bit
    // arrangement), so storing q0 is correct for both Q values.
    str q0, [x15]
    gret
.endm

three_same_int vadd,    add,    1,1,1,1
three_same_int vsub,    sub,    1,1,1,1
three_same_int vmul,    mul,    1,1,1,0
three_same_int vpmul,   pmul,   1,0,0,0
three_same_int vcmeq,   cmeq,   1,1,1,1
three_same_int vcmtst,  cmtst,  1,1,1,1
three_same_int vcmgt,   cmgt,   1,1,1,1
three_same_int vcmge,   cmge,   1,1,1,1
three_same_int vcmhi,   cmhi,   1,1,1,1
three_same_int vcmhs,   cmhs,   1,1,1,1
three_same_int vsmax,   smax,   1,1,1,0
three_same_int vsmin,   smin,   1,1,1,0
three_same_int vumax,   umax,   1,1,1,0
three_same_int vumin,   umin,   1,1,1,0
three_same_int vsabd,   sabd,   1,1,1,0
three_same_int vuabd,   uabd,   1,1,1,0
three_same_int vshadd,  shadd,  1,1,1,0
three_same_int vuhadd,  uhadd,  1,1,1,0
three_same_int vsrhadd, srhadd, 1,1,1,0
three_same_int vurhadd, urhadd, 1,1,1,0
three_same_int vshsub,  shsub,  1,1,1,0
three_same_int vuhsub,  uhsub,  1,1,1,0
three_same_int vsshl,   sshl,   1,1,1,1
three_same_int vushl,   ushl,   1,1,1,1
three_same_int vsrshl,  srshl,  1,1,1,1
three_same_int vurshl,  urshl,  1,1,1,1
three_same_int vsqshl,  sqshl,  1,1,1,1
three_same_int vuqshl,  uqshl,  1,1,1,1
three_same_int vsqrshl, sqrshl, 1,1,1,1
three_same_int vuqrshl, uqrshl, 1,1,1,1
three_same_int vsqadd,  sqadd,  1,1,1,1
three_same_int vuqadd,  uqadd,  1,1,1,1
three_same_int vsqsub,  sqsub,  1,1,1,1
three_same_int vuqsub,  uqsub,  1,1,1,1
three_same_int vsqdmulh, sqdmulh, 0,1,1,0
three_same_int vsqrdmulh, sqrdmulh, 0,1,1,0
three_same_int vaddp,   addp,   1,1,1,1
three_same_int vsmaxp,  smaxp,  1,1,1,0
three_same_int vsminp,  sminp,  1,1,1,0
three_same_int vumaxp,  umaxp,  1,1,1,0
three_same_int vuminp,  uminp,  1,1,1,0

three_same_acc vmla,  mla
three_same_acc vmls,  mls
three_same_acc vsaba, saba
three_same_acc vuaba, uaba

// ---- FP vector three-same -----------------------------------------------
// Arrangement from (sz, Q): sz=0 Q=0 -> .2s, sz=0 Q=1 -> .4s, sz=1 Q=1 ->
// .2d (sz=1 Q=0 is UNDEFINED; gen rejects it).
// Code stream: [gadget][rd | rn<<8 | rm<<16 | sz<<24 | Q<<26]
.macro fp_three_same name, op
.gadget \name
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x9, x8, #0x1f          // rd
    ubfx x10, x8, #8, #5       // rn
    ubfx x11, x8, #16, #5      // rm
    ubfx x12, x8, #24, #1      // sz
    ubfx x13, x8, #26, #1      // q
    add x14, _cpu, #CPU_v0
    add x15, x14, x10, lsl #4
    ldr q0, [x15]
    add x16, x14, x11, lsl #4
    ldr q1, [x16]
    cbnz x12, 1f
    cbz x13, 2f
    \op v0.4s, v0.4s, v1.4s
    b 3f
2:  \op v0.2s, v0.2s, v1.2s
    b 3f
1:  \op v0.2d, v0.2d, v1.2d
3:  add x15, x14, x9, lsl #4
    str q0, [x15]
    gret
.endm

.macro fp_three_same_acc name, op
.gadget \name
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x9, x8, #0x1f          // rd
    ubfx x10, x8, #8, #5       // rn
    ubfx x11, x8, #16, #5      // rm
    ubfx x12, x8, #24, #1      // sz
    ubfx x13, x8, #26, #1      // q
    add x14, _cpu, #CPU_v0
    add x15, x14, x9, lsl #4
    ldr q0, [x15]              // Vd (accumulator)
    add x16, x14, x10, lsl #4
    ldr q1, [x16]              // Vn
    add x16, x14, x11, lsl #4
    ldr q2, [x16]              // Vm
    cbnz x12, 1f
    cbz x13, 2f
    \op v0.4s, v1.4s, v2.4s
    b 3f
2:  \op v0.2s, v1.2s, v2.2s
    b 3f
1:  \op v0.2d, v1.2d, v2.2d
3:  str q0, [x15]
    gret
.endm

fp_three_same vfadd,    fadd
fp_three_same vfsub,    fsub
fp_three_same vfmul,    fmul
fp_three_same vfdiv,    fdiv
fp_three_same vfmulx,   fmulx
fp_three_same vfmax,    fmax
fp_three_same vfmin,    fmin
fp_three_same vfmaxnm,  fmaxnm
fp_three_same vfminnm,  fminnm
fp_three_same vfaddp,   faddp
fp_three_same vfmaxp,   fmaxp
fp_three_same vfminp,   fminp
// FMAXNMP/FMINNMP vector exist in the ISA but are absent from OpenMinis'
// three-same table (a coverage gap there, fixed here).
fp_three_same vfmaxnmp, fmaxnmp
fp_three_same vfminnmp, fminnmp
fp_three_same vfrecps,  frecps
fp_three_same vfrsqrts, frsqrts
fp_three_same vfcmeq,   fcmeq
fp_three_same vfcmge,   fcmge
fp_three_same vfcmgt,   fcmgt
fp_three_same vfacge,   facge
fp_three_same vfacgt,   facgt
fp_three_same vfabd,    fabd

fp_three_same_acc vfmla, fmla
fp_three_same_acc vfmls, fmls

// ---- Scalar three-same (D-register) integer ops -------------------------
// Prefix 0x5e (U=0) / 0x7e (U=1); operate on a single 64-bit D register.
// Code stream: [gadget][rd | rn<<8 | rm<<16]
.macro scalar_three_same name, op
.gadget \name
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x9, x8, #0x1f
    ubfx x10, x8, #8, #5
    ubfx x11, x8, #16, #5
    add x14, _cpu, #CPU_v0
    add x15, x14, x10, lsl #4
    ldr d0, [x15]
    add x16, x14, x11, lsl #4
    ldr d1, [x16]
    \op d0, d0, d1
    add x15, x14, x9, lsl #4
    str q0, [x15]              // scalar D-op zeroes the upper 64 bits
    gret
.endm

scalar_three_same sadd,   add
scalar_three_same ssub,   sub
scalar_three_same scmeq,  cmeq
scalar_three_same scmgt,  cmgt
scalar_three_same scmge,  cmge
scalar_three_same scmhi,  cmhi
scalar_three_same scmhs,  cmhs
scalar_three_same scmtst, cmtst
scalar_three_same ssshl,  sshl
scalar_three_same sushl,  ushl
scalar_three_same ssrshl, srshl
scalar_three_same surshl, urshl

// Scalar saturating ops write FPSR.QC on saturation; the host op does that
// natively and our FPSR is materialized lazily (mrs on read), matching how
// OpenMinis handles it. B/H/S/D element widths all exist for these; the
// code stream carries size so we dispatch. [gadget][rd | rn<<8 | rm<<16 |
// size<<24]
.macro scalar_three_same_sat name, op
.gadget \name
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x9, x8, #0x1f
    ubfx x10, x8, #8, #5
    ubfx x11, x8, #16, #5
    ubfx x12, x8, #24, #2
    add x14, _cpu, #CPU_v0
    add x15, x14, x10, lsl #4
    ldr q0, [x15]
    add x16, x14, x11, lsl #4
    ldr q1, [x16]
    cmp x12, #0
    b.eq \name\()_b
    cmp x12, #1
    b.eq \name\()_h
    cmp x12, #2
    b.eq \name\()_s
    \op d0, d0, d1
    b \name\()_st
\name\()_b:
    \op b0, b0, b1
    b \name\()_st
\name\()_h:
    \op h0, h0, h1
    b \name\()_st
\name\()_s:
    \op s0, s0, s1
\name\()_st:
    // The host scalar op zeroes all upper bits of v0 past the element.
    add x15, x14, x9, lsl #4
    str q0, [x15]
    gret
.endm

scalar_three_same_sat ssqadd,  sqadd
scalar_three_same_sat suqadd,  uqadd
scalar_three_same_sat ssqsub,  sqsub
scalar_three_same_sat suqsub,  uqsub
scalar_three_same_sat ssqshl,  sqshl
scalar_three_same_sat suqshl,  uqshl
scalar_three_same_sat ssqrshl, sqrshl
scalar_three_same_sat suqrshl, uqrshl

// SQDMULH/SQRDMULH scalar: h/s only.
.macro scalar_three_same_hs name, op
.gadget \name
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x9, x8, #0x1f
    ubfx x10, x8, #8, #5
    ubfx x11, x8, #16, #5
    ubfx x12, x8, #24, #2
    add x14, _cpu, #CPU_v0
    add x15, x14, x10, lsl #4
    ldr q0, [x15]
    add x16, x14, x11, lsl #4
    ldr q1, [x16]
    cmp x12, #1
    b.eq \name\()_h
    \op s0, s0, s1
    b \name\()_st
\name\()_h:
    \op h0, h0, h1
\name\()_st:
    add x15, x14, x9, lsl #4
    str q0, [x15]
    gret
.endm

scalar_three_same_hs ssqdmulh,  sqdmulh
scalar_three_same_hs ssqrdmulh, sqrdmulh

// Scalar FP three-same stragglers not in our fp.S fp2src family: FABD,
// FMULX, FRECPS, FRSQRTS, FCMEQ/FCMGE/FCMGT/FACGE/FACGT (register form).
// [gadget][rd | rn<<8 | rm<<16 | sz<<24]  sz: 0=S, 1=D
.macro scalar_fp_three_same name, op
.gadget \name
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x9, x8, #0x1f
    ubfx x10, x8, #8, #5
    ubfx x11, x8, #16, #5
    ubfx x12, x8, #24, #1
    add x14, _cpu, #CPU_v0
    add x15, x14, x10, lsl #4
    ldr q0, [x15]
    add x16, x14, x11, lsl #4
    ldr q1, [x16]
    cbnz x12, 1f
    \op s0, s0, s1
    b 2f
1:  \op d0, d0, d1
2:  add x15, x14, x9, lsl #4
    str q0, [x15]
    gret
.endm

scalar_fp_three_same sfabd,    fabd
scalar_fp_three_same sfmulx,   fmulx
scalar_fp_three_same sfrecps,  frecps
scalar_fp_three_same sfrsqrts, frsqrts
scalar_fp_three_same sfcmeq,   fcmeq
scalar_fp_three_same sfcmge,   fcmge
scalar_fp_three_same sfcmgt,   fcmgt
scalar_fp_three_same sfacge,   facge
scalar_fp_three_same sfacgt,   facgt

# vim: ft=gas
