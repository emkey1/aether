/*
 * neg_not — differential test for NOT (0xf7 /2) and NEG (0xf7 /3) at 16/32/64-bit.
 *
 * The amd64 JIT only had a native byte (0xf6) NOT/NEG gadget; the wider register forms
 * bridged to the interp (ending the JIT block). This campaign adds native 32/64-bit
 * gadgets (gadget_amd64_negnot32/64). NOT = ~src (no flags). NEG = 0 - src with sub
 * flags (CF=src!=0, OF at the sign bit, ZF/SF/PF/AF). 16-bit still bridges (exercised
 * here as a cross-check). Result + DEFINED EFLAGS captured via pushf, compared
 * interp==jit==oracle (Rosetta + real-Intel mint). Byte NOT/NEG is covered by byte_imm.
 */
#include "diff_common.h"

#define WARM 64

static const uint64_t APAT[] = {
    0x0, 0x1, 0x2, 0x7f, 0x80, 0xff, 0x100, 0x7fff, 0x8000, 0xffff, 0x1234,
    0x7fffffff, 0x80000000, 0xffffffff, 0x55555555, 0xaaaaaaaa, 0xdeadbeef,
    0x7fffffffffffffffULL, 0x8000000000000000ULL, 0xffffffffffffffffULL
};
#define NAP (sizeof APAT / sizeof APAT[0])

#define NN16(MNEM, NAME, A, DEF) do {                                        \
    unsigned short r_ = 0; unsigned long f_ = 0;                             \
    for (int w_ = 0; w_ < WARM; w_++) {                                      \
        r_ = (unsigned short)(A);                                            \
        __asm__ volatile(MNEM " %w[r]\n\t" PUSHF "\n\tpop %[f]"               \
            : [r] "+r"(r_), [f] "=r"(f_) :: "cc"); }                         \
    dc_emit(NAME, 16, (A) & 0xffff, 0, r_, f_, DEF);                         \
} while (0)

#define NN32(MNEM, NAME, A, DEF) do {                                        \
    uint32_t r_ = 0; unsigned long f_ = 0;                                   \
    for (int w_ = 0; w_ < WARM; w_++) {                                      \
        r_ = (uint32_t)(A);                                                  \
        __asm__ volatile(MNEM " %k[r]\n\t" PUSHF "\n\tpop %[f]"               \
            : [r] "+r"(r_), [f] "=r"(f_) :: "cc"); }                         \
    dc_emit(NAME, 32, (A) & 0xffffffff, 0, r_, f_, DEF);                     \
} while (0)

#if HAVE_W64
#define NN64(MNEM, NAME, A, DEF) do {                                        \
    uint64_t r_ = 0; unsigned long f_ = 0;                                   \
    for (int w_ = 0; w_ < WARM; w_++) {                                      \
        r_ = (uint64_t)(A);                                                  \
        __asm__ volatile(MNEM " %q[r]\n\t" PUSHF "\n\tpop %[f]"               \
            : [r] "+r"(r_), [f] "=r"(f_) :: "cc"); }                         \
    dc_emit(NAME, 64, (A), 0, r_, f_, DEF);                                  \
} while (0)
#endif

static void run_cases(void) {
    for (size_t i = 0; i < NAP; i++) {
        uint64_t a = APAT[i];
        NN16("not", "n16", a, 0);
        NN16("neg", "g16", a, FL_ALL);
        NN32("not", "n32", a, 0);
        NN32("neg", "g32", a, FL_ALL);
#if HAVE_W64
        NN64("not", "n64", a, 0);
        NN64("neg", "g64", a, FL_ALL);
#endif
    }
}
