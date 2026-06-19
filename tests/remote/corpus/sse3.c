/*
 * sse3 — differential test for the SSE3 (PNI) instructions: the duplicating
 * moves (movsldup/movshdup/movddup), the unaligned load (lddqu), and the
 * alternating/horizontal float add-subtracts (addsub/hadd/hsub ps&pd).
 *
 * The moves and lddqu are pure bit copies -> exact match, any bit pattern.
 * The add/sub ops use only finite inputs (like round{ps,pd} in sse4.c) so the
 * IEEE result is exact and identical across arm64 and x86 (no NaN payloads to
 * canonicalize). The emulator computes them as plain adds/subs (no FMA), which
 * matches x86 bit-for-bit. The conductor requires byte-identical lines vs
 * mint's real Intel silicon.
 */
#include "diff_common.h"

typedef int       v4si __attribute__((vector_size(16)));
typedef float     v4sf __attribute__((vector_size(16)));
typedef double    v2df __attribute__((vector_size(16)));

/* bit patterns for the moves (sign bits, zeros, sequences, equal lanes) */
static const uint32_t XPAT[][4] = {
    {0x00010203, 0x04050607, 0x08090a0b, 0x0c0d0e0f},
    {0xdeadbeef, 0xcafebabe, 0x12345678, 0x9abcdef0},
    {0x80000000, 0x7fffffff, 0x00000000, 0xffffffff},
    {0x00000001, 0xfffffffe, 0x8000ffff, 0x7fff0001},
};
#define NX (sizeof XPAT / sizeof XPAT[0])

/* finite float/double inputs for the add/sub ops (no NaN/inf -> exact) */
static const float DPF[][4] = {
    {1.0f, 2.0f, 3.0f, 4.0f}, {0.5f, -1.5f, 2.25f, -0.75f},
    {10.0f, 20.0f, 30.0f, 40.0f}, {-1.0f, -2.0f, -3.0f, -4.0f},
};
static const double DPD[][2] = {
    {1.0, 2.0}, {0.5, -1.5}, {10.0, -20.0}, {-3.25, 4.75},
};
#define ND 4

static void emit_xmm(const char *op, uint64_t key, const void *xmm) {
    const uint32_t *o = (const uint32_t *)xmm;
    for (int k = 0; k < 4; k++)
        dc_emit(op, 32, key, k, o[k], 0, 0);
}

/* unary move (src -> dst): movsldup / movshdup / movddup */
#define RUN_UN(MNEM, OP) do {                                                   \
    for (size_t i = 0; i < NX; i++) {                                           \
        v4si s_, d_; uint32_t o[4]; memcpy(&s_, XPAT[i], 16);                  \
        __asm__ volatile(MNEM " %[s], %[d]" : [d] "=x"(d_) : [s] "x"(s_));      \
        memcpy(o, &d_, 16); emit_xmm(OP, i, o);                                 \
    } } while (0)

/* binary single-precision (dst in/out, src in), finite inputs */
#define RUN_PS(MNEM, OP) do {                                                   \
    for (size_t i = 0; i < ND; i++) for (size_t j = 0; j < ND; j++) {           \
        v4sf d_, s_; uint32_t o[4];                                            \
        memcpy(&d_, DPF[i], 16); memcpy(&s_, DPF[j], 16);                      \
        __asm__ volatile(MNEM " %[s], %[d]" : [d] "+x"(d_) : [s] "x"(s_));      \
        memcpy(o, &d_, 16); emit_xmm(OP, (i << 4) | j, o);                      \
    } } while (0)

/* binary double-precision (dst in/out, src in), finite inputs */
#define RUN_PD(MNEM, OP) do {                                                   \
    for (size_t i = 0; i < ND; i++) for (size_t j = 0; j < ND; j++) {           \
        v2df d_, s_; uint32_t o[4];                                            \
        memcpy(&d_, DPD[i], 16); memcpy(&s_, DPD[j], 16);                      \
        __asm__ volatile(MNEM " %[s], %[d]" : [d] "+x"(d_) : [s] "x"(s_));      \
        memcpy(o, &d_, 16); emit_xmm(OP, (i << 4) | j, o);                      \
    } } while (0)

__attribute__((target("sse3")))
static void run_cases(void) {
    /* ---- duplicating moves (register form) ---- */
    RUN_UN("movsldup", "movsldup");
    RUN_UN("movshdup", "movshdup");
    RUN_UN("movddup", "movddup");

    /* ---- duplicating moves (memory form: movddup reads m64, the others m128) ---- */
    for (size_t i = 0; i < NX; i++) {
        v4si d_; uint32_t o[4];
        uint64_t m64 = ((uint64_t)XPAT[i][1] << 32) | XPAT[i][0];
        __asm__ volatile("movddup %[s], %[d]" : [d] "=x"(d_) : [s] "m"(m64));
        memcpy(o, &d_, 16); emit_xmm("movddupm", i, o);
        v4si mbuf; memcpy(&mbuf, XPAT[i], 16);
        __asm__ volatile("movsldup %[s], %[d]" : [d] "=x"(d_) : [s] "m"(mbuf));
        memcpy(o, &d_, 16); emit_xmm("movsldupm", i, o);
        __asm__ volatile("movshdup %[s], %[d]" : [d] "=x"(d_) : [s] "m"(mbuf));
        memcpy(o, &d_, 16); emit_xmm("movshdupm", i, o);
        /* lddqu: unaligned 128-bit load */
        __asm__ volatile("lddqu %[s], %[d]" : [d] "=x"(d_) : [s] "m"(mbuf));
        memcpy(o, &d_, 16); emit_xmm("lddqu", i, o);
    }

    /* ---- alternating / horizontal add-subtract (finite inputs) ---- */
    RUN_PS("addsubps", "addsubps"); RUN_PD("addsubpd", "addsubpd");
    RUN_PS("haddps", "haddps");     RUN_PD("haddpd", "haddpd");
    RUN_PS("hsubps", "hsubps");     RUN_PD("hsubpd", "hsubpd");

    /* ---- fisttp: x87 store with truncation, then pop (SSE3) ---- */
    /* In-range finite inputs; truncation toward zero is exact and matches x86. */
    static const double FT[] = {
        0.0, 2.7, -2.7, 2.3, -2.3, 100.9, -100.9, 12345.99, -12345.01,
    };
    for (size_t i = 0; i < sizeof FT / sizeof FT[0]; i++) {
        double dv = FT[i];
        int16_t r16 = 0; int32_t r32 = 0; int64_t r64 = 0;
        __asm__ volatile("fldl %[in]\n\tfisttps %[out]" : [out] "=m"(r16) : [in] "m"(dv) : "st");
        dc_emit("fisttp16", 16, i, 0, (uint16_t)r16, 0, 0);
        __asm__ volatile("fldl %[in]\n\tfisttpl %[out]" : [out] "=m"(r32) : [in] "m"(dv) : "st");
        dc_emit("fisttp32", 32, i, 0, (uint32_t)r32, 0, 0);
        __asm__ volatile("fldl %[in]\n\tfisttpll %[out]" : [out] "=m"(r64) : [in] "m"(dv) : "st");
        dc_emit("fisttp64lo", 32, i, 0, (uint32_t)r64, 0, 0);
        dc_emit("fisttp64hi", 32, i, 0, (uint32_t)((uint64_t)r64 >> 32), 0, 0);
    }
}
