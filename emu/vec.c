#include <math.h>
#include <string.h>

#include "emu/vec.h"
#include "emu/cpu.h"

union vec {
    uint8_t u8[16];
    uint16_t u16[8];
    uint32_t u32[4];
    uint64_t u64[2];
    __uint128_t u128[1];
    __uint128_t dqw;
};

static inline void zero_xmm(union xmm_reg *xmm) {
    xmm->qw[0] = 0;
    xmm->qw[1] = 0;
}

static inline int32_t satsw(int32_t dw) {
    if (dw > 0xff80)
        dw &= 0xff;
    else if (dw > 0x7fff)
        dw = 0x80;
    else if (dw > 0x7f)
        dw = 0x7f;
    return dw;
}
static inline uint32_t satud(uint32_t dw) {
    if (dw > 0xffff8000)
        dw &= 0xffff;
    else if (dw > 0x7fffffff)
        dw = 0x8000;
    else if (dw > 0x7fff)
        dw = 0x7fff;
    return dw;
}
static inline uint32_t satub(uint32_t dw) {
    if (dw >= 0x8000)
        dw = 0;
    else if (dw > 0xff)
        dw = 0xff;
    return dw;
}
static inline uint32_t satsb(uint32_t dw) {
    if (dw > 0xffffff80)
        dw &= 0xff;
    else if (dw > 0x7fffffff)
        dw = 0x80;
    else if (dw > 0x7f)
        dw = 0x7f;
    return dw;
}

#define VEC_ZERO_COPY(zero, copy) \
    void vec_zero##zero##_copy##copy(NO_CPU, const void *src, void *dst) { \
        memcpy(dst, src, copy/8); \
        memset((char *) dst + copy/8, 0, (zero-copy)/8); \
    }
VEC_ZERO_COPY(128, 128)
VEC_ZERO_COPY(128, 64)
VEC_ZERO_COPY(128, 32)
VEC_ZERO_COPY(64, 64)
VEC_ZERO_COPY(64, 32)
VEC_ZERO_COPY(32, 32)

void vec_merge32(NO_CPU, const void *src, void *dst) {
    memcpy(dst, src, 4);
}
void vec_merge64(NO_CPU, const void *src, void *dst) {
    memcpy(dst, src, 8);
}
void vec_merge128(NO_CPU, const void *src, void *dst) {
    memcpy(dst, src, 16);
}

#define _SHIFT(op, size) \
    do { \
        if (unlikely(amount > (size)-1)) { \
            zero_xmm(dst); \
        } else { \
            union vec d = { .dqw = dst->u128 }; \
            for (unsigned i = 0; i < array_size(d.u##size); i++) \
                d.u##size[i] op##= amount; \
            dst->u128 = d.dqw; \
        } \
    } while (0)

#define VEC_SSE_SHIFT(dir, suffix, op, size) \
    void vec_shift##dir##_##suffix##128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) { \
        const uint8_t amount = src->u8[0]; \
        _SHIFT(op, size); \
    } \
    void vec_imm_shift##dir##_##suffix##128(NO_CPU, const uint8_t amount, union xmm_reg *dst) { \
        _SHIFT(op, size); \
    }

#define _VEC_SSE_CMP(sgn, usgn, suffix, relop, size) \
    void vec_compare##sgn##_##suffix##128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) { \
        union vec s = { .dqw = src->u128 }, d = { .dqw = dst->u128 }; \
        for (unsigned i = 0; i < array_size(s.u##size); i++) \
            d.u##size[i] = (usgn##int##size##_t)d.u##size[i] relop (usgn##int##size##_t)s.u##size[i] ? ~0 : 0;\
        dst->u128 = d.dqw; \
    }

#define VEC_SSE_CMPD(suffix, relop, size) \
    _VEC_SSE_CMP(, u, suffix, relop, size)
#define VEC_SSE_CMPS(suffix, relop, size) \
    _VEC_SSE_CMP(s,, suffix, relop, size)

#define VEC_SSE_OP(name, suffix, op, size) \
    void vec_##name##_##suffix##128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) { \
        union vec s = { .dqw = src->u128 }, d = { .dqw = dst->u128 }; \
        for (unsigned i = 0; i < array_size(s.u##size); i++) \
            d.u##size[i] op##= s.u##size[i]; \
        dst->u128 = d.dqw; \
    }

VEC_SSE_SHIFT(r, w, >>, 16)
VEC_SSE_SHIFT(r, d, >>, 32)
VEC_SSE_SHIFT(r, q, >>, 64)

VEC_SSE_SHIFT(l, w, <<, 16)
VEC_SSE_SHIFT(l, d, <<, 32)
VEC_SSE_SHIFT(l, q, <<, 64)

VEC_SSE_CMPD(eqb, ==,  8)
VEC_SSE_CMPD(eqw, ==, 16)
VEC_SSE_CMPD(eqd, ==, 32)

VEC_SSE_CMPS(gtb, >,  8)
VEC_SSE_CMPS(gtw, >, 16)
VEC_SSE_CMPS(gtd, >, 32)

VEC_SSE_OP(add, b, +, 8)
VEC_SSE_OP(add, w, +, 16)
VEC_SSE_OP(add, d, +, 32)
VEC_SSE_OP(add, q, +, 64)

VEC_SSE_OP(sub, b, -, 8)
VEC_SSE_OP(sub, w, -, 16)
VEC_SSE_OP(sub, d, -, 32)
VEC_SSE_OP(sub, q, -, 64)

VEC_SSE_OP(and, dq, &, 128)
VEC_SSE_OP(or,  dq, |, 128)
VEC_SSE_OP(xor, dq, ^, 128)

void vec_imm_shiftl_dq128(NO_CPU, uint8_t amount, union xmm_reg *dst) {
    if (amount >= 16)
        zero_xmm(dst);
    else
        dst->u128 <<= amount * 8;
}
void vec_imm_shiftr_dq128(NO_CPU, uint8_t amount, union xmm_reg *dst) {
    if (amount >= 16)
        zero_xmm(dst);
    else
        dst->u128 >>= amount * 8;
}
void vec_shiftrs_w128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    const uint8_t amount = src->u8[0];
    for (unsigned i = 0; i < 8; i++) {
        if (unlikely(amount > 15))
            dst->u16[i] = ((dst->u16[i] >> 15) & (uint16_t)1) ? 0xffff : 0;
        else
            dst->u16[i] = ((int16_t)(dst->u16[i])) >> amount;
    }
}
void vec_shiftrs_d128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    const uint8_t amount = src->u8[0];
    for (unsigned i = 0; i < 4; i++) {
        if (unlikely(amount > 31))
            dst->u32[i] = ((dst->u32[i] >> 31) & (uint32_t)1) ? 0xffffffff : 0;
        else
            dst->u32[i] = ((int32_t)(dst->u32[i])) >> amount;
    }
}
void vec_imm_shiftrs_w128(NO_CPU, const uint8_t amount, union xmm_reg *dst) {
    for (unsigned i = 0; i < 8; i++) {
        if (unlikely(amount > 15))
            dst->u16[i] = ((dst->u16[i] >> 15) & (uint16_t)1) ? 0xffff : 0;
        else
            dst->u16[i] = ((int16_t)(dst->u16[i])) >> amount;
    }
}
void vec_imm_shiftrs_d128(NO_CPU, const uint8_t amount, union xmm_reg *dst) {
    for (unsigned i = 0; i < 4; i++) {
        if (unlikely(amount > 31))
            dst->u32[i] = ((dst->u32[i] >> 31) & (uint32_t)1) ? 0xffffffff : 0;
        else
            dst->u32[i] = ((int32_t)(dst->u32[i])) >> amount;
    }
}

