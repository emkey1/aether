/*
 * sext — differential test for sign/zero-extension result correctness:
 * movsx/movzx (byte/word -> word/long/quad), movsxd (long->quad, 0x63), and the
 * accumulator forms cbw/cwde/cdqe (0x98 + operand size) and cwd/cdq/cqo (0x99).
 * None of these affect flags, so the defined flag mask is 0 and we compare the
 * extended result only.
 */
#include "diff_common.h"

static const uint64_t PAT[] = {
    0x0, 0x1, 0x7f, 0x80, 0xff, 0x7fff, 0x8000, 0xffff,
    0x7fffffff, 0x80000000, 0xffffffff,
};
#define NPAT (sizeof PAT / sizeof PAT[0])

/* two-register movsx/movzx: dst <- ext(src). SRCT sizes the source operand. */
#define EXT(LABEL, MNEM, SRCT, DSTT, DW, SRCIN) do {                       \
    SRCT s_ = (SRCT)(SRCIN); DSTT d_ = 0;                                  \
    __asm__(MNEM " %[s], %[d]" : [d] "=r"(d_) : [s] "r"(s_));               \
    dc_emit(LABEL, DW, (uint64_t)(SRCT)(SRCIN), 0, (uint64_t)d_, 0, 0);    \
} while (0)

/* accumulator sign-extend in place: A-reg <- signext(low part). */
#define CSX(LABEL, MNEM, SRCT, DSTT, DW, SRCIN) do {                       \
    DSTT a_ = (DSTT)(SRCT)(SRCIN);                                         \
    __asm__(MNEM : "+a"(a_));                                              \
    dc_emit(LABEL, DW, (uint64_t)(SRCT)(SRCIN), 0, (uint64_t)a_, 0, 0);    \
} while (0)

/* sign-fill the D-reg from the A-reg: cwd/cdq/cqo -> D = sign(A). */
#define CSF(LABEL, MNEM, T, DW, SRCIN) do {                                \
    T a_ = (T)(SRCIN); T d_ = 0;                                           \
    __asm__(MNEM : "=d"(d_) : "a"(a_));                                    \
    dc_emit(LABEL, DW, (uint64_t)a_, 0, (uint64_t)d_, 0, 0);               \
} while (0)

static void run_cases(void) {
    for (size_t i = 0; i < NPAT; i++) {
        uint64_t v = PAT[i];
        EXT("movsbw", "movsbw", uint8_t,  uint16_t, 16, v);
        EXT("movzbw", "movzbw", uint8_t,  uint16_t, 16, v);
        EXT("movsbl", "movsbl", uint8_t,  uint32_t, 32, v);
        EXT("movzbl", "movzbl", uint8_t,  uint32_t, 32, v);
        EXT("movswl", "movswl", uint16_t, uint32_t, 32, v);
        EXT("movzwl", "movzwl", uint16_t, uint32_t, 32, v);
        CSX("cbw",  "cbtw", uint8_t,  uint16_t, 16, v);  /* AT&T cbtw = cbw  */
        CSX("cwde", "cwtl", uint16_t, uint32_t, 32, v);  /* AT&T cwtl = cwde */
        CSF("cwd",  "cwtd", uint16_t, 16, v);            /* AT&T cwtd = cwd  */
        CSF("cdq",  "cltd", uint32_t, 32, v);            /* AT&T cltd = cdq  */
#if HAVE_W64
        EXT("movsbq", "movsbq", uint8_t,  uint64_t, 64, v);
        EXT("movzbq", "movzbq", uint8_t,  uint64_t, 64, v);
        EXT("movswq", "movswq", uint16_t, uint64_t, 64, v);
        EXT("movzwq", "movzwq", uint16_t, uint64_t, 64, v);
        EXT("movslq", "movslq", uint32_t, uint64_t, 64, v);  /* movsxd */
        CSX("cdqe", "cltq", uint32_t, uint64_t, 64, v);      /* AT&T cltq = cdqe */
        CSF("cqo",  "cqto", uint64_t, 64, v);                /* AT&T cqto = cqo  */
#endif
    }
}
