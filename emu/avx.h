#ifndef EMU_AVX_H
#define EMU_AVX_H

// Arch-independent AVX/AVX2/AVX-512 instruction semantics (GH #525).
//
// These operate purely on flat byte buffers holding vector values -- they know
// nothing about CPU state, the TLB, modrm, or which guest ABI is executing.
// That keeps exactly one implementation of each instruction's meaning, shared
// by every front-end that decodes VEX/EVEX:
//
//   amd64  emu/amd64_interp.c -- decodes and executes in the interpreter,
//          which the JIT already bails to for any opcode it can't translate
//   i386   emu/decode.h / jit/gen.c -- decodes at codegen time and calls
//          these through the vec-helper gadgets, since i386 is JIT-only and
//          has no interpreter to fall back on
//
// Buffers are always at least `vlen / 8` bytes. A `vlen` of 128/256/512 is
// the operation width; helpers that are lane-local (shuffles, packs, unpacks)
// iterate 128-bit lanes internally, and the ones that deliberately cross lanes
// (the permutes) say so at their definition.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "misc.h"

// ---- lane helpers ----
//
// A "lane" here is one element (1/2/4/8 bytes). Values are moved through
// uint64_t regardless of width; avx_lane_mask/avx_lane_sext put them back
// into the right width with the right signedness.

static inline uint64_t avx_lane_get(const uint8_t *p, unsigned bytes) {
    uint64_t v = 0;
    memcpy(&v, p, bytes);
    return v;
}

static inline void avx_lane_put(uint8_t *p, unsigned bytes, uint64_t v) {
    memcpy(p, &v, bytes);
}

static inline uint64_t avx_lane_mask(unsigned bytes) {
    return bytes >= 8 ? ~UINT64_C(0) : (UINT64_C(1) << (bytes * 8)) - 1;
}

static inline int64_t avx_lane_sext(uint64_t v, unsigned bytes) {
    unsigned sh = 64 - bytes * 8;
    return ((int64_t) (v << sh)) >> sh;
}


enum avx_op {
    AVX_XOR, AVX_AND, AVX_ANDN, AVX_OR,
    AVX_ADD, AVX_SUB,
    AVX_ADDS, AVX_ADDUS, AVX_SUBS, AVX_SUBUS,
    AVX_CMPEQ, AVX_CMPGT,
    AVX_MINS, AVX_MINU, AVX_MAXS, AVX_MAXU,
    AVX_AVG, AVX_MULLO, AVX_MULHI, AVX_MULHIU,
};

enum avx_shift_kind { AVX_SHL, AVX_SHR, AVX_SAR };

enum avx_fp_op { AVX_FADD, AVX_FSUB, AVX_FMUL, AVX_FDIV, AVX_FMIN, AVX_FMAX };

void avx_binop(enum avx_op op, unsigned lb, unsigned vlen, const uint8_t *s1, const uint8_t *s2, uint8_t *d);
void avx_shift(enum avx_shift_kind kind, unsigned lb, unsigned vlen, const uint8_t *s, uint64_t count, uint8_t *d);
void avx_shift_var(enum avx_shift_kind kind, unsigned lb, unsigned vlen, const uint8_t *s, const uint8_t *counts, uint8_t *d);
void avx_byte_shift(bool left, unsigned vlen, unsigned count, const uint8_t *s, uint8_t *d);
void avx_pshufb(unsigned vlen, const uint8_t *s1, const uint8_t *s2, uint8_t *d);
void avx_pshufd(unsigned vlen, byte_t imm, const uint8_t *s, uint8_t *d);
void avx_pshufw_half(unsigned vlen, byte_t imm, bool high, const uint8_t *s, uint8_t *d);
void avx_unpack(bool high, unsigned lb, unsigned vlen, const uint8_t *s1, const uint8_t *s2, uint8_t *d);
void avx_pack(bool to_signed, unsigned src_lb, unsigned vlen, const uint8_t *s1, const uint8_t *s2, uint8_t *d);
void avx_widen(bool sign, unsigned src_lb, unsigned dst_lb, unsigned vlen, const uint8_t *s, uint8_t *d);
void avx_broadcast(unsigned lb, unsigned vlen, const uint8_t *s, uint8_t *d);
void avx_palignr(unsigned vlen, byte_t imm, const uint8_t *s1, const uint8_t *s2, uint8_t *d);
void avx_pmaddwd(unsigned vlen, const uint8_t *s1, const uint8_t *s2, uint8_t *d);
void avx_pmaddubsw(unsigned vlen, const uint8_t *s1, const uint8_t *s2, uint8_t *d);
void avx_pmuludq(unsigned vlen, const uint8_t *s1, const uint8_t *s2, uint8_t *d);
void avx_psadbw(unsigned vlen, const uint8_t *s1, const uint8_t *s2, uint8_t *d);
void avx_abs(unsigned lb, unsigned vlen, const uint8_t *s, uint8_t *d);
double avx_fp_apply(enum avx_fp_op op, double a, double b);
void avx_fp_binop(enum avx_fp_op op, bool is_double, unsigned vlen, const uint8_t *s1, const uint8_t *s2, uint8_t *d);
bool avx_fp_compare(unsigned pred, double a, double b);
void avx_fp_cmp(unsigned pred, bool is_double, unsigned vlen, const uint8_t *s1, const uint8_t *s2, uint8_t *d);
void avx_fma(unsigned form, bool is_double, bool negate_mul, bool subtract, unsigned vlen, const uint8_t *dst_in, const uint8_t *s2, const uint8_t *s3, uint8_t *d);
void avx_aes_round(bool decrypt, bool last, const uint8_t *state, const uint8_t *round_key, uint8_t *out);
void avx_pclmulqdq(unsigned vlen, byte_t imm, const uint8_t *s1, const uint8_t *s2, uint8_t *d);
void avx_gf2p8affine(unsigned vlen, byte_t imm, const uint8_t *s1, const uint8_t *s2, uint8_t *d);
void avx_vpdpbusd(unsigned vlen, const uint8_t *acc, const uint8_t *s1, const uint8_t *s2, uint8_t *d);
void avx_vpdpwssd(unsigned vlen, const uint8_t *acc, const uint8_t *s1, const uint8_t *s2, uint8_t *d);
void avx_pternlog(unsigned vlen, byte_t imm, const uint8_t *s1, const uint8_t *s2, const uint8_t *s3, uint8_t *d);

#endif
