/*
 * bit_ops — differential test for the bit instructions: bt/bts/btr/btc (test,
 * and test-and-set/reset/complement) and bsf/bsr (bit scan fwd/rev), widths
 * 16/32(/64). Subtle defined-flag rules:
 *   bt-family: CF = the selected bit; OF/SF/ZF/AF/PF undefined. The result (for
 *     bts/btr/btc) is defined. The bit index is masked to the operand width.
 *   bsf/bsr: ZF = (src == 0); CF/OF/SF/AF/PF undefined. The destination is
 *     defined only when src != 0 (undefined otherwise) -- so we force it to 0
 *     for the src==0 case to keep the line deterministic across cells.
 */
#include "diff_common.h"

static const uint64_t PAT[] = {
    0x0, 0x1, 0x80, 0x8000, 0x80000000, 0xdeadbeef, 0xffffffff, 0xa5a5a5a5, 0x10000,
};
#define NPAT (sizeof PAT / sizeof PAT[0])
static const int BITS[] = {0, 1, 7, 15, 16, 31, 33, 63};
#define NBITS (sizeof BITS / sizeof BITS[0])

/* bt/bts/btr/btc reg,reg: CF = selected bit; result (v) defined. */
#define BT(MNEM, CT, VIN, BITV, RESOUT, FLOUT) do {                        \
    CT v_ = (CT)(VIN); unsigned long f_; CT b_ = (CT)(BITV);                \
    __asm__ volatile(MNEM " %[b], %[v]\n\t" PUSHF "\n\tpop %[f]"            \
        : [v] "+r"(v_), [f] "=r"(f_) : [b] "r"(b_) : "cc");                 \
    (RESOUT) = (uint64_t)v_; (FLOUT) = f_;                                  \
} while (0)

/* bsf/bsr reg,reg: ZF = (src==0); dest defined only when src != 0. */
#define BS(MNEM, CT, SIN, RESOUT, FLOUT) do {                              \
    CT s_ = (CT)(SIN); CT d_ = 0; unsigned long f_;                         \
    __asm__ volatile(MNEM " %[s], %[d]\n\t" PUSHF "\n\tpop %[f]"            \
        : [d] "+r"(d_), [f] "=r"(f_) : [s] "r"(s_) : "cc");                 \
    (RESOUT) = (uint64_t)d_; (FLOUT) = f_;                                  \
} while (0)

#define RUN_WIDTH(W, CT) do {                                              \
    for (size_t i = 0; i < NPAT; i++) {                                    \
        uint64_t v = PAT[i], res; unsigned long fl;                        \
        for (size_t j = 0; j < NBITS; j++) {                               \
            uint64_t b = (uint64_t)BITS[j];                                \
            BT("bt",  CT, v, b, res, fl); dc_emit("bt",  W, v, b, res, fl, FL_CF); \
            BT("bts", CT, v, b, res, fl); dc_emit("bts", W, v, b, res, fl, FL_CF); \
            BT("btr", CT, v, b, res, fl); dc_emit("btr", W, v, b, res, fl, FL_CF); \
            BT("btc", CT, v, b, res, fl); dc_emit("btc", W, v, b, res, fl, FL_CF); \
        }                                                                  \
        int zero = ((CT)v) == 0;                                           \
        BS("bsf", CT, v, res, fl); dc_emit("bsf", W, v, 0, zero ? 0 : res, fl, FL_ZF); \
        BS("bsr", CT, v, res, fl); dc_emit("bsr", W, v, 0, zero ? 0 : res, fl, FL_ZF); \
    }                                                                      \
} while (0)

static void run_cases(void) {
    RUN_WIDTH(16, uint16_t);
    RUN_WIDTH(32, uint32_t);
#if HAVE_W64
    RUN_WIDTH(64, uint64_t);
#endif
}