void vec_addus_b128(NO_CPU, union xmm_reg *src, union xmm_reg *dst) {
    for (unsigned i = 0; i < 16; i++) {
        const int32_t sb = dst->u8[i] + src->u8[i];
        dst->u8[i] = sb > 0xff ? 0xff : sb;
    }
}
void vec_addus_w128(NO_CPU, union xmm_reg *src, union xmm_reg *dst) {
    for (unsigned i = 0; i < 8; i++) {
        const int32_t sw = dst->u16[i] + src->u16[i];
        dst->u16[i] = sw > 0xffff ? 0xffff : sw;
    }
}
void vec_addss_b128(NO_CPU, union xmm_reg *src, union xmm_reg *dst) {
    for (unsigned i = 0; i < 16; i++)
        dst->u8[i] = satsb((int8_t)dst->u8[i] + (int8_t)src->u8[i]);
}
void vec_addss_w128(NO_CPU, union xmm_reg *src, union xmm_reg *dst) {
    for (unsigned i = 0; i < 8; i++)
        dst->u16[i] = satud((int16_t)dst->u16[i] + (int16_t)src->u16[i]);
}

void vec_subus_b128(NO_CPU, union xmm_reg *src, union xmm_reg *dst) {
    for (unsigned i = 0; i < 16; i++) {
        const int32_t sb = dst->u8[i] - src->u8[i];
        dst->u8[i] = sb < 0 ? 0 : sb;
    }
}
void vec_subus_w128(NO_CPU, union xmm_reg *src, union xmm_reg *dst) {
    for (unsigned i = 0; i < 8; i++) {
        const int32_t sw = dst->u16[i] - src->u16[i];
        dst->u16[i] = sw < 0 ? 0 : sw;
    }
}
void vec_subss_b128(NO_CPU, union xmm_reg *src, union xmm_reg *dst) {
    for (unsigned i = 0; i < 16; i++)
        dst->u8[i] = satsb((int8_t)dst->u8[i] - (int8_t)src->u8[i]);
}
void vec_subss_w128(NO_CPU, union xmm_reg *src, union xmm_reg *dst) {
    for (unsigned i = 0; i < 8; i++)
        dst->u16[i] = satud((int16_t)dst->u16[i] - (int16_t)src->u16[i]);
}

void vec_madd_d128(NO_CPU, union xmm_reg *src, union xmm_reg *dst) {
    dst->u32[0] = (int32_t)((int16_t)dst->u16[0] * (int16_t)src->u16[0]) +
                  (int32_t)((int16_t)dst->u16[1] * (int16_t)src->u16[1]);
    dst->u32[1] = (int32_t)((int16_t)dst->u16[2] * (int16_t)src->u16[2]) +
                  (int32_t)((int16_t)dst->u16[3] * (int16_t)src->u16[3]);
    dst->u32[2] = (int32_t)((int16_t)dst->u16[4] * (int16_t)src->u16[4]) +
                  (int32_t)((int16_t)dst->u16[5] * (int16_t)src->u16[5]);
    dst->u32[3] = (int32_t)((int16_t)dst->u16[6] * (int16_t)src->u16[6]) +
                  (int32_t)((int16_t)dst->u16[7] * (int16_t)src->u16[7]);
}

void vec_sumabs_w128(NO_CPU, union xmm_reg *src, union xmm_reg *dst) {
    uint32_t sum[2] = { 0, 0 };
    for (unsigned i = 0; i < 8; i++) {
        int32_t difflo = dst->u8[i + 0] - src->u8[i + 0];
        int32_t diffhi = dst->u8[i + 8] - src->u8[i + 8];
        sum[0] += (difflo < 0) ? -(uint32_t)difflo : difflo;
        sum[1] += (diffhi < 0) ? -(uint32_t)diffhi : diffhi;
    }
    dst->u32[0] = sum[0];
    dst->u32[2] = sum[1];
    dst->u32[1] = dst->u32[3] = 0;
}

