/*
 * popcnt_lzcnt_tzcnt — differential test for POPCNT/TZCNT/LZCNT, widths
 * 16/32(/64). Unlike bsf/bsr, all three are fully defined for every input,
 * including src==0 (no undefined-destination case to mask out):
 *   popcnt: result = number of set bits; CF/OF/SF/AF/PF cleared, ZF=(res==0).
 *   tzcnt/lzcnt: result = trailing/leading zero count, defined as the operand
 *     width when src==0; CF=(src==0), ZF=(res==0); OF/SF/AF/PF undefined.
 */
#include "diff_common.h"

static const uint64_t PAT[] = {
    0x0, 0x1, 0x2, 0x80, 0x8000, 0x80000000, 0xdeadbeef, 0xffffffff,
    0xa5a5a5a5, 0x10000, 0x1000000000000ULL, 0x8000000000000000ULL,
};
#define NPAT (sizeof PAT / sizeof PAT[0])

/* popcnt/tzcnt/lzcnt reg,reg: result always defined, no undefined-dest case. */
#define CNT(MNEM, CT, SIN, RESOUT, FLOUT) do {                             \
    CT s_ = (CT)(SIN); CT d_ = 0; unsigned long f_;                        \
    __asm__ volatile(MNEM " %[s], %[d]\n\t" PUSHF "\n\tpop %[f]"           \
        : [d] "=r"(d_), [f] "=r"(f_) : [s] "r"(s_) : "cc");                \
    (RESOUT) = (uint64_t)d_; (FLOUT) = f_;                                 \
} while (0)

#define RUN_WIDTH(W, CT) do {                                              \
    for (size_t i = 0; i < NPAT; i++) {                                    \
        uint64_t v = PAT[i], res; unsigned long fl;                        \
        CNT("popcnt", CT, v, res, fl); dc_emit("popcnt", W, v, 0, res, fl, FL_ALL); \
        CNT("tzcnt",  CT, v, res, fl); dc_emit("tzcnt",  W, v, 0, res, fl, FL_CF | FL_ZF); \
        CNT("lzcnt",  CT, v, res, fl); dc_emit("lzcnt",  W, v, 0, res, fl, FL_CF | FL_ZF); \
    }                                                                       \
} while (0)

static void run_cases(void) {
    RUN_WIDTH(16, uint16_t);
    RUN_WIDTH(32, uint32_t);
#if HAVE_W64
    RUN_WIDTH(64, uint64_t);
#endif
}
