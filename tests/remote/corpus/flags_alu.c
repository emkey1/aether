/*
 * flags_alu — differential test for integer ALU result + EFLAGS exactness
 * across operand widths 8/16/32(/64). Compares emulator (i386/amd64, every
 * engine) against the native x86 oracle. Guards only architecturally-DEFINED
 * flag bits per op so we never false-positive on undefined bits.
 *
 * Exercises the exact bug classes this project has hit: eager-flag computation,
 * hi-register ALU, adc/sbb carry chaining, inc/dec CF-preservation (CF excluded
 * from the compare here; a dedicated CF-preservation test covers it separately).
 */
#include "diff_common.h"

/* Edge operands; masked to width at use. Hit signed/unsigned boundaries. */
static const uint64_t PAT[] = {
    0x0, 0x1, 0x2, 0x7f, 0x80, 0xff,
    0x7fffffff, 0x80000000, 0xffffffff,
};
#define NPAT (sizeof PAT / sizeof PAT[0])
#define RAND_PER 24

#define LOGIC_DEF (FL_ALL & ~FL_AF)   /* AF undefined after and/or/xor/test */
#define INCDEC_DEF (FL_ALL & ~FL_CF)  /* inc/dec leave CF unaffected */

#define RUN_WIDTH(W, CT) do {                                                  \
    dc_rng_state = dc.seed ^ ((uint64_t)(W) << 32);                            \
    for (size_t i = 0; i < NPAT; i++) for (size_t j = 0; j < NPAT; j++) {      \
        uint64_t a = PAT[i], b = PAT[j], res; unsigned long fl;                \
        DC_ALU2("add", CT, a, b, res, fl); dc_emit("add", W, a, b, res, fl, FL_ALL);    \
        DC_ALU2("sub", CT, a, b, res, fl); dc_emit("sub", W, a, b, res, fl, FL_ALL);    \
        DC_ALU2("and", CT, a, b, res, fl); dc_emit("and", W, a, b, res, fl, LOGIC_DEF); \
        DC_ALU2("or",  CT, a, b, res, fl); dc_emit("or",  W, a, b, res, fl, LOGIC_DEF); \
        DC_ALU2("xor", CT, a, b, res, fl); dc_emit("xor", W, a, b, res, fl, LOGIC_DEF); \
        DC_ALU2_CMP("cmp",  CT, a, b, fl); dc_emit("cmp",  W, a, b, a, fl, FL_ALL);     \
        DC_ALU2_CMP("test", CT, a, b, fl); dc_emit("test", W, a, b, a, fl, LOGIC_DEF);  \
        DC_ALU2_CF("adc", CT, a, b, 0, res, fl); dc_emit("adc0", W, a, b, res, fl, FL_ALL); \
        DC_ALU2_CF("adc", CT, a, b, 1, res, fl); dc_emit("adc1", W, a, b, res, fl, FL_ALL); \
        DC_ALU2_CF("sbb", CT, a, b, 0, res, fl); dc_emit("sbb0", W, a, b, res, fl, FL_ALL); \
        DC_ALU2_CF("sbb", CT, a, b, 1, res, fl); dc_emit("sbb1", W, a, b, res, fl, FL_ALL); \
    }                                                                          \
    for (size_t i = 0; i < NPAT; i++) {                                        \
        uint64_t a = PAT[i], res; unsigned long fl;                            \
        DC_ALU1("inc", CT, a, res, fl); dc_emit("inc", W, a, 0, res, fl, INCDEC_DEF); \
        DC_ALU1("dec", CT, a, res, fl); dc_emit("dec", W, a, 0, res, fl, INCDEC_DEF); \
        DC_ALU1("neg", CT, a, res, fl); dc_emit("neg", W, a, 0, res, fl, FL_ALL);      \
        DC_ALU1("not", CT, a, res, fl); dc_emit("not", W, a, 0, res, fl, 0);           \
    }                                                                          \
    for (int k = 0; k < RAND_PER; k++) {                                       \
        uint64_t a = dc_rng(), b = dc_rng(), res; unsigned long fl;            \
        DC_ALU2("add", CT, a, b, res, fl); dc_emit("add", W, a, b, res, fl, FL_ALL);    \
        DC_ALU2("sub", CT, a, b, res, fl); dc_emit("sub", W, a, b, res, fl, FL_ALL);    \
        DC_ALU2("xor", CT, a, b, res, fl); dc_emit("xor", W, a, b, res, fl, LOGIC_DEF); \
        DC_ALU2_CMP("cmp", CT, a, b, fl);  dc_emit("cmp", W, a, b, a, fl, FL_ALL);      \
        DC_ALU2_CF("adc", CT, a, b, 1, res, fl); dc_emit("adc1", W, a, b, res, fl, FL_ALL); \
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