void vec_mulu_dq128(NO_CPU, union xmm_reg *src, union xmm_reg *dst) {
    dst->qw[0] = (uint64_t) src->u32[0] * dst->u32[0];
    dst->qw[1] = (uint64_t) src->u32[2] * dst->u32[2];
}

void vec_andn128(NO_CPU, union xmm_reg *src, union xmm_reg *dst) {
    dst->qw[0] = ~dst->qw[0] & src->qw[0];
    dst->qw[1] = ~dst->qw[1] & src->qw[1];
}

void vec_min_ub128(NO_CPU, union xmm_reg *src, union xmm_reg *dst) {
    for (unsigned i = 0; i < array_size(src->u8); i++)
        if (src->u8[i] < dst->u8[i])
            dst->u8[i] = src->u8[i];
}
void vec_max_ub128(NO_CPU, union xmm_reg *src, union xmm_reg *dst) {
    for (unsigned i = 0; i < array_size(src->u8); i++)
        if (src->u8[i] > dst->u8[i])
            dst->u8[i] = src->u8[i];
}
void vec_mins_w128(NO_CPU, union xmm_reg *src, union xmm_reg *dst) {
    for (unsigned i = 0; i < 8; i++)
        dst->u16[i] = (int16_t)dst->u16[i] < (int16_t)src->u16[i] ? dst->u16[i] : src->u16[i];
}

void vec_maxs_w128(NO_CPU, union xmm_reg *src, union xmm_reg *dst) {
    for (unsigned i = 0; i < 8; i++)
        dst->u16[i] = (int16_t)dst->u16[i] > (int16_t)src->u16[i] ? dst->u16[i] : src->u16[i];
}

static bool cmpd(double a, double b, int type) {
    bool res;
    switch (type % 4) {
        case 0: res = a == b; break;
        case 1: res = a < b; break;
        case 2: res = a <= b; break;
        case 3: res = isnan(a) || isnan(b); break;
    }
    if (type >= 4) res = !res;
    return res;
}
static bool cmps(float a, float b, int type) {
    bool res;
    switch (type % 4) {
        case 0: res = a == b; break;
        case 1: res = a < b; break;
        case 2: res = a <= b; break;
        case 3: res = isnan(a) || isnan(b); break;
    }
    if (type >= 4) res = !res;
    return res;
}

void vec_single_fcmp64(NO_CPU, const double *src, union xmm_reg *dst, uint8_t type) {
    dst->qw[0] = cmpd(dst->f64[0], *src, type) ? -1 : 0;
}
void vec_single_fcmp32(NO_CPU, const float *src, union xmm_reg *dst, uint8_t type) {
    dst->u32[0] = cmps(dst->f32[0], *src, type) ? -1 : 0;
}

void vec_single_fadd64(NO_CPU, const double *src, double *dst) { *dst += *src; }
void vec_single_fadd32(NO_CPU, const float *src, float *dst) { *dst += *src; }
void vec_single_fmul64(NO_CPU, const double *src, double *dst) { *dst *= *src; }
void vec_single_fmul32(NO_CPU, const float *src, float *dst) { *dst *= *src; }
void vec_single_fsub64(NO_CPU, const double *src, double *dst) { *dst -= *src; }
void vec_single_fsub32(NO_CPU, const float *src, float *dst) { *dst -= *src; }
void vec_single_fdiv64(NO_CPU, const double *src, double *dst) { *dst /= *src; }
void vec_single_fdiv32(NO_CPU, const float *src, float *dst) { *dst /= *src; }

void vec_single_fsqrt64(NO_CPU, const double *src, double *dst) { *dst = sqrt(*src); }
void vec_single_fsqrt32(NO_CPU, const float *src, float *dst) { *dst = sqrtf(*src); }

// x86 [max|min]s[s|d]: DEST is kept ONLY when (DEST > SRC) / (DEST < SRC);
// in every other case -- SRC greater/less, the two equal (incl. +0 vs -0), or
// either NaN -- the SRC operand is returned. So the predicate is !(dst cmp src),
// NOT (src cmp dst): the latter keeps dst on the equal/unordered tie, which
// differs from real x86 (and from the amd64 path's `dst>src ? dst : src`) on
// the +0/-0 case. Using !(dst cmp src) also folds in the NaN rule for free
// (a NaN compare is false, so !false -> take src).
void vec_single_fmax64(NO_CPU, const double *src, double *dst) {
    if (!(*dst > *src)) *dst = *src;
}
void vec_single_fmin64(NO_CPU, const double *src, double *dst) {
    if (!(*dst < *src)) *dst = *src;
}
void vec_single_fmax32(NO_CPU, const float *src, float *dst) {
    if (!(*dst > *src)) *dst = *src;
}
void vec_single_fmin32(NO_CPU, const float *src, float *dst) {
    if (!(*dst < *src)) *dst = *src;
}

static void vec_set_ucomi_flags(struct cpu_state *cpu, bool unordered, bool equal, bool less) {
    cpu->zf_res = 0;
    cpu->pf_res = 0;
    cpu->sf_res = 0;
    cpu->af_ops = 0;

    cpu->zf = unordered || equal;
    cpu->pf = unordered;
    cpu->cf = unordered || less;
    cpu->sf = 0;
    cpu->af = 0;
    cpu->of = 0;
    cpu->cf_bit = cpu->cf;
    cpu->of_bit = 0;
}

void vec_single_ucomi32(struct cpu_state *cpu, const float *src, const float *dst) {
    bool unordered = isnan(*src) || isnan(*dst);
    vec_set_ucomi_flags(cpu, unordered, *src == *dst, *dst < *src);
}

void vec_single_ucomi64(struct cpu_state *cpu, const double *src, const double *dst) {
    bool unordered = isnan(*src) || isnan(*dst);
    vec_set_ucomi_flags(cpu, unordered, *src == *dst, *dst < *src);
}

