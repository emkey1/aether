/*
 * sse4 — differential test for the SSSE3 / SSE4.1 three-byte (0F 38 / 0F 3A)
 * vector ops that the i386 JIT previously could not decode at all (a guest
 * `pinsrd` SIGILL'd cmake). Each op runs via inline asm on controlled operands
 * and emits its 128-bit result as four dwords (or the GP/flag result directly);
 * the conductor requires byte-identical lines against mint's real Intel silicon
 * (the i386 ground truth the M5/Rosetta cannot provide).
 *
 * Integer ops are pure bit moves -> exact match required, no canonicalization.
 * round{ps,pd,ss,sd} use only finite inputs so the rounded result is exact and
 * architecture-independent (no NaN payloads to canonicalize).
 */
#include "diff_common.h"

typedef int       v4si __attribute__((vector_size(16)));
typedef long long v2di __attribute__((vector_size(16)));
typedef float     v4sf __attribute__((vector_size(16)));
typedef double    v2df __attribute__((vector_size(16)));

/* distinctive 128-bit input patterns (sign bits, zeros, max values, sequences) */
static const uint32_t XPAT[][4] = {
    {0x00010203, 0x04050607, 0x08090a0b, 0x0c0d0e0f},
    {0xdeadbeef, 0xcafebabe, 0x12345678, 0x9abcdef0},
    {0x80000000, 0x7fffffff, 0x00000000, 0xffffffff},
    {0x00000001, 0xfffffffe, 0x8000ffff, 0x7fff0001},
    {0x574e4170, 0x574e4170, 0x574e4170, 0x574e4170},
};
#define NX (sizeof XPAT / sizeof XPAT[0])

static void emit_xmm(const char *op, uint64_t key, const void *xmm) {
    const uint32_t *o = (const uint32_t *)xmm;
    for (int k = 0; k < 4; k++)
        dc_emit(op, 32, key, k, o[k], 0, 0);
}

/* binary op, no imm: dst in/out (=reg), src in (=r/m, register form) */
#define RUN_BIN(MNEM, OP) do {                                                  \
    for (size_t i = 0; i < NX; i++) for (size_t j = 0; j < NX; j++) {           \
        v4si d_, s_; uint32_t o[4];                                            \
        memcpy(&d_, XPAT[i], 16); memcpy(&s_, XPAT[j], 16);                    \
        __asm__ volatile(MNEM " %[s], %[d]" : [d] "+x"(d_) : [s] "x"(s_));      \
        memcpy(o, &d_, 16); emit_xmm(OP, (i << 4) | j, o);                      \
    } } while (0)

/* unary op (src -> dst, dst write-only): pabs*, pmovsx/zx */
#define RUN_UN(MNEM, OP) do {                                                   \
    for (size_t i = 0; i < NX; i++) {                                           \
        v4si s_, d_; uint32_t o[4]; memcpy(&s_, XPAT[i], 16);                  \
        __asm__ volatile(MNEM " %[s], %[d]" : [d] "=x"(d_) : [s] "x"(s_));      \
        memcpy(o, &d_, 16); emit_xmm(OP, i, o);                                 \
    } } while (0)

/* binary op with imm8 (palignr, blend*, round packed) */
#define RUN_BIN_IMM(MNEM, OP, IMM) do {                                        \
    for (size_t i = 0; i < NX; i++) for (size_t j = 0; j < NX; j++) {           \
        v4si d_, s_; uint32_t o[4];                                            \
        memcpy(&d_, XPAT[i], 16); memcpy(&s_, XPAT[j], 16);                    \
        __asm__ volatile(MNEM " $%c[m], %[s], %[d]"                            \
            : [d] "+x"(d_) : [s] "x"(s_), [m] "i"(IMM));                       \
        memcpy(o, &d_, 16);                                                    \
        emit_xmm(OP, ((uint64_t)(IMM) << 8) | (i << 4) | j, o);                \
    } } while (0)

