//
//  mmx.c
//  libISH_emu
//
//  Created by Jason Conway on 02/02/23.
//

#include <math.h>
#include <string.h>

#include "emu/vec.h"
#include "emu/cpu.h"

union vec {
    uint64_t qw;
    uint8_t u8[8];
    uint16_t u16[4];
    uint32_t u32[2];
    uint64_t u64[1];
};

#define VEC_MMX_OP(name, suffix, op, size) \
    void vec_##name##_##suffix##64(NO_CPU, const union mm_reg *src, union mm_reg *dst) { \
        union vec s = { .qw = src->qw }, d = { .qw = dst->qw }; \
        for (unsigned i = 0; i < array_size(s.u##size); i++) \
            d.u##size[i] op##= s.u##size[i]; \
        dst->qw = d.qw; \
    }

#define _VEC_MMX_CMP(sgn, usgn, suffix, relop, size) \
    void vec_compare##sgn##_##suffix##64(NO_CPU, const union mm_reg *src, union mm_reg *dst) { \
        union vec s = { .qw = src->qw }, d = { .qw = dst->qw }; \
        for (unsigned i = 0; i < array_size(s.u##size); i++) \
            d.u##size[i] = (usgn##int##size##_t)d.u##size[i] relop (usgn##int##size##_t)s.u##size[i] ? ~0 : 0;\
        dst->qw = d.qw; \
    }

#define _SHIFT(op, size) \
    do { \
        if (unlikely(amount > (size)-1)) { \
            dst->qw = 0; \
        } else { \
            union vec d = { .qw = dst->qw }; \
            for (unsigned i = 0; i < array_size(d.u##size); i++) \
                d.u##size[i] op##= amount; \
            dst->qw = d.qw; \
        } \
    } while (0)

#define VEC_MMX_SHIFT(dir, suffix, op, size) \
    void vec_shift##dir##_##suffix##64(NO_CPU, const union mm_reg *src, union mm_reg *dst) { \
        const uint8_t amount = src->qw; \
        _SHIFT(op, size); \
    } \
    void vec_imm_shift##dir##_##suffix##64(NO_CPU, const uint8_t amount, union mm_reg *dst) { \
        _SHIFT(op, size); \
    }

#define VEC_MMX_CMPD(suffix, relop, size) \
    _VEC_MMX_CMP(, u, suffix, relop, size)
#define VEC_MMX_CMPS(suffix, relop, size) \
    _VEC_MMX_CMP(s,, suffix, relop, size)

VEC_MMX_OP(add, b, +, 8)
VEC_MMX_OP(add, w, +, 16)
VEC_MMX_OP(add, d, +, 32)
VEC_MMX_OP(add, q, +, 64)

VEC_MMX_OP(sub, b, -, 8)
VEC_MMX_OP(sub, w, -, 16)
VEC_MMX_OP(sub, d, -, 32)
VEC_MMX_OP(sub, q, -, 64)

VEC_MMX_OP(and, q, &, 64)
VEC_MMX_OP(or,  q, |, 64)
VEC_MMX_OP(xor, q, ^, 64)

// pandn (0F DF): dst = ~dst & src. Unlike pand/por/pxor this is not a plain
// accumulate, so it can't use VEC_MMX_OP; mirror the 128-bit vec_andn128 at
// 64-bit MMX width. (libgcrypt SHA uses MMX pandn -> gpgv SIGILL without it.)
void vec_andn64(NO_CPU, const union mm_reg *src, union mm_reg *dst) {
    dst->qw = ~dst->qw & src->qw;
}

// ---- packed-integer MMX ops the original decoder omitted (0F 60/61/63/67/68/
// 69/6a/6b, d8-ee, f5/f6, no-66 forms). Each mirrors its 128-bit vec_*128
// sibling in vec.c at 64-bit MMX width so the results stay bit-exact with real
// silicon (the XMM siblings are validated vs Intel). Saturation helpers are
// copied from vec.c (file-local statics there). maskmovq (0F F7) is omitted on
// purpose: it is a masked store to DS:[e/rDI] and is essentially never used. ----
static inline int32_t mmx_satsw(int32_t dw) {     // signed word -> signed byte
    if (dw > 0xff80) dw &= 0xff;
    else if (dw > 0x7fff) dw = 0x80;
    else if (dw > 0x7f) dw = 0x7f;
    return dw;
}
static inline uint32_t mmx_satud(uint32_t dw) {   // signed dword -> signed word
    if (dw > 0xffff8000) dw &= 0xffff;
    else if (dw > 0x7fffffff) dw = 0x8000;
    else if (dw > 0x7fff) dw = 0x7fff;
    return dw;
}
static inline uint32_t mmx_satub(uint32_t dw) {   // signed word -> unsigned byte
    if (dw >= 0x8000) dw = 0;
    else if (dw > 0xff) dw = 0xff;
    return dw;
}
static inline uint32_t mmx_satsb(uint32_t dw) {   // signed -> signed byte
    if (dw > 0xffffff80) dw &= 0xff;
    else if (dw > 0x7fffffff) dw = 0x80;
    else if (dw > 0x7f) dw = 0x7f;
    return dw;
}

// punpck{l,h}{bw,wd,dq}: interleave bytes/words/dwords from dst and src. Build
// the result in a temp so an in-place src==dst (e.g. punpcklbw mm0,mm0) works.
void vec_unpackl_bw64(NO_CPU, const union mm_reg *src, union mm_reg *dst) {
    union vec s = { .qw = src->qw }, d = { .qw = dst->qw }, r;
    for (int i = 0; i < 4; i++) { r.u8[2*i] = d.u8[i]; r.u8[2*i + 1] = s.u8[i]; }
    dst->qw = r.qw;
}
void vec_unpackl_w64(NO_CPU, const union mm_reg *src, union mm_reg *dst) {
    union vec s = { .qw = src->qw }, d = { .qw = dst->qw }, r;
    for (int i = 0; i < 2; i++) { r.u16[2*i] = d.u16[i]; r.u16[2*i + 1] = s.u16[i]; }
    dst->qw = r.qw;
}
void vec_unpackh_bw64(NO_CPU, const union mm_reg *src, union mm_reg *dst) {
    union vec s = { .qw = src->qw }, d = { .qw = dst->qw }, r;
    for (int i = 0; i < 4; i++) { r.u8[2*i] = d.u8[i + 4]; r.u8[2*i + 1] = s.u8[i + 4]; }
    dst->qw = r.qw;
}
void vec_unpackh_w64(NO_CPU, const union mm_reg *src, union mm_reg *dst) {
    union vec s = { .qw = src->qw }, d = { .qw = dst->qw }, r;
    for (int i = 0; i < 2; i++) { r.u16[2*i] = d.u16[i + 2]; r.u16[2*i + 1] = s.u16[i + 2]; }
    dst->qw = r.qw;
}
void vec_unpackh_d64(NO_CPU, const union mm_reg *src, union mm_reg *dst) {
    union vec s = { .qw = src->qw }, d = { .qw = dst->qw }, r;
    r.u32[0] = d.u32[1]; r.u32[1] = s.u32[1];
    dst->qw = r.qw;
}

// packsswb/packssdw (signed) and packuswb (unsigned): saturate-pack dst then src.
void vec_packss_w64(NO_CPU, const union mm_reg *src, union mm_reg *dst) {
    union vec s = { .qw = src->qw }, d = { .qw = dst->qw }, r;
    for (int i = 0; i < 4; i++) r.u8[i]     = (uint8_t) mmx_satsw(d.u16[i]);
    for (int i = 0; i < 4; i++) r.u8[4 + i] = (uint8_t) mmx_satsw(s.u16[i]);
    dst->qw = r.qw;
}
void vec_packss_d64(NO_CPU, const union mm_reg *src, union mm_reg *dst) {
    union vec s = { .qw = src->qw }, d = { .qw = dst->qw }, r;
    r.u16[0] = (uint16_t) mmx_satud(d.u32[0]);
    r.u16[1] = (uint16_t) mmx_satud(d.u32[1]);
    r.u16[2] = (uint16_t) mmx_satud(s.u32[0]);
    r.u16[3] = (uint16_t) mmx_satud(s.u32[1]);
    dst->qw = r.qw;
}
void vec_packsu_w64(NO_CPU, const union mm_reg *src, union mm_reg *dst) {
    union vec s = { .qw = src->qw }, d = { .qw = dst->qw }, r;
    for (int i = 0; i < 4; i++) r.u8[i]     = (uint8_t) mmx_satub(d.u16[i]);
    for (int i = 0; i < 4; i++) r.u8[4 + i] = (uint8_t) mmx_satub(s.u16[i]);
    dst->qw = r.qw;
}

// Saturating add/sub: unsigned (us) and signed (ss).
void vec_addus_b64(NO_CPU, const union mm_reg *src, union mm_reg *dst) {
    union vec s = { .qw = src->qw }, d = { .qw = dst->qw };
    for (unsigned i = 0; i < 8; i++) { int32_t v = d.u8[i] + s.u8[i]; d.u8[i] = v > 0xff ? 0xff : v; }
    dst->qw = d.qw;
}
void vec_addus_w64(NO_CPU, const union mm_reg *src, union mm_reg *dst) {
    union vec s = { .qw = src->qw }, d = { .qw = dst->qw };
    for (unsigned i = 0; i < 4; i++) { int32_t v = d.u16[i] + s.u16[i]; d.u16[i] = v > 0xffff ? 0xffff : v; }
    dst->qw = d.qw;
}
void vec_subus_b64(NO_CPU, const union mm_reg *src, union mm_reg *dst) {
    union vec s = { .qw = src->qw }, d = { .qw = dst->qw };
    for (unsigned i = 0; i < 8; i++) { int32_t v = d.u8[i] - s.u8[i]; d.u8[i] = v < 0 ? 0 : v; }
    dst->qw = d.qw;
}
void vec_subus_w64(NO_CPU, const union mm_reg *src, union mm_reg *dst) {
    union vec s = { .qw = src->qw }, d = { .qw = dst->qw };
    for (unsigned i = 0; i < 4; i++) { int32_t v = d.u16[i] - s.u16[i]; d.u16[i] = v < 0 ? 0 : v; }
    dst->qw = d.qw;
}
void vec_addss_b64(NO_CPU, const union mm_reg *src, union mm_reg *dst) {
    union vec s = { .qw = src->qw }, d = { .qw = dst->qw };
    for (unsigned i = 0; i < 8; i++) d.u8[i] = mmx_satsb((int8_t)d.u8[i] + (int8_t)s.u8[i]);
    dst->qw = d.qw;
}
void vec_addss_w64(NO_CPU, const union mm_reg *src, union mm_reg *dst) {
    union vec s = { .qw = src->qw }, d = { .qw = dst->qw };
    for (unsigned i = 0; i < 4; i++) d.u16[i] = mmx_satud((int16_t)d.u16[i] + (int16_t)s.u16[i]);
    dst->qw = d.qw;
}
void vec_subss_b64(NO_CPU, const union mm_reg *src, union mm_reg *dst) {
    union vec s = { .qw = src->qw }, d = { .qw = dst->qw };
    for (unsigned i = 0; i < 8; i++) d.u8[i] = mmx_satsb((int8_t)d.u8[i] - (int8_t)s.u8[i]);
    dst->qw = d.qw;
}
void vec_subss_w64(NO_CPU, const union mm_reg *src, union mm_reg *dst) {
    union vec s = { .qw = src->qw }, d = { .qw = dst->qw };
    for (unsigned i = 0; i < 4; i++) d.u16[i] = mmx_satud((int16_t)d.u16[i] - (int16_t)s.u16[i]);
    dst->qw = d.qw;
}

// Unsigned byte min/max, signed word min/max.
void vec_min_ub64(NO_CPU, const union mm_reg *src, union mm_reg *dst) {
    union vec s = { .qw = src->qw }, d = { .qw = dst->qw };
    for (unsigned i = 0; i < 8; i++) if (s.u8[i] < d.u8[i]) d.u8[i] = s.u8[i];
    dst->qw = d.qw;
}
void vec_max_ub64(NO_CPU, const union mm_reg *src, union mm_reg *dst) {
    union vec s = { .qw = src->qw }, d = { .qw = dst->qw };
    for (unsigned i = 0; i < 8; i++) if (s.u8[i] > d.u8[i]) d.u8[i] = s.u8[i];
    dst->qw = d.qw;
}
void vec_mins_w64(NO_CPU, const union mm_reg *src, union mm_reg *dst) {
    union vec s = { .qw = src->qw }, d = { .qw = dst->qw };
    for (unsigned i = 0; i < 4; i++) d.u16[i] = (int16_t)d.u16[i] < (int16_t)s.u16[i] ? d.u16[i] : s.u16[i];
    dst->qw = d.qw;
}
void vec_maxs_w64(NO_CPU, const union mm_reg *src, union mm_reg *dst) {
    union vec s = { .qw = src->qw }, d = { .qw = dst->qw };
    for (unsigned i = 0; i < 4; i++) d.u16[i] = (int16_t)d.u16[i] > (int16_t)s.u16[i] ? d.u16[i] : s.u16[i];
    dst->qw = d.qw;
}

// Unsigned byte/word rounding average.
void vec_avg_b64(NO_CPU, const union mm_reg *src, union mm_reg *dst) {
    union vec s = { .qw = src->qw }, d = { .qw = dst->qw };
    for (unsigned i = 0; i < 8; i++) d.u8[i] = (1 + d.u8[i] + s.u8[i]) >> 1;
    dst->qw = d.qw;
}
void vec_avg_w64(NO_CPU, const union mm_reg *src, union mm_reg *dst) {
    union vec s = { .qw = src->qw }, d = { .qw = dst->qw };
    for (unsigned i = 0; i < 4; i++) d.u16[i] = (1 + d.u16[i] + s.u16[i]) >> 1;
    dst->qw = d.qw;
}

// pmulhuw: high 16 bits of the unsigned 16x16 product.
void vec_muluu64(NO_CPU, const union mm_reg *src, union mm_reg *dst) {
    union vec s = { .qw = src->qw }, d = { .qw = dst->qw };
    for (unsigned i = 0; i < 4; i++) { uint32_t res = (uint32_t)d.u16[i] * s.u16[i]; d.u16[i] = (res >> 16) & 0xffff; }
    dst->qw = d.qw;
}

// pmaddwd: dword[i] = d[2i]*s[2i] + d[2i+1]*s[2i+1] (signed 16x16 products).
void vec_madd_d64(NO_CPU, const union mm_reg *src, union mm_reg *dst) {
    union vec s = { .qw = src->qw }, d = { .qw = dst->qw }, r;
    r.u32[0] = (int32_t)((int16_t)d.u16[0] * (int16_t)s.u16[0]) +
               (int32_t)((int16_t)d.u16[1] * (int16_t)s.u16[1]);
    r.u32[1] = (int32_t)((int16_t)d.u16[2] * (int16_t)s.u16[2]) +
               (int32_t)((int16_t)d.u16[3] * (int16_t)s.u16[3]);
    dst->qw = r.qw;
}

// psadbw: sum of |d[i]-s[i]| over the 8 bytes, written to the low word; rest 0.
void vec_sumabs_w64(NO_CPU, const union mm_reg *src, union mm_reg *dst) {
    union vec s = { .qw = src->qw }, d = { .qw = dst->qw };
    uint32_t sum = 0;
    for (unsigned i = 0; i < 8; i++) { int32_t diff = d.u8[i] - s.u8[i]; sum += diff < 0 ? -(uint32_t)diff : diff; }
    dst->qw = sum;
}

VEC_MMX_CMPD(eqb, ==,  8)
VEC_MMX_CMPD(eqw, ==, 16)
VEC_MMX_CMPD(eqd, ==, 32)

VEC_MMX_CMPS(gtb, >,  8)
VEC_MMX_CMPS(gtw, >, 16)
VEC_MMX_CMPS(gtd, >, 32)

VEC_MMX_SHIFT(r, w, >>, 16)
VEC_MMX_SHIFT(r, d, >>, 32)
VEC_MMX_SHIFT(r, q, >>, 64)

VEC_MMX_SHIFT(l, w, <<, 16)
VEC_MMX_SHIFT(l, d, <<, 32)
VEC_MMX_SHIFT(l, q, <<, 64)

void vec_shiftrs_w64(NO_CPU, const union mm_reg *src, union mm_reg *dst) {
    union vec d = { .qw = dst->qw };
    const uint8_t amount = src->qw;
    for (unsigned i = 0; i < 4; i++) {
        if (amount > 15)
            d.u16[i] = ((d.u16[i] >> 15) & (uint16_t)1) ? 0xffff : 0;
        else
            d.u16[i] = ((int16_t)(d.u16[i])) >> amount;
    }
    dst->qw = d.qw;
}
void vec_shiftrs_d64(NO_CPU, const union mm_reg *src, union mm_reg *dst) {
    union vec d = { .qw = dst->qw };
    const uint8_t amount = src->qw;
    for (unsigned i = 0; i < 2; i++) {
        if (amount > 31)
            d.u32[i] = ((d.u32[i] >> 31) & (uint32_t)1) ? 0xffffffff : 0;
        else
            d.u32[i] = ((int32_t)(d.u32[i])) >> amount;
    }
    dst->qw = d.qw;
}
void vec_imm_shiftrs_w64(NO_CPU, const uint8_t amount, union mm_reg *dst) {
    union vec d = { .qw = dst->qw };
    for (unsigned i = 0; i < 4; i++) {
        if (amount > 15)
            d.u16[i] = ((d.u16[i] >> 15) & (uint16_t)1) ? 0xffff : 0;
        else
            d.u16[i] = ((int16_t)(d.u16[i])) >> amount;
    }
    dst->qw = d.qw;
}
void vec_imm_shiftrs_d64(NO_CPU, const uint8_t amount, union mm_reg *dst) {
    union vec d = { .qw = dst->qw };
    for (unsigned i = 0; i < 2; i++) {
        if (amount > 31)
            d.u32[i] = ((d.u32[i] >> 31) & (uint32_t)1) ? 0xffffffff : 0;
        else
            d.u32[i] = ((int32_t)(d.u32[i])) >> amount;
    }
    dst->qw = d.qw;
}

void vec_mulu64(NO_CPU, const union mm_reg *src, union mm_reg *dst) {
    union vec s = { .qw = src->qw }, d = { .qw = dst->qw };
    for (unsigned i = 0; i < 4; i++) {
        uint32_t res = ((int16_t)d.u16[i] * (int16_t)s.u16[i]);
        d.u16[i] = ((res >> 16) & 0xffff);
    }
    dst->qw = d.qw;
}
void vec_mull64(NO_CPU, const union mm_reg *src, union mm_reg *dst) {
    union vec s = { .qw = src->qw }, d = { .qw = dst->qw };
    for (int i = 0; i < 4; i++) {
        d.u16[i] = (uint16_t)(d.u16[i] * s.u16[i]);
    }
    dst->qw = d.qw;
}
void vec_mulu_dq64(NO_CPU, union mm_reg *src, union mm_reg *dst) {
    dst->qw = (uint64_t) src->dw[0] * dst->dw[0];
}

void vec_unpackl_dq64(NO_CPU, const union mm_reg *src, union mm_reg *dst) {
    dst->dw[1] = src->dw[0];
}

void vec_shuffle_w64(NO_CPU, const union mm_reg *src, union mm_reg *dst, uint8_t encoding) {
    union vec s = { .qw = src->qw }, d = { .qw = dst->qw };
    for (unsigned i = 0; i < 4; i++)
        d.u16[i] = s.u16[(encoding >> (2 * i)) % 4];
    dst->qw = d.qw;
}

void vec_movmask_b64(NO_CPU, const union mm_reg *src, uint32_t *dst) {
    union vec s = { .qw = src->qw };
    *dst = 0;
    for (unsigned i = 0; i < array_size(s.u8); i++) {
        if (s.u8[i] & (1 << 7))
            *dst |= 1 << i;
    }
}

void vec_insert_w64(NO_CPU, const uint32_t *src, union mm_reg *dst, uint8_t index) {
    union vec d = { .qw = dst->qw };
    d.u16[index % 4] = (uint16_t)*src;
    dst->qw = d.qw;
}