#define VEC_PACKED_OP(name, op, field, size, n) \
    void vec_##name##size(NO_CPU, union xmm_reg *src, union xmm_reg *dst) { \
        for (int i = 0; i < n; ++i) { \
            dst->field[i] op##= src->field[i]; \
        } \
    }

VEC_PACKED_OP(add_p, +, f64, 64, 2)
VEC_PACKED_OP(add_p, +, f32, 32, 4)
VEC_PACKED_OP(sub_p, -, f64, 64, 2)
VEC_PACKED_OP(sub_p, -, f32, 32, 4)
VEC_PACKED_OP(mul_p, *, f64, 64, 2)
VEC_PACKED_OP(mul_p, *, f32, 32, 4)
VEC_PACKED_OP(div_p, /, f64, 64, 2)
VEC_PACKED_OP(div_p, /, f32, 32, 4)

// Packed min/max mirror the scalar single_fmin/single_fmax rule exactly: keep
// dst only when (dst cmp src) holds, else take src -- so the equal/unordered
// (incl. +0 vs -0) and NaN cases both return src, matching real x86.
#define VEC_PACKED_MINMAX(name, cmp, field, size, n) \
    void vec_##name##size(NO_CPU, union xmm_reg *src, union xmm_reg *dst) { \
        for (int i = 0; i < n; ++i) { \
            if (!(dst->field[i] cmp src->field[i])) \
                dst->field[i] = src->field[i]; \
        } \
    }
VEC_PACKED_MINMAX(min_p, <, f64, 64, 2)
VEC_PACKED_MINMAX(min_p, <, f32, 32, 4)
VEC_PACKED_MINMAX(max_p, >, f64, 64, 2)
VEC_PACKED_MINMAX(max_p, >, f32, 32, 4)

// sqrtpd/sqrtps: dst[i] = sqrt(src[i]) (unary; not an accumulate). Read all
// source lanes before writing in case src and dst are the same register.
void vec_sqrt_p64(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    double s0 = src->f64[0], s1 = src->f64[1];
    dst->f64[0] = sqrt(s0);
    dst->f64[1] = sqrt(s1);
}
void vec_sqrt_p32(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    float s0 = src->f32[0], s1 = src->f32[1], s2 = src->f32[2], s3 = src->f32[3];
    dst->f32[0] = sqrtf(s0);
    dst->f32[1] = sqrtf(s1);
    dst->f32[2] = sqrtf(s2);
    dst->f32[3] = sqrtf(s3);
}

// cvtps2pd: two packed floats (low 64 bits of src) -> two doubles.
void vec_cvtps2pd64(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    float s0 = src->f32[0], s1 = src->f32[1];
    dst->f64[0] = s0;
    dst->f64[1] = s1;
}
// cvtpd2ps: two doubles -> two floats in the low 64 bits; high 64 bits zeroed.
void vec_cvtpd2ps128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    double s0 = src->f64[0], s1 = src->f64[1];
    dst->f32[0] = (float) s0;
    dst->f32[1] = (float) s1;
    dst->f32[2] = 0;
    dst->f32[3] = 0;
}
// cvtdq2ps: four packed signed int32 -> four floats (same lane count/width).
void vec_cvtdq2ps128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    int32_t s0 = (int32_t) src->u32[0], s1 = (int32_t) src->u32[1];
    int32_t s2 = (int32_t) src->u32[2], s3 = (int32_t) src->u32[3];
    dst->f32[0] = s0;
    dst->f32[1] = s1;
    dst->f32[2] = s2;
    dst->f32[3] = s3;
}

void vec_fcmp_p64(NO_CPU, const union xmm_reg *src, union xmm_reg *dst, uint8_t type) {
    for (size_t i = 0; i < sizeof(dst->f64) / sizeof(*dst->f64); ++i) {
        dst->qw[i] = cmpd(dst->f64[i], src->f64[i], type) ? -1 : 0;
    }
}

// come to the dark side of macros
#define _ISNAN_int32_t(x) false
#define VEC_CAST(src, dst, src_t, dst_t, n) \
    do { \
        for (int i = 0; i < n; ++i) \
            ((dst_t *)dst)[i] = ((src_t *)src)[i]; \
    } while (0)

// x86 cvtt* (truncating float/double -> signed int32) return the integer
// indefinite 0x80000000 for NaN AND for any value outside the int32 range. A
// bare C cast is wrong on the arm64 host: float/double -> int32 SATURATES there
// (e.g. (int32_t)1e30 -> 0x7fffffff), whereas x86 yields 0x80000000 for overflow
// in BOTH directions. Range-check explicitly, matching amd64_cvtt_scalar_to_int
// (emu/amd64_interp.c). Every user of this macro has an int32_t destination.
#define VEC_TRUNC_INT(src, dst, src_t, dst_t, n) \
    do { \
        for (int i = 0; i < n; ++i) { \
            src_t _v = ((src_t *)src)[i]; \
            if (isnan(_v) || _v >= 2147483648.0 || _v < -2147483648.0) \
                ((dst_t *)dst)[i] = INT32_MIN; \
            else \
                ((dst_t *)dst)[i] = (dst_t) _v; \
        } \
    } while (0)

#define VEC_CVT(name, src_t, dst_t) \
    void vec_cvt##name(NO_CPU, const src_t *src, dst_t *dst) { \
        VEC_CAST(src, dst, src_t, dst_t, 1); \
    }

#define VEC_CVTT(name, src_t, dst_t) \
    void vec_cvt##name(NO_CPU, const src_t *src, dst_t *dst) { \
        VEC_TRUNC_INT(src, dst, src_t, dst_t, 1); \
    }

#define PACKED_VEC_CVTT(name, src_field, dst_field, src_t, dst_t, n) \
    void vec_cvt##name(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) { \
        VEC_TRUNC_INT(src->src_field, dst->dst_field, src_t, dst_t, n); \
        /* Note: this needs to be second, because src and dst may alias */ \
        memset(dst->dst_field + n, 0, sizeof(*dst) - n * sizeof(*dst->dst_field)); \
    }

