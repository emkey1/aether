/*
 * adc_sbb_mem — differential test for adc/sbb with a MEMORY operand + carry-in.
 *
 * The register-operand adc/sbb (carry-in) bridge to the interp helper, but the
 * 32/64-bit memory-operand forms (0x11/0x13/0x19/0x1b) take the native amd64
 * gadget path (jit/gen.c), which "feeds rhs+cf to the addsub flag macro" — the
 * same carry-fold anti-pattern fixed in the interp helper (commit 2827afd1).
 * Forcing a memory r/m here exercises that gadget. Byte/16-bit forms bridge, so
 * the width stratification localizes any remaining bug to the native gadget.
 *
 *   Mxxx = memory destination (RMW):  [m] = [m] <op> reg (+cf)   0x11/0x19
 *   Lxxx = memory source (load-op):   reg = reg <op> [m]  (+cf)   0x13/0x1b
 */
#include "diff_common.h"

static const uint64_t PAT[] = {
    0x0, 0x1, 0x7f, 0x80, 0xff, 0x7fffffff, 0x80000000, 0xffffffff,
};
#define NPAT (sizeof PAT / sizeof PAT[0])
#define RAND_PER 24

/* reg -> [mem] (RMW): [m] = [m] <op> b (+cf). result read back from memory. */
#define RMW(MNEM, CT, MIN, BIN, CFIN, RESOUT, FLOUT) do {                      \
    CT m_ = (CT)(MIN); unsigned long f_; unsigned char cf_ = (CFIN);           \
    __asm__ volatile("add $0xff, %b[cf]\n\t" MNEM " %[b], %[m]\n\t"            \
                     PUSHF "\n\tpop %[f]"                                       \
        : [m] "+m"(m_), [f] "=r"(f_), [cf] "+q"(cf_)                            \
        : [b] "r"((CT)(BIN)) : "cc", "memory");                                \
    (RESOUT) = (uint64_t)m_; (FLOUT) = f_;                                      \
} while (0)

/* [mem] -> reg (load-op): r = r <op> [m] (+cf). result read back from reg. */
#define LOADOP(MNEM, CT, RIN, MIN, CFIN, RESOUT, FLOUT) do {                   \
    CT r_ = (CT)(RIN); CT m_ = (CT)(MIN); unsigned long f_;                     \
    unsigned char cf_ = (CFIN);                                                \
    __asm__ volatile("add $0xff, %b[cf]\n\t" MNEM " %[m], %[r]\n\t"            \
                     PUSHF "\n\tpop %[f]"                                       \
        : [r] "+r"(r_), [f] "=r"(f_), [cf] "+q"(cf_)                            \
        : [m] "m"(m_) : "cc", "memory");                                       \
    (RESOUT) = (uint64_t)r_; (FLOUT) = f_;                                      \
} while (0)

#define RUN_WIDTH(W, CT) do {                                                  \
    dc_rng_state = dc.seed ^ ((uint64_t)(W) << 32);                            \
    for (size_t i = 0; i < NPAT; i++) for (size_t j = 0; j < NPAT; j++) {      \
        uint64_t a = PAT[i], b = PAT[j], res; unsigned long fl;                \
        RMW("adc", CT, a, b, 0, res, fl); dc_emit("Madc0", W, a, b, res, fl, FL_ALL); \
        RMW("adc", CT, a, b, 1, res, fl); dc_emit("Madc1", W, a, b, res, fl, FL_ALL); \
        RMW("sbb", CT, a, b, 0, res, fl); dc_emit("Msbb0", W, a, b, res, fl, FL_ALL); \
        RMW("sbb", CT, a, b, 1, res, fl); dc_emit("Msbb1", W, a, b, res, fl, FL_ALL); \
        LOADOP("adc", CT, a, b, 0, res, fl); dc_emit("Ladc0", W, a, b, res, fl, FL_ALL); \
        LOADOP("adc", CT, a, b, 1, res, fl); dc_emit("Ladc1", W, a, b, res, fl, FL_ALL); \
        LOADOP("sbb", CT, a, b, 0, res, fl); dc_emit("Lsbb0", W, a, b, res, fl, FL_ALL); \
        LOADOP("sbb", CT, a, b, 1, res, fl); dc_emit("Lsbb1", W, a, b, res, fl, FL_ALL); \
    }                                                                          \
    for (int k = 0; k < RAND_PER; k++) {                                       \
        uint64_t a = dc_rng(), b = dc_rng(), res; unsigned long fl;            \
        RMW("adc", CT, a, b, 1, res, fl);    dc_emit("Madc1", W, a, b, res, fl, FL_ALL); \
        RMW("sbb", CT, a, b, 1, res, fl);    dc_emit("Msbb1", W, a, b, res, fl, FL_ALL); \
        LOADOP("adc", CT, a, b, 1, res, fl); dc_emit("Ladc1", W, a, b, res, fl, FL_ALL); \
        LOADOP("sbb", CT, a, b, 1, res, fl); dc_emit("Lsbb1", W, a, b, res, fl, FL_ALL); \
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
