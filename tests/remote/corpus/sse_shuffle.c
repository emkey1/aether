/*
 * sse_shuffle — differential test for SSE/SSE2 lane-shuffle + sign-mask ops:
 *   shufps/shufpd (imm-selected lanes from dst+src)
 *   unpcklps/unpckhps/unpcklpd/unpckhpd (interleave)
 *   pshufd/pshuflw/pshufhw (imm-selected dwords / low|high words, src->dst)
 *   movmskps/movmskpd/pmovmskb (extract sign bits to a GP reg)
 *
 * These are pure bit/lane moves -- no FP arithmetic -- so there is NO arm64-vs-
 * x86 divergence to canonicalize: the result bytes must be identical. A wrong
 * lane (this project has shipped a shufps/shufpd lane-corruption bug) shows up
 * as an exact byte mismatch. Shuffle results (128-bit) are emitted as four
 * dwords (lane in b); sign masks are emitted directly.
 */
#include "diff_common.h"

typedef int   v4si __attribute__((vector_size(16)));
typedef float v4sf __attribute__((vector_size(16)));

static const uint32_t XPAT[][4] = {
    {0x00010203, 0x04050607, 0x08090a0b, 0x0c0d0e0f},  /* sequential bytes */
    {0x3f800000, 0x40000000, 0x40400000, 0x40800000},  /* 1.0 2.0 3.0 4.0 */
    {0xdeadbeef, 0xcafebabe, 0x12345678, 0x9abcdef0},  /* distinct dwords */
    {0x80000000, 0x00000000, 0x7fffffff, 0xffff8000},  /* sign-bit mix */
};
#define NX (sizeof XPAT / sizeof XPAT[0])

/* binary imm shuffle: dst in/out, src in, imm literal (via %c modifier) */
#define RUN_BIN_IMM(MNEM, OP, IMM) do {                                        \
    for (size_t i = 0; i < NX; i++) for (size_t j = 0; j < NX; j++) {          \
        v4si d_, s_; uint32_t o[4]; memcpy(&d_, XPAT[i], 16); memcpy(&s_, XPAT[j], 16); \
        __asm__ volatile(MNEM " $%c[m], %[s], %[d]"                            \
            : [d] "+x"(d_) : [s] "x"(s_), [m] "i"(IMM));                       \
        memcpy(o, &d_, 16);                                                    \
        for (int k = 0; k < 4; k++)                                           \
            dc_emit(OP, 32, ((uint64_t)(IMM) << 8) | (i << 4) | j, k, o[k], 0, 0); \
    } } while (0)

/* unary imm shuffle: src -> dst (dst write-only) */
#define RUN_UN_IMM(MNEM, OP, IMM) do {                                         \
    for (size_t i = 0; i < NX; i++) {                                          \
        v4si s_, d_; uint32_t o[4]; memcpy(&s_, XPAT[i], 16);                  \
        __asm__ volatile(MNEM " $%c[m], %[s], %[d]"                            \
            : [d] "=x"(d_) : [s] "x"(s_), [m] "i"(IMM));                       \
        memcpy(o, &d_, 16);                                                    \
        for (int k = 0; k < 4; k++)                                           \
            dc_emit(OP, 32, ((uint64_t)(IMM) << 8) | i, k, o[k], 0, 0);        \
    } } while (0)

/* binary no-imm interleave: dst in/out, src in */
#define RUN_BIN(MNEM, OP) do {                                                 \
    for (size_t i = 0; i < NX; i++) for (size_t j = 0; j < NX; j++) {          \
        v4si d_, s_; uint32_t o[4]; memcpy(&d_, XPAT[i], 16); memcpy(&s_, XPAT[j], 16); \
        __asm__ volatile(MNEM " %[s], %[d]" : [d] "+x"(d_) : [s] "x"(s_));     \
        memcpy(o, &d_, 16);                                                    \
        for (int k = 0; k < 4; k++)                                           \
            dc_emit(OP, 32, (i << 4) | j, k, o[k], 0, 0);                      \
    } } while (0)

__attribute__((target("sse2")))
static void run_cases(void) {
    /* shufps imm8 (4 lanes: 2 from dst, 2 from src) */
    RUN_BIN_IMM("shufps", "shufps", 0x00); RUN_BIN_IMM("shufps", "shufps", 0x1b);
    RUN_BIN_IMM("shufps", "shufps", 0xe4); RUN_BIN_IMM("shufps", "shufps", 0xb1);
    RUN_BIN_IMM("shufps", "shufps", 0x4e); RUN_BIN_IMM("shufps", "shufps", 0xff);
    /* shufpd imm (low 2 bits: lane0 from dst, lane1 from src) */
    RUN_BIN_IMM("shufpd", "shufpd", 0x0); RUN_BIN_IMM("shufpd", "shufpd", 0x1);
    RUN_BIN_IMM("shufpd", "shufpd", 0x2); RUN_BIN_IMM("shufpd", "shufpd", 0x3);
    /* pshufd imm8 (4 dwords from src) */
    RUN_UN_IMM("pshufd", "pshufd", 0x00); RUN_UN_IMM("pshufd", "pshufd", 0x1b);
    RUN_UN_IMM("pshufd", "pshufd", 0xe4); RUN_UN_IMM("pshufd", "pshufd", 0xb1);
    RUN_UN_IMM("pshufd", "pshufd", 0x4e); RUN_UN_IMM("pshufd", "pshufd", 0xff);
    /* pshuflw / pshufhw imm8 (shuffle low|high 4 words, copy the other half) */
    RUN_UN_IMM("pshuflw", "pshuflw", 0x1b); RUN_UN_IMM("pshuflw", "pshuflw", 0xe4);
    RUN_UN_IMM("pshuflw", "pshuflw", 0xb1); RUN_UN_IMM("pshuflw", "pshuflw", 0xff);
    RUN_UN_IMM("pshufhw", "pshufhw", 0x1b); RUN_UN_IMM("pshufhw", "pshufhw", 0xe4);
    RUN_UN_IMM("pshufhw", "pshufhw", 0xb1); RUN_UN_IMM("pshufhw", "pshufhw", 0xff);
    /* interleave */
    RUN_BIN("unpcklps", "unpcklps"); RUN_BIN("unpckhps", "unpckhps");
    RUN_BIN("unpcklpd", "unpcklpd"); RUN_BIN("unpckhpd", "unpckhpd");

    /* sign-mask extract -> GP reg */
    for (size_t i = 0; i < NX; i++) {
        v4si x_; memcpy(&x_, XPAT[i], 16); uint32_t m;
        __asm__ volatile("movmskps %[x], %[m]" : [m] "=r"(m) : [x] "x"(x_));
        dc_emit("movmskps", 32, i, 0, m, 0, 0);
        __asm__ volatile("movmskpd %[x], %[m]" : [m] "=r"(m) : [x] "x"(x_));
        dc_emit("movmskpd", 32, i, 0, m, 0, 0);
        __asm__ volatile("pmovmskb %[x], %[m]" : [m] "=r"(m) : [x] "x"(x_));
        dc_emit("pmovmskb", 32, i, 0, m, 0, 0);
    }
    (void)sizeof(v4sf);
}