VEC_CVT(si2sd32, int32_t, double)
VEC_CVTT(tsd2si64, double, int32_t)
VEC_CVT(sd2ss64, double, float)
VEC_CVT(si2ss32, int32_t, float)
VEC_CVTT(tss2si32, float, int32_t)
VEC_CVT(ss2sd32, float, double)

PACKED_VEC_CVTT(tpd2dq64, f64, u32, double, int32_t, 2)
PACKED_VEC_CVTT(tps2dq32, f32, u32, float, int32_t, 4)

// cvtdq2pd: two packed signed int32 from the low 64 bits of src -> two
// doubles in dst. Read both source dwords before writing any result: src and
// dst may be the same register, and writing dst->f64[0] clobbers src->u32[1].
void vec_cvtdq2pd64(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    int32_t s0 = (int32_t) src->u32[0];
    int32_t s1 = (int32_t) src->u32[1];
    dst->f64[0] = s0;
    dst->f64[1] = s1;
}

void vec_unpackl_bw128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    for (int i = 7; i >= 0; i--) {
        dst->u8[i*2 + 1] = src->u8[i];
        dst->u8[i*2] = dst->u8[i];
    }
}
void vec_unpackl_w128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    for (int i = 3; i >= 0; i--) {
        dst->u16[i*2 + 1] = src->u16[i];
        dst->u16[i*2] = dst->u16[i];
    }
}
void vec_unpackl_dq128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    dst->u32[3] = src->u32[1];
    dst->u32[2] = dst->u32[1];
    dst->u32[1] = src->u32[0];
}
void vec_unpackl_qdq128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    dst->qw[1] = src->qw[0];
}
void vec_unpackl_ps128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    dst->u32[2] = dst->u32[1];
    dst->u32[1] = src->u32[0];
    dst->u32[3] = src->u32[1];
}
void vec_unpackl_pd128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    dst->f64[1] = src->f64[0];
}
void vec_unpackh_bw128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    for (int i = 0; i < 8; i++) {
        dst->u8[2 * i + 0] = dst->u8[i + 8];
        dst->u8[2 * i + 1] = src->u8[i + 8];
    }
}
void vec_unpackh_w128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    for (int i = 0; i < 4; i++) {
        dst->u16[2 * i + 0] = dst->u16[i + 4];
        dst->u16[2 * i + 1] = src->u16[i + 4];
    }
}
void vec_unpackh_d128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    dst->u32[0] = dst->u32[2];
    dst->u32[1] = src->u32[2];
    dst->u32[2] = dst->u32[3];
    dst->u32[3] = src->u32[3];
}
void vec_unpackh_dq128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    dst->qw[0] = dst->qw[1];
    dst->qw[1] = src->qw[1];
}
void vec_unpackh_ps128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    dst->u32[0] = dst->u32[2];
    dst->u32[1] = src->u32[2];
    dst->u32[2] = dst->u32[3];
    dst->u32[3] = src->u32[3];
}
void vec_unpackh_pd128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    dst->f64[0] = dst->f64[1];
    dst->f64[1] = src->f64[1];
}

void vec_packss_w128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    dst->u32[0] = (satsw(dst->u16[0]) << 0x00) | (satsw(dst->u16[1]) << 0x08) |
                  (satsw(dst->u16[2]) << 0x10) | (satsw(dst->u16[3]) << 0x18);
    dst->u32[1] = (satsw(dst->u16[4]) << 0x00) | (satsw(dst->u16[5]) << 0x08) |
                  (satsw(dst->u16[6]) << 0x10) | (satsw(dst->u16[7]) << 0x18);
    dst->u32[2] = (satsw(src->u16[0]) << 0x00) | (satsw(src->u16[1]) << 0x08) |
                  (satsw(src->u16[2]) << 0x10) | (satsw(src->u16[3]) << 0x18);
    dst->u32[3] = (satsw(src->u16[4]) << 0x00) | (satsw(src->u16[5]) << 0x08) |
                  (satsw(src->u16[6]) << 0x10) | (satsw(src->u16[7]) << 0x18);
}
void vec_packss_d128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    dst->u32[0] = satud(dst->u32[0]) | (satud(dst->u32[1]) << 16);
    dst->u32[1] = satud(dst->u32[2]) | (satud(dst->u32[3]) << 16);
    dst->u32[2] = satud(src->u32[0]) | (satud(src->u32[1]) << 16);
    dst->u32[3] = satud(src->u32[2]) | (satud(src->u32[3]) << 16);
}
void vec_packsu_w128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    dst->u32[0] = (satub(dst->u16[0]) << 0x00) | (satub(dst->u16[1]) << 0x08) |
                  (satub(dst->u16[2]) << 0x10) | (satub(dst->u16[3]) << 0x18);
    dst->u32[1] = (satub(dst->u16[4]) << 0x00) | (satub(dst->u16[5]) << 0x08) |
                  (satub(dst->u16[6]) << 0x10) | (satub(dst->u16[7]) << 0x18);
    dst->u32[2] = (satub(src->u16[0]) << 0x00) | (satub(src->u16[1]) << 0x08) |
                  (satub(src->u16[2]) << 0x10) | (satub(src->u16[3]) << 0x18);
    dst->u32[3] = (satub(src->u16[4]) << 0x00) | (satub(src->u16[5]) << 0x08) |
                  (satub(src->u16[6]) << 0x10) | (satub(src->u16[7]) << 0x18);
}

void vec_shuffle_lw128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst, uint8_t encoding) {
    union xmm_reg src_copy = *src;
    for (int i = 0; i < 4; i++)
        dst->u16[i] = src_copy.u16[(encoding >> (i*2)) % 4];
    dst->qw[1] = src->qw[1];
}
void vec_shuffle_hw128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst, uint8_t encoding) {
    union xmm_reg src_copy = *src;
    dst->qw[0] = src->qw[0];
    dst->u32[2] = src_copy.u16[(encoding >> 0 & 3) | 4] | src_copy.u16[(encoding >> 2 & 3) | 4] << 16;
    dst->u32[3] = src_copy.u16[(encoding >> 4 & 3) | 4] | src_copy.u16[(encoding >> 6 & 3) | 4] << 16;
}

