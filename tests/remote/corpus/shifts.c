/*
 * shifts — differential test for shl/shr/sar/rol/ror result + flags across
 * widths 8/16/32(/64) and the three count encodings: by CL (0xD3), by 1
 * (0xD0/0xD1), and by imm8 (0xC1). Heavy JIT territory (per the project's
 * shift/rotate gadget work) with subtle flag rules:
 *
 *   shifts: CF|SF|ZF|PF defined; OF defined only for 1-bit; AF undefined.
 *   rotates: CF defined; OF defined only for 1-bit; SF/ZF/PF/AF UNAFFECTED.
 *   count masked to 5 bits (<=32-bit) or 6 bits (64-bit); eff==0 => no flags.
 *
 * We compare only architecturally-defined bits, and for rotates we never
 * compare SF/ZF/PF/AF (they retain their prior value, which our harness does
 * not control). The result is always compared.
 */
#include "diff_common.h"

static const uint64_t PAT[] = {
    0x1, 0x7f, 0x80, 0xff, 0x1234, 0x80000001, 0xdeadbeef, 0xffffffff,
};
#define NPAT (sizeof PAT / sizeof PAT[0])
static const int CNT[] = {0, 1, 2, 3, 4, 7, 8, 15, 16, 31, 63};
#define NCNT (sizeof CNT / sizeof CNT[0])

#define SHDEF (FL_CF | FL_SF | FL_ZF | FL_PF)         /* shift, count != 0 */
#define SHDEF1 (FL_CF | FL_SF | FL_ZF | FL_PF | FL_OF) /* shift by 1 */

/* shift/rotate by CL (0xD3) */
#define SH_CL(MNEM, CT, AIN, CNTV, RESOUT, FLOUT) do {                         \
    CT r_ = (CT)(AIN); unsigned long f_;                                       \
    __asm__ volatile(MNEM " %%cl, %[r]\n\t" PUSHF "\n\tpop %[f]"               \
        : [r] "+r"(r_), [f] "=r"(f_) : "c"((unsigned char)(CNTV)) : "cc");      \
    (RESOUT) = (uint64_t)r_; (FLOUT) = f_;                                      \
} while (0)

/* shift/rotate by 1 (0xD0/0xD1) — OF defined */
#define SH_1(MNEM, CT, AIN, RESOUT, FLOUT) do {                                \
    CT r_ = (CT)(AIN); unsigned long f_;                                       \
    __asm__ volatile(MNEM " %[r]\n\t" PUSHF "\n\tpop %[f]"                      \
        : [r] "+r"(r_), [f] "=r"(f_) : : "cc");                                 \
    (RESOUT) = (uint64_t)r_; (FLOUT) = f_;                                      \
} while (0)

/* shift/rotate by imm8 (0xC1); IMM must be a literal > 1 to force 0xC1 */
#define SH_IMM(MNEM, CT, AIN, IMM, RESOUT, FLOUT) do {                         \
    CT r_ = (CT)(AIN); unsigned long f_;                                       \
    __asm__ volatile(MNEM " $" #IMM ", %[r]\n\t" PUSHF "\n\tpop %[f]"           \
        : [r] "+r"(r_), [f] "=r"(f_) : : "cc");                                 \
    (RESOUT) = (uint64_t)r_; (FLOUT) = f_;                                      \
} while (0)

#define CL_SWEEP(LABEL, MNEM, CT, W, A, ROT) do {                              \
    for (size_t c = 0; c < NCNT; c++) {                                        \
        unsigned eff = CNT[c] & ((W) == 64 ? 0x3f : 0x1f);                     \
        uint64_t res; unsigned long fl;                                        \
        SH_CL(MNEM, CT, A, CNT[c], res, fl);                                    \
        unsigned long def = eff ? ((ROT) ? FL_CF : SHDEF) : 0;                 \
        dc_emit(LABEL, W, A, (uint64_t)CNT[c], res, fl, def);                  \
    }                                                                          \
} while (0)

#define RUN_WIDTH(W, CT) do {                                                  \
    for (size_t i = 0; i < NPAT; i++) {                                        \
        uint64_t a = PAT[i], res; unsigned long fl;                            \
        CL_SWEEP("shl", "shl", CT, W, a, 0);                                   \
        CL_SWEEP("shr", "shr", CT, W, a, 0);                                   \
        CL_SWEEP("sar", "sar", CT, W, a, 0);                                   \
        CL_SWEEP("rol", "rol", CT, W, a, 1);                                   \
        CL_SWEEP("ror", "ror", CT, W, a, 1);                                   \
        SH_1("shl", CT, a, res, fl); dc_emit("shl1", W, a, 1, res, fl, SHDEF1); \
        SH_1("shr", CT, a, res, fl); dc_emit("shr1", W, a, 1, res, fl, SHDEF1); \
        SH_1("sar", CT, a, res, fl); dc_emit("sar1", W, a, 1, res, fl, SHDEF1); \
        SH_1("rol", CT, a, res, fl); dc_emit("rol1", W, a, 1, res, fl, FL_CF | FL_OF); \
        SH_1("ror", CT, a, res, fl); dc_emit("ror1", W, a, 1, res, fl, FL_CF | FL_OF); \
        SH_IMM("shl", CT, a, 3, res, fl); dc_emit("shlI3", W, a, 3, res, fl, SHDEF); \
        SH_IMM("shr", CT, a, 7, res, fl); dc_emit("shrI7", W, a, 7, res, fl, SHDEF); \
        SH_IMM("sar", CT, a, 7, res, fl); dc_emit("sarI7", W, a, 7, res, fl, SHDEF); \
        SH_IMM("rol", CT, a, 5, res, fl); dc_emit("rolI5", W, a, 5, res, fl, FL_CF); \
        SH_IMM("ror", CT, a, 5, res, fl); dc_emit("rorI5", W, a, 5, res, fl, FL_CF); \
    }                                                                          \
} while (0)

static void run_cases(void) {
    RUN_WIDTH(8, uint8_t);
    RUN_WIDTH(16, uint16_t);
    RUN_WIDTH(32, uint32_t);
#if HAVE_W64
    RUN_WIDTH(64, uint64_t);
#endif
}
