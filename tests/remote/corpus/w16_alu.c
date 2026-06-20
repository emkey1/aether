/*
 * w16_alu — differential test for amd64 16-bit (operand-size 0x66) integer ALU.
 *
 * The native arith reg gadgets gated on size 32/64 only (their CF/OF come from the
 * host's 32-bit flags), so 16-bit ADD/SUB/CMP reg-imm (0x81/0x83) and reg-reg
 * (0x01/03/29/2b/39/3b) bridged. This campaign adds width-16 native gadgets for them
 * (gadget_amd64_w16_alu_reg_imm / _reg_reg). 16-bit OR/AND/XOR/TEST were already
 * native (logic flags are width-agnostic) and are exercised here as a cross-check;
 * NOT/NEG still bridge. Result + DEFINED EFLAGS captured via pushf, compared
 * interp==jit==oracle (Rosetta + real-Intel mint). Each case is warmed so its JIT
 * block compiles.
 */
#include "diff_common.h"

#define LOGIC_DEF (FL_ALL & ~FL_AF)   /* AF undefined after and/or/xor/test */
#define WARM 64

static const uint64_t APAT[] = {
    0x0000, 0x0001, 0x0002, 0x7fff, 0x8000, 0x8001, 0xfffe, 0xffff,
    0x5555, 0xaaaa, 0x1234, 0x00ff, 0xff00, 0x0080
};
#define NAP (sizeof APAT / sizeof APAT[0])

/* op r/m16, imm16 (or imm8-sign-extended via 0x83 for small values) */
#define IMM2(MNEM, NAME, A, IMM, DEF) do {                                   \
    unsigned short r_ = 0; unsigned long f_ = 0;                             \
    for (int w_ = 0; w_ < WARM; w_++) {                                      \
        r_ = (unsigned short)(A);                                            \
        __asm__ volatile(MNEM " $" #IMM ", %w[r]\n\t" PUSHF "\n\tpop %[f]"    \
            : [r] "+r"(r_), [f] "=r"(f_) :: "cc"); }                         \
    dc_emit(NAME, 16, (A) & 0xffff, (IMM) & 0xffff, r_, f_, DEF);            \
} while (0)

/* cmp/test r/m16, imm16 (result unchanged) */
#define IMM2_NW(MNEM, NAME, A, IMM, DEF) do {                                \
    unsigned short r_ = 0; unsigned long f_ = 0;                             \
    for (int w_ = 0; w_ < WARM; w_++) {                                      \
        r_ = (unsigned short)(A);                                            \
        __asm__ volatile(MNEM " $" #IMM ", %w[r]\n\t" PUSHF "\n\tpop %[f]"    \
            : [r] "+r"(r_), [f] "=r"(f_) :: "cc"); }                         \
    dc_emit(NAME, 16, (A) & 0xffff, (IMM) & 0xffff, (A) & 0xffff, f_, DEF);  \
} while (0)

/* op r/m16, r16 */
#define RR(MNEM, NAME, A, B, DEF) do {                                       \
    unsigned short r_ = 0; unsigned long f_ = 0;                             \
    for (int w_ = 0; w_ < WARM; w_++) {                                      \
        r_ = (unsigned short)(A);                                            \
        __asm__ volatile(MNEM " %w[b], %w[r]\n\t" PUSHF "\n\tpop %[f]"        \
            : [r] "+r"(r_), [f] "=r"(f_) : [b] "r"((unsigned short)(B))      \
            : "cc"); }                                                       \
    dc_emit(NAME, 16, (A) & 0xffff, (B) & 0xffff, r_, f_, DEF);              \
} while (0)

/* cmp/test r/m16, r16 (result unchanged) */
#define RR_NW(MNEM, NAME, A, B, DEF) do {                                    \
    unsigned short r_ = 0; unsigned long f_ = 0;                             \
    for (int w_ = 0; w_ < WARM; w_++) {                                      \
        r_ = (unsigned short)(A);                                            \
        __asm__ volatile(MNEM " %w[b], %w[r]\n\t" PUSHF "\n\tpop %[f]"        \
            : [r] "+r"(r_), [f] "=r"(f_) : [b] "r"((unsigned short)(B))      \
            : "cc"); }                                                       \
    dc_emit(NAME, 16, (A) & 0xffff, (B) & 0xffff, (A) & 0xffff, f_, DEF);    \
} while (0)

/* not/neg r/m16 */
#define ONE(MNEM, NAME, A, DEF) do {                                         \
    unsigned short r_ = 0; unsigned long f_ = 0;                             \
    for (int w_ = 0; w_ < WARM; w_++) {                                      \
        r_ = (unsigned short)(A);                                            \
        __asm__ volatile(MNEM " %w[r]\n\t" PUSHF "\n\tpop %[f]"              \
            : [r] "+r"(r_), [f] "=r"(f_) :: "cc"); }                         \
    dc_emit(NAME, 16, (A) & 0xffff, 0, r_, f_, DEF);                         \
} while (0)

#define SWEEP_IMM(IMM) do {                                                  \
    for (size_t i = 0; i < NAP; i++) {                                       \
        uint64_t a = APAT[i];                                                \
        IMM2("add", "wadi", a, IMM, FL_ALL);                                 \
        IMM2("or",  "wori", a, IMM, LOGIC_DEF);                              \
        IMM2("and", "wani", a, IMM, LOGIC_DEF);                              \
        IMM2("sub", "wsui", a, IMM, FL_ALL);                                 \
        IMM2("xor", "wxoi", a, IMM, LOGIC_DEF);                              \
        IMM2_NW("cmp",  "wcmi", a, IMM, FL_ALL);                             \
        IMM2_NW("test", "wtsi", a, IMM, LOGIC_DEF);                          \
    }                                                                        \
} while (0)

static void run_cases(void) {
    SWEEP_IMM(0x0000); SWEEP_IMM(0x0001); SWEEP_IMM(0x7fff); SWEEP_IMM(0x8000);
    SWEEP_IMM(0xffff); SWEEP_IMM(0x1234); SWEEP_IMM(0x007f); SWEEP_IMM(0x0080);
    for (size_t i = 0; i < NAP; i++)
        for (size_t j = 0; j < NAP; j++) {
            uint64_t a = APAT[i], b = APAT[j];
            RR("add", "wadr", a, b, FL_ALL);
            RR("or",  "worr", a, b, LOGIC_DEF);
            RR("and", "wanr", a, b, LOGIC_DEF);
            RR("sub", "wsur", a, b, FL_ALL);
            RR("xor", "wxor", a, b, LOGIC_DEF);
            RR_NW("cmp",  "wcmr", a, b, FL_ALL);
            RR_NW("test", "wtsr", a, b, LOGIC_DEF);
        }
    for (size_t i = 0; i < NAP; i++) {
        uint64_t a = APAT[i];
        ONE("not", "wnot", a, 0);
        ONE("neg", "wneg", a, FL_ALL);
    }
}