void vec_shuffle_d128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst, uint8_t encoding) {
    union xmm_reg src_copy = *src;
    for (int i = 0; i < 4; i++)
        dst->u32[i] = src_copy.u32[(encoding >> (i*2)) % 4];
}
void vec_shuffle_ps128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst, uint8_t encoding) {
    // The low two lanes come from dst, the high two from src. Snapshot both
    // first: writing dst->u32[0] before reading the lane-1 selector (which may
    // point back at lane 0) otherwise corrupts the result, and src may alias dst.
    union xmm_reg d = *dst, s = *src;
    dst->u32[0] = d.u32[(encoding >> 0) & 3];
    dst->u32[1] = d.u32[(encoding >> 2) & 3];
    dst->u32[2] = s.u32[(encoding >> 4) & 3];
    dst->u32[3] = s.u32[(encoding >> 6) & 3];
}
void vec_shuffle_pd128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst, uint8_t encoding) {
    union xmm_reg d = *dst, s = *src;
    dst->qw[0] = d.qw[(encoding >> 0) & 1];
    dst->qw[1] = s.qw[(encoding >> 1) & 1];
}

void vec_movmask_b128(NO_CPU, const union xmm_reg *src, uint32_t *dst) {
    *dst = 0;
    for (unsigned i = 0; i < array_size(src->u8); i++) {
        if (src->u8[i] & (1 << 7))
            *dst |= 1 << i;
    }
}
void vec_fmovmask_s128(NO_CPU, const union xmm_reg *src, uint32_t *dst) {
    *dst = 0;
    for (unsigned i = 0; i < array_size(src->f32); i++) {
        if (signbit(src->f32[i]))
            *dst |= 1 << i;
    }
}
void vec_fmovmask_d128(NO_CPU, const union xmm_reg *src, uint32_t *dst) {
    *dst = 0;
    for (unsigned i = 0; i < array_size(src->f64); i++) {
        if (signbit(src->f64[i]))
            *dst |= 1 << i;
    }
}

void vec_movl_p64(NO_CPU, const uint64_t *src, union xmm_reg *dst) {
    dst->qw[0] = *src;
}
void vec_movl_pm64(NO_CPU, const union xmm_reg *src, uint64_t *dst) {
    *dst = src->qw[0];
}
void vec_movh_p64(NO_CPU, const uint64_t *src, union xmm_reg *dst) {
    dst->qw[1] = *src;
}
void vec_movh_pm64(NO_CPU, const union xmm_reg *src, uint64_t *dst) {
    *dst = src->qw[1];
}

void vec_insert_w128(NO_CPU, const uint32_t *src, union xmm_reg *dst, uint8_t index) {
    dst->u16[index % 8] = (uint16_t)*src;
}
void vec_extract_w128(NO_CPU, const union xmm_reg *src, uint32_t *dst, uint8_t index) {
    *dst = src->u16[index % 8];
}

void vec_avg_b128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    for (unsigned i = 0; i < 16; i++)
        dst->u8[i] = (1 + dst->u8[i] + src->u8[i]) >> 1;
}
void vec_avg_w128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    for (unsigned i = 0; i < 8; i++)
        dst->u16[i] = (1 + dst->u16[i] + src->u16[i]) >> 1;
}

void vec_mull128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    for (int i = 0; i < 8; i++) {
        dst->u16[i] = (uint16_t)(dst->u16[i] * src->u16[i]);
    }
}

void vec_mulu128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    for (int i = 0; i < 8; i++) {
        uint32_t res = ((int16_t)dst->u16[i] * (int16_t)src->u16[i]);
        dst->u16[i] = ((res >> 16) & 0xffff);
    }
}
void vec_muluu128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    for (int i = 0; i < 8; i++) {
        uint32_t res = dst->u16[i] * src->u16[i];
        dst->u16[i] = ((res >> 16) & 0xffff);
    }
}

// ---------------------------------------------------------------------------
// SSSE3 / SSE4.1 (three-byte 0F 38 / 0F 3A opcodes).
//
// These are the helpers behind the i386 JIT's three-byte vector dispatch (see
// emu/decode.h). They are validated bit-exact against real Intel silicon via
// tests/remote/corpus/sse4.c. The `z` numeric suffix is the memory-operand
// access width the gadget uses for that op (the source width for the widening
// pmovsx/pmovzx moves, the destination width for byte/word extracts), NOT the
// xmm register width -- which is always 128.
// ---------------------------------------------------------------------------

// pinsrd: insert a dword (from r/m32) into lane (imm & 3). dst is always xmm.
void vec_insert_d32(NO_CPU, const uint32_t *src, union xmm_reg *dst, uint8_t index) {
    dst->u32[index & 3] = *src;
}
// pinsrb: insert the low byte of r/m into byte lane (imm & 15).
void vec_insert_b8(NO_CPU, const uint8_t *src, union xmm_reg *dst, uint8_t index) {
    dst->u8[index & 15] = *src;
}
// pextrd / extractps: extract dword lane (imm & 3) to r/m32 (4-byte write is
// correct for both a memory destination and a full GP register).
void vec_extract_d32(NO_CPU, const union xmm_reg *src, uint32_t *dst, uint8_t index) {
    *dst = src->u32[index & 3];
}
// pextrb to a *memory* destination (m8): write exactly one byte.
void vec_extract_b8(NO_CPU, const union xmm_reg *src, uint8_t *dst, uint8_t index) {
    *dst = src->u8[index & 15];
}
// pextrb to a *register* destination: the byte is zero-extended to 32 bits.
void vec_extract_b_reg128(NO_CPU, const union xmm_reg *src, uint32_t *dst, uint8_t index) {
    *dst = src->u8[index & 15];
}
// pextrw (0F 3A 15) to a *memory* destination (m16): write exactly two bytes.
// (The register-destination form reuses vec_extract_w128, which zero-extends.)
void vec_extract_w_mem16(NO_CPU, const union xmm_reg *src, uint16_t *dst, uint8_t index) {
    *dst = src->u16[index & 7];
}

