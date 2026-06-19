/*
 * adcsbb_reg — differential test for REGISTER adc/sbb (reg-reg + reg-imm) with carry-in.
 *
 * The memory-operand adc/sbb were already native (adc_sbb_mem covers them); the register
 * forms bridged to the interp (ending the JIT block). This campaign adds native gadgets
 * (gadget_amd64_adcsbb_rr32/64 + _ri32/64) using the same ARM carry trick as loadop_addc.
 * adc/sbb are the family the harness originally caught flag bugs in (AF/OF carry-fold),
 * so this guards the register path too. Carry-in is encoded in the op name (…0/…1) so the
 * (op,width,a,b) key stays distinct. Result + all EFLAGS, compared interp==jit==oracle
 * (Rosetta + real-Intel mint). Byte/16-bit reg forms bridge but are exercised as a check.
 */
#include "diff_common.h"

static const uint64_t PAT[] = {
    0x0, 0x1, 0x7f, 0x80, 0xff, 0x100, 0x7fff, 0x8000, 0xffff,
    0x7fffffff, 0x80000000, 0xffffffff, 0x12345678
};
#define NPAT (sizeof PAT / sizeof PAT[0])

/* reg-reg adc/sbb with caller-chosen carry-in (DC_ALU2_CF from diff_common.h). */
#define RR(MNEM, NAME, W, CT, A, B, CF) do {                                 \
    uint64_t res_; unsigned long fl_;                                        \
    DC_ALU2_CF(MNEM, CT, A, B, CF, res_, fl_);                               \
    dc_emit(NAME, W, A, B, res_, fl_, FL_ALL);                               \
} while (0)

#define RUN_RR(W, CT) do {                                                   \
    for (size_t i = 0; i < NPAT; i++) for (size_t j = 0; j < NPAT; j++) {     \
        uint64_t a = PAT[i], b = PAT[j];                                     \
        RR("adc", "Radc0", W, CT, a, b, 0);                                  \
        RR("adc", "Radc1", W, CT, a, b, 1);                                  \
        RR("sbb", "Rsbb0", W, CT, a, b, 0);                                  \
        RR("sbb", "Rsbb1", W, CT, a, b, 1);                                  \
    }                                                                        \
} while (0)

/* reg-imm adc/sbb with a literal immediate + carry-in. The imm goes in dc_emit's b
 * field, so distinct immediates produce distinct keys. */
#define RI(MNEM, NAME, W, CT, A, IMM, CF) do {                               \
    CT r_ = (CT)(A); unsigned long f_; unsigned char cf_ = (CF);             \
    __asm__ volatile("add $0xff, %b[cf]\n\t" MNEM " $" #IMM ", %[r]\n\t"      \
                     PUSHF "\n\tpop %[f]"                                     \
        : [r] "+r"(r_), [f] "=r"(f_), [cf] "+q"(cf_) :: "cc");               \
    dc_emit(NAME, W, A, (uint64_t)(IMM), (uint64_t)r_, f_, FL_ALL);          \
} while (0)

#define RUN_RI_IMM(W, CT, IMM) do {                                          \
    for (size_t i = 0; i < NPAT; i++) {                                      \
        uint64_t a = PAT[i];                                                 \
        RI("adc", "Iadc0", W, CT, a, IMM, 0);                                \
        RI("adc", "Iadc1", W, CT, a, IMM, 1);                                \
        RI("sbb", "Isbb0", W, CT, a, IMM, 0);                                \
        RI("sbb", "Isbb1", W, CT, a, IMM, 1);                                \
    }                                                                        \
} while (0)

#define RUN_RI(W, CT) do {                                                   \
    RUN_RI_IMM(W, CT, 0x1);    RUN_RI_IMM(W, CT, 0x7f);                       \
    RUN_RI_IMM(W, CT, 0x80);   RUN_RI_IMM(W, CT, 0x7fffffff);                 \
    RUN_RI_IMM(W, CT, -1);     /* imm8 0xff sign-extended (valid imm32 form) */ \
} while (0)

static void run_cases(void) {
    RUN_RR(8, uint8_t);
    RUN_RR(16, uint16_t);
    RUN_RR(32, uint32_t);
    RUN_RI(32, uint32_t);
#if HAVE_W64
    RUN_RR(64, uint64_t);
    RUN_RI(64, uint64_t);
#endif
}