__attribute__((target("sse4.1")))
static void run_cases(void) {
    /* ---- insert / extract (dword, byte, word) ---- */
    for (size_t i = 0; i < NX; i++) {
        v4si d_; uint32_t o[4];
        /* pinsrd xmm, r32, imm (the crashing instruction) */
        memcpy(&d_, XPAT[i], 16);
        uint32_t gv = 0x574e4170u;
        __asm__ volatile("pinsrd $1, %[s], %[d]" : [d] "+x"(d_) : [s] "r"(gv));
        memcpy(o, &d_, 16); emit_xmm("pinsrd", i, o);
        /* pinsrd from memory */
        memcpy(&d_, XPAT[i], 16);
        __asm__ volatile("pinsrd $2, %[s], %[d]" : [d] "+x"(d_) : [s] "m"(gv));
        memcpy(o, &d_, 16); emit_xmm("pinsrdm", i, o);
        /* pinsrb xmm, r32, imm */
        memcpy(&d_, XPAT[i], 16);
        __asm__ volatile("pinsrb $5, %[s], %[d]" : [d] "+x"(d_) : [s] "r"(gv));
        memcpy(o, &d_, 16); emit_xmm("pinsrb", i, o);

        v4si s_; memcpy(&s_, XPAT[i], 16);
        /* pextrd r32, xmm, imm (register dest) */
        uint32_t er;
        __asm__ volatile("pextrd $2, %[s], %[d]" : [d] "=r"(er) : [s] "x"(s_));
        dc_emit("pextrd", 32, i, 0, er, 0, 0);
        /* pextrd m32, xmm, imm (memory dest) */
        uint32_t em = 0;
        __asm__ volatile("pextrd $3, %[s], %[d]" : [d] "=m"(em) : [s] "x"(s_));
        dc_emit("pextrdm", 32, i, 0, em, 0, 0);
        /* extractps r32, xmm, imm (== pextrd lane move) */
        __asm__ volatile("extractps $1, %[s], %[d]" : [d] "=r"(er) : [s] "x"(s_));
        dc_emit("extractps", 32, i, 0, er, 0, 0);
        /* pextrb r32, xmm, imm (zero-extended) */
        __asm__ volatile("pextrb $7, %[s], %[d]" : [d] "=r"(er) : [s] "x"(s_));
        dc_emit("pextrb", 32, i, 0, er, 0, 0);
        /* pextrb m8, xmm, imm */
        uint8_t eb = 0;
        __asm__ volatile("pextrb $11, %[s], %[d]" : [d] "=m"(eb) : [s] "x"(s_));
        dc_emit("pextrbm", 32, i, 0, eb, 0, 0);
        /* pextrw r32, xmm, imm (0F 3A 15 form, zero-extended) */
        __asm__ volatile("pextrw $3, %[s], %[d]" : [d] "=r"(er) : [s] "x"(s_));
        dc_emit("pextrw", 32, i, 0, er, 0, 0);
        /* pextrw m16, xmm, imm */
        uint16_t ew = 0;
        __asm__ volatile("pextrw $5, %[s], %[d]" : [d] "=m"(ew) : [s] "x"(s_));
        dc_emit("pextrwm", 32, i, 0, ew, 0, 0);
    }

    /* ---- SSSE3 shuffle / align / abs ---- */
    RUN_BIN("pshufb", "pshufb");
    RUN_BIN_IMM("palignr", "palignr", 0); RUN_BIN_IMM("palignr", "palignr", 5);
    RUN_BIN_IMM("palignr", "palignr", 11); RUN_BIN_IMM("palignr", "palignr", 16);
    RUN_BIN_IMM("palignr", "palignr", 20);
    RUN_UN("pabsb", "pabsb"); RUN_UN("pabsw", "pabsw"); RUN_UN("pabsd", "pabsd");

    /* ---- SSE4.1 widening moves ---- */
    RUN_UN("pmovsxbw", "pmovsxbw"); RUN_UN("pmovzxbw", "pmovzxbw");
    RUN_UN("pmovsxbd", "pmovsxbd"); RUN_UN("pmovzxbd", "pmovzxbd");
    RUN_UN("pmovsxbq", "pmovsxbq"); RUN_UN("pmovzxbq", "pmovzxbq");
    RUN_UN("pmovsxwd", "pmovsxwd"); RUN_UN("pmovzxwd", "pmovzxwd");
    RUN_UN("pmovsxwq", "pmovsxwq"); RUN_UN("pmovzxwq", "pmovzxwq");
    RUN_UN("pmovsxdq", "pmovsxdq"); RUN_UN("pmovzxdq", "pmovzxdq");

    /* ---- SSE4.1 integer multiply / compare / pack ---- */
    RUN_BIN("pmulld", "pmulld"); RUN_BIN("pmuldq", "pmuldq");
    RUN_BIN("pcmpeqq", "pcmpeqq"); RUN_BIN("pcmpgtq", "pcmpgtq");
    RUN_BIN("packusdw", "packusdw");

    /* ---- SSE4.1 min / max (the variants SSE2 lacks) ---- */
    RUN_BIN("pminsb", "pminsb"); RUN_BIN("pmaxsb", "pmaxsb");
    RUN_BIN("pminuw", "pminuw"); RUN_BIN("pmaxuw", "pmaxuw");
    RUN_BIN("pminsd", "pminsd"); RUN_BIN("pmaxsd", "pmaxsd");
    RUN_BIN("pminud", "pminud"); RUN_BIN("pmaxud", "pmaxud");

    /* ---- SSE4.1 blends (imm-controlled) ---- */
    RUN_BIN_IMM("pblendw", "pblendw", 0x00); RUN_BIN_IMM("pblendw", "pblendw", 0xa5);
    RUN_BIN_IMM("pblendw", "pblendw", 0xff);
    RUN_BIN_IMM("blendps", "blendps", 0x0); RUN_BIN_IMM("blendps", "blendps", 0x5);
    RUN_BIN_IMM("blendps", "blendps", 0xf);
    RUN_BIN_IMM("blendpd", "blendpd", 0x0); RUN_BIN_IMM("blendpd", "blendpd", 0x1);
    RUN_BIN_IMM("blendpd", "blendpd", 0x2); RUN_BIN_IMM("blendpd", "blendpd", 0x3);

    /* ---- SSE4.1 variable blends (implicit XMM0 mask) ---- */
    for (size_t i = 0; i < NX; i++) for (size_t j = 0; j < NX; j++) {
        for (size_t m = 0; m < NX; m++) {
            v4si d_, s_, mask_; uint32_t o[4];
            memcpy(&mask_, XPAT[m], 16);
            __asm__ volatile("movdqa %0, %%xmm0" : : "x"(mask_) : "xmm0");
            memcpy(&d_, XPAT[i], 16); memcpy(&s_, XPAT[j], 16);
            __asm__ volatile("pblendvb %%xmm0, %[s], %[d]" : [d] "+x"(d_) : [s] "x"(s_) : );
            memcpy(o, &d_, 16); emit_xmm("pblendvb", (i << 8) | (j << 4) | m, o);
            memcpy(&d_, XPAT[i], 16);
            __asm__ volatile("blendvps %%xmm0, %[s], %[d]" : [d] "+x"(d_) : [s] "x"(s_) : );
            memcpy(o, &d_, 16); emit_xmm("blendvps", (i << 8) | (j << 4) | m, o);
            memcpy(&d_, XPAT[i], 16);
            __asm__ volatile("blendvpd %%xmm0, %[s], %[d]" : [d] "+x"(d_) : [s] "x"(s_) : );
            memcpy(o, &d_, 16); emit_xmm("blendvpd", (i << 8) | (j << 4) | m, o);
        }
    }

    /* ---- SSE4.1 ptest (ZF/CF only) ---- */
    for (size_t i = 0; i < NX; i++) for (size_t j = 0; j < NX; j++) {
        v4si d_, s_; unsigned long f;
        memcpy(&d_, XPAT[i], 16); memcpy(&s_, XPAT[j], 16);
        __asm__ volatile("ptest %[s], %[d]\n\t" PUSHF "\n\tpop %[f]"
            : [f] "=r"(f) : [d] "x"(d_), [s] "x"(s_) : "cc");
        dc_emit("ptest", 32, (i << 4) | j, 0, 0, f, FL_ZF | FL_CF);
    }

    /* ---- SSE4.1 round (finite inputs -> exact, mode in imm[1:0]) ---- */
    static const float  FV[4] = { 2.5f, -2.5f, 0.49f, -1.75f };
    static const double DV[2] = { 2.5, -3.5 };
    for (int mode = 0; mode < 4; mode++) {
        v4sf fs; for (int k = 0; k < 4; k++) ((float *)&fs)[k] = FV[k];
        v4sf fd; v2df ds; for (int k = 0; k < 2; k++) ((double *)&ds)[k] = DV[k];
        v2df dd; uint32_t o[4];
        switch (mode) {
            case 0:
                __asm__ volatile("roundps $0,%[s],%[d]" : [d] "=x"(fd) : [s] "x"(fs));
                memcpy(o, &fd, 16); emit_xmm("roundps", mode, o);
                __asm__ volatile("roundpd $0,%[s],%[d]" : [d] "=x"(dd) : [s] "x"(ds));
                memcpy(o, &dd, 16); emit_xmm("roundpd", mode, o);
                break;
            case 1:
                __asm__ volatile("roundps $1,%[s],%[d]" : [d] "=x"(fd) : [s] "x"(fs));
                memcpy(o, &fd, 16); emit_xmm("roundps", mode, o);
                __asm__ volatile("roundpd $1,%[s],%[d]" : [d] "=x"(dd) : [s] "x"(ds));
                memcpy(o, &dd, 16); emit_xmm("roundpd", mode, o);
                break;
            case 2:
                __asm__ volatile("roundps $2,%[s],%[d]" : [d] "=x"(fd) : [s] "x"(fs));
                memcpy(o, &fd, 16); emit_xmm("roundps", mode, o);
                __asm__ volatile("roundpd $2,%[s],%[d]" : [d] "=x"(dd) : [s] "x"(ds));
                memcpy(o, &dd, 16); emit_xmm("roundpd", mode, o);
                break;
            default:
                __asm__ volatile("roundps $3,%[s],%[d]" : [d] "=x"(fd) : [s] "x"(fs));
                memcpy(o, &fd, 16); emit_xmm("roundps", mode, o);
                __asm__ volatile("roundpd $3,%[s],%[d]" : [d] "=x"(dd) : [s] "x"(ds));
                memcpy(o, &dd, 16); emit_xmm("roundpd", mode, o);
                break;
        }
        /* scalar forms (low lane rounded, upper lanes from dst) */
        v4sf sfd; memcpy(&sfd, XPAT[0], 16);
        v2df sdd; memcpy(&sdd, XPAT[1], 16);
        uint32_t so[4];
        switch (mode) {
            case 0:
                __asm__ volatile("roundss $0,%[s],%[d]" : [d] "+x"(sfd) : [s] "x"(fs));
                __asm__ volatile("roundsd $0,%[s],%[d]" : [d] "+x"(sdd) : [s] "x"(ds));
                break;
            case 1:
                __asm__ volatile("roundss $1,%[s],%[d]" : [d] "+x"(sfd) : [s] "x"(fs));
                __asm__ volatile("roundsd $1,%[s],%[d]" : [d] "+x"(sdd) : [s] "x"(ds));
                break;
            case 2:
                __asm__ volatile("roundss $2,%[s],%[d]" : [d] "+x"(sfd) : [s] "x"(fs));
                __asm__ volatile("roundsd $2,%[s],%[d]" : [d] "+x"(sdd) : [s] "x"(ds));
                break;
            default:
                __asm__ volatile("roundss $3,%[s],%[d]" : [d] "+x"(sfd) : [s] "x"(fs));
                __asm__ volatile("roundsd $3,%[s],%[d]" : [d] "+x"(sdd) : [s] "x"(ds));
                break;
        }
        memcpy(so, &sfd, 16); emit_xmm("roundss", mode, so);
        memcpy(so, &sdd, 16); emit_xmm("roundsd", mode, so);
    }
    (void)sizeof(v2di);
}