// palignr (SSSE3): concatenate dst:src (dst high, src low) into 32 bytes, shift
// right by `shift` bytes, take the low 16 into dst. shift >= 32 -> all zero.
void vec_palignr128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst, uint8_t shift) {
    uint8_t tmp[32];
    memcpy(&tmp[0], src->u8, 16);
    memcpy(&tmp[16], dst->u8, 16);
    union xmm_reg res;
    for (int i = 0; i < 16; i++) {
        int idx = i + shift;
        res.u8[i] = idx < 32 ? tmp[idx] : 0;
    }
    *dst = res;
}

// pshufb (SSSE3): dst (=reg) is BOTH the byte source and the destination; the
// control vector is r/m. result[i] = ctrl[i]&0x80 ? 0 : dst_orig[ctrl[i]&0xF].
void vec_pshufb128(NO_CPU, const union xmm_reg *control, union xmm_reg *dst) {
    union xmm_reg orig = *dst;
    union xmm_reg res;
    for (int i = 0; i < 16; i++)
        res.u8[i] = (control->u8[i] & 0x80) ? 0 : orig.u8[control->u8[i] & 0x0f];
    *dst = res;
}

// pabsb/w/d (SSSE3): dst = |src| per lane (signed).
void vec_pabsb128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    for (int i = 0; i < 16; i++) { int8_t v = (int8_t)src->u8[i]; dst->u8[i] = v < 0 ? -v : v; }
}
void vec_pabsw128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    for (int i = 0; i < 8; i++) { int16_t v = (int16_t)src->u16[i]; dst->u16[i] = v < 0 ? -v : v; }
}
void vec_pabsd128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    for (int i = 0; i < 4; i++) { int32_t v = (int32_t)src->u32[i]; dst->u32[i] = v < 0 ? -(uint32_t)v : (uint32_t)v; }
}

// pmovsx/pmovzx (SSE4.1): sign-/zero-extend the low packed elements of the
// source. The `z` suffix is the source width consumed (so a memory source
// reads exactly that many bytes).
void vec_pmovzxbw64(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    union xmm_reg r; for (int i = 0; i < 8; i++) r.u16[i] = src->u8[i]; *dst = r;
}
void vec_pmovsxbw64(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    union xmm_reg r; for (int i = 0; i < 8; i++) r.u16[i] = (int16_t)(int8_t)src->u8[i]; *dst = r;
}
void vec_pmovzxbd32(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    union xmm_reg r; for (int i = 0; i < 4; i++) r.u32[i] = src->u8[i]; *dst = r;
}
void vec_pmovsxbd32(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    union xmm_reg r; for (int i = 0; i < 4; i++) r.u32[i] = (int32_t)(int8_t)src->u8[i]; *dst = r;
}
void vec_pmovzxbq16(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    union xmm_reg r; for (int i = 0; i < 2; i++) r.qw[i] = src->u8[i]; *dst = r;
}
void vec_pmovsxbq16(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    union xmm_reg r; for (int i = 0; i < 2; i++) r.qw[i] = (uint64_t)(int64_t)(int8_t)src->u8[i]; *dst = r;
}
void vec_pmovzxwd64(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    union xmm_reg r; for (int i = 0; i < 4; i++) r.u32[i] = src->u16[i]; *dst = r;
}
void vec_pmovsxwd64(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    union xmm_reg r; for (int i = 0; i < 4; i++) r.u32[i] = (int32_t)(int16_t)src->u16[i]; *dst = r;
}
void vec_pmovzxwq32(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    union xmm_reg r; for (int i = 0; i < 2; i++) r.qw[i] = src->u16[i]; *dst = r;
}
void vec_pmovsxwq32(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    union xmm_reg r; for (int i = 0; i < 2; i++) r.qw[i] = (uint64_t)(int64_t)(int16_t)src->u16[i]; *dst = r;
}
void vec_pmovzxdq64(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    union xmm_reg r; for (int i = 0; i < 2; i++) r.qw[i] = src->u32[i]; *dst = r;
}
void vec_pmovsxdq64(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    union xmm_reg r; for (int i = 0; i < 2; i++) r.qw[i] = (uint64_t)(int64_t)(int32_t)src->u32[i]; *dst = r;
}

// pmulld (SSE4.1): packed 32-bit low product.
void vec_pmulld128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    for (int i = 0; i < 4; i++) dst->u32[i] = (uint32_t)(dst->u32[i] * src->u32[i]);
}
// pmuldq (SSE4.1): signed 32x32 -> 64 on lanes 0 and 2 -> qwords 0 and 1.
void vec_pmuldq128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    int64_t r0 = (int64_t)(int32_t)dst->u32[0] * (int64_t)(int32_t)src->u32[0];
    int64_t r1 = (int64_t)(int32_t)dst->u32[2] * (int64_t)(int32_t)src->u32[2];
    dst->qw[0] = (uint64_t)r0; dst->qw[1] = (uint64_t)r1;
}
// pcmpeqq (SSE4.1) / pcmpgtq (SSE4.2): packed 64-bit compare -> all-ones/zero.
void vec_pcmpeqq128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    for (int i = 0; i < 2; i++) dst->qw[i] = dst->qw[i] == src->qw[i] ? ~0ULL : 0;
}
void vec_pcmpgtq128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    for (int i = 0; i < 2; i++) dst->qw[i] = (int64_t)dst->qw[i] > (int64_t)src->qw[i] ? ~0ULL : 0;
}
// packusdw (SSE4.1): pack signed dwords (dst then src) to unsigned words, sat.
static inline uint16_t satusw(int32_t v) {
    if (v < 0) return 0;
    if (v > 0xffff) return 0xffff;
    return (uint16_t)v;
}
void vec_packusdw128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    union xmm_reg r;
    for (int i = 0; i < 4; i++) r.u16[i]     = satusw((int32_t)dst->u32[i]);
    for (int i = 0; i < 4; i++) r.u16[4 + i] = satusw((int32_t)src->u32[i]);
    *dst = r;
}

// pmin*/pmax* (SSE4.1): the variants SSE2 lacks (signed byte, signed/unsigned
// dword, unsigned word). result keeps dst when the predicate holds, else src.
#define VEC_MINMAX(name, lane, ctype, cmp) \
    void vec_##name##128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) { \
        for (unsigned i = 0; i < array_size(dst->lane); i++) \
            dst->lane[i] = (ctype)dst->lane[i] cmp (ctype)src->lane[i] ? dst->lane[i] : src->lane[i]; \
    }
VEC_MINMAX(pminsb, u8,  int8_t,   <)
VEC_MINMAX(pmaxsb, u8,  int8_t,   >)
VEC_MINMAX(pminuw, u16, uint16_t, <)
VEC_MINMAX(pmaxuw, u16, uint16_t, >)
VEC_MINMAX(pminsd, u32, int32_t,  <)
VEC_MINMAX(pmaxsd, u32, int32_t,  >)
VEC_MINMAX(pminud, u32, uint32_t, <)
VEC_MINMAX(pmaxud, u32, uint32_t, >)
#undef VEC_MINMAX

// ptest (SSE4.1): ZF = (DEST & SRC)==0; CF = (SRC & ~DEST)==0; clears the rest.
// DEST is the reg operand (dst), SRC is r/m (src). Sets eager flags directly.
void vec_ptest128(struct cpu_state *cpu, const union xmm_reg *src, const union xmm_reg *dst) {
    unsigned __int128 s = src->u128, d = dst->u128;
    cpu->zf_res = cpu->pf_res = cpu->sf_res = 0;
    cpu->af_ops = 0;
    cpu->zf = (d & s) == 0;
    cpu->cf = (s & ~d) == 0;
    cpu->pf = cpu->sf = cpu->af = cpu->of = 0;
    cpu->cf_bit = cpu->cf;
    cpu->of_bit = 0;
}

// pblendvb / blendvps / blendvpd (SSE4.1): variable blend, mask is implicit
// XMM0 (high bit of each byte/dword/qword lane). Snapshot the mask first so a
// destination that aliases XMM0 stays correct.
void vec_pblendvb128(struct cpu_state *cpu, const union xmm_reg *src, union xmm_reg *dst) {
    union xmm_reg mask = cpu->xmm[0];
    for (int i = 0; i < 16; i++) if (mask.u8[i] & 0x80) dst->u8[i] = src->u8[i];
}
void vec_blendvps128(struct cpu_state *cpu, const union xmm_reg *src, union xmm_reg *dst) {
    union xmm_reg mask = cpu->xmm[0];
    for (int i = 0; i < 4; i++) if (mask.u32[i] & 0x80000000u) dst->u32[i] = src->u32[i];
}
void vec_blendvpd128(struct cpu_state *cpu, const union xmm_reg *src, union xmm_reg *dst) {
    union xmm_reg mask = cpu->xmm[0];
    for (int i = 0; i < 2; i++) if (mask.qw[i] & 0x8000000000000000ULL) dst->qw[i] = src->qw[i];
}
// blendps / blendpd / pblendw (SSE4.1): imm-controlled blend (set bit -> src).
void vec_blend_ps128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst, uint8_t imm) {
    for (int i = 0; i < 4; i++) if (imm & (1 << i)) dst->u32[i] = src->u32[i];
}
void vec_blend_pd128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst, uint8_t imm) {
    for (int i = 0; i < 2; i++) if (imm & (1 << i)) dst->qw[i] = src->qw[i];
}
void vec_blend_w128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst, uint8_t imm) {
    for (int i = 0; i < 8; i++) if (imm & (1 << i)) dst->u16[i] = src->u16[i];
}

// roundps/pd/ss/sd (SSE4.1): imm8[1:0] picks the mode when imm8[2]==0
// (0 nearest-even, 1 floor, 2 ceil, 3 trunc); imm8[2]=1 means "use MXCSR",
// which iSH models as round-nearest-even. Scalar forms leave the upper lanes.
static inline float dc_round_f32(float x, uint8_t imm) {
    if (imm & 0x4) return rintf(x);
    switch (imm & 3) { case 1: return floorf(x); case 2: return ceilf(x); case 3: return truncf(x); default: return rintf(x); }
}
static inline double dc_round_f64(double x, uint8_t imm) {
    if (imm & 0x4) return rint(x);
    switch (imm & 3) { case 1: return floor(x); case 2: return ceil(x); case 3: return trunc(x); default: return rint(x); }
}
void vec_round_ps128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst, uint8_t imm) {
    union xmm_reg r; for (int i = 0; i < 4; i++) r.f32[i] = dc_round_f32(src->f32[i], imm); *dst = r;
}
void vec_round_pd128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst, uint8_t imm) {
    union xmm_reg r; for (int i = 0; i < 2; i++) r.f64[i] = dc_round_f64(src->f64[i], imm); *dst = r;
}
void vec_round_ss32(NO_CPU, const union xmm_reg *src, union xmm_reg *dst, uint8_t imm) {
    dst->f32[0] = dc_round_f32(src->f32[0], imm);
}
void vec_round_sd64(NO_CPU, const union xmm_reg *src, union xmm_reg *dst, uint8_t imm) {
    dst->f64[0] = dc_round_f64(src->f64[0], imm);
}
