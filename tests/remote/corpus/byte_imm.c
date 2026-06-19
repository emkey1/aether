/*
 * byte_imm — differential test for amd64 BYTE (8-bit) ALU on REGISTER operands
 * that flags_alu doesn't cover: the `op r/m8, imm8` (0x80) form, the legacy
 * high-byte registers AH/CH/DH/BH, and the rex r8b form. These are the cc1 hot
 * path the native byte-ALU gadgets target. Result + DEFINED EFLAGS captured via
 * pushf, compared interp==jit==oracle (Rosetta/mint). Each case is warmed in a
 * loop so its JIT block actually compiles (cold blocks run the interpreter).
 *
 * High-byte / r8b cases are x86_64-only (amd64 gadgets); i386 cells omit those
 * keys, which key-based comparison handles.
 */
#include "diff_common.h"

#define LOGIC_DEF (FL_ALL & ~FL_AF)   /* AF undefined after and/or/xor/test */
#define WARM 64                       /* force the block hot so the JIT compiles it */

static const uint64_t APAT[] = { 0x00, 0x01, 0x02, 0x7f, 0x80, 0x81, 0xfe, 0xff, 0x55, 0xaa, 0x3c };
#define NAP (sizeof APAT / sizeof APAT[0])

/* byte op r/m8, imm8 on a LOW byte register (al/bl/cl/dl via "+q") */
#define IMM2(MNEM, NAME, A, IMM, DEF) do {                                   \
    unsigned char r_ = 0; unsigned long f_ = 0;                              \
    for (int w_ = 0; w_ < WARM; w_++) {                                      \
        r_ = (unsigned char)(A);                                             \
        __asm__ volatile(MNEM " $" #IMM ", %b[r]\n\t" PUSHF "\n\tpop %[f]"   \
            : [r] "+q"(r_), [f] "=r"(f_) :: "cc");                           \
    }                                                                        \
    dc_emit(NAME, 8, (A) & 0xff, (IMM) & 0xff, r_, f_, DEF);                 \
} while (0)

/* cmp/test r/m8, imm8 (no result change) */
#define IMM2_NW(MNEM, NAME, A, IMM, DEF) do {                                \
    unsigned char r_ = 0; unsigned long f_ = 0;                              \
    for (int w_ = 0; w_ < WARM; w_++) {                                      \
        r_ = (unsigned char)(A);                                             \
        __asm__ volatile(MNEM " $" #IMM ", %b[r]\n\t" PUSHF "\n\tpop %[f]"   \
            : [r] "+q"(r_), [f] "=r"(f_) :: "cc");                           \
    }                                                                        \
    dc_emit(NAME, 8, (A) & 0xff, (IMM) & 0xff, (A) & 0xff, f_, DEF);         \
} while (0)

#if defined(__x86_64__)
/* op r/m8, imm8 on the legacy HIGH byte %ah (value in bits 8-15). The value is
 * pinned to %rsi (input "S") so it never collides with the clobbered %rax. */
#define IMM2_AH(MNEM, NAME, A, IMM, DEF) do {                                \
    unsigned long in_ = ((unsigned long)(unsigned char)(A)) << 8;            \
    unsigned long res_ = 0, f_ = 0;                                          \
    for (int w_ = 0; w_ < WARM; w_++) {                                      \
        __asm__ volatile("mov %%rsi, %%rax\n\t"                              \
            MNEM " $" #IMM ", %%ah\n\t" PUSHF "\n\tpop %[f]\n\t"             \
            "movzbl %%ah, %%eax\n\tmov %%rax, %[r]"                          \
            : [r] "=&r"(res_), [f] "=&r"(f_) : "S"(in_) : "rax", "cc");      \
    }                                                                        \
    dc_emit(NAME, 8, (A) & 0xff, (IMM) & 0xff, res_, f_, DEF);               \
} while (0)

/* op r/m8, imm8 on %r8b (rex.b low byte). Value pinned to %rsi. */
#define IMM2_R8(MNEM, NAME, A, IMM, DEF) do {                                \
    unsigned long in_ = (unsigned char)(A);                                  \
    unsigned long res_ = 0, f_ = 0;                                          \
    for (int w_ = 0; w_ < WARM; w_++) {                                      \
        __asm__ volatile("mov %%rsi, %%r8\n\t"                               \
            MNEM " $" #IMM ", %%r8b\n\t" PUSHF "\n\tpop %[f]\n\t"            \
            "movzbl %%r8b, %%r8d\n\tmov %%r8, %[r]"                          \
            : [r] "=&r"(res_), [f] "=&r"(f_) : "S"(in_) : "r8", "cc");       \
    }                                                                        \
    dc_emit(NAME, 8, (A) & 0xff, (IMM) & 0xff, res_, f_, DEF);               \
} while (0)

/* reg-reg with a HIGH-byte destination: op %cl, %ah (src low cl, dst high ah).
 * AH value in %rsi (->rax), src in %rcx ("c"); %rax clobbered, %rcx preserved. */
#define RR_AH(MNEM, NAME, A, B, DEF) do {                                    \
    unsigned long in_ = ((unsigned long)(unsigned char)(A)) << 8;            \
    unsigned long res_ = 0, f_ = 0;                                          \
    for (int w_ = 0; w_ < WARM; w_++) {                                      \
        __asm__ volatile("mov %%rsi, %%rax\n\t"                              \
            MNEM " %%cl, %%ah\n\t" PUSHF "\n\tpop %[f]\n\t"                  \
            "movzbl %%ah, %%eax\n\tmov %%rax, %[r]"                          \
            : [r] "=&r"(res_), [f] "=&r"(f_)                                 \
            : "S"(in_), "c"((unsigned long)(unsigned char)(B))              \
            : "rax", "cc"); }                                                \
    dc_emit(NAME, 8, (A) & 0xff, (B) & 0xff, res_, f_, DEF);                 \
} while (0)

/* NOT/NEG on %ah */
#define ONE_AH(MNEM, NAME, A, DEF) do {                                      \
    unsigned long in_ = ((unsigned long)(unsigned char)(A)) << 8;            \
    unsigned long res_ = 0, f_ = 0;                                          \
    for (int w_ = 0; w_ < WARM; w_++) {                                      \
        __asm__ volatile("mov %%rsi, %%rax\n\t"                              \
            MNEM " %%ah\n\t" PUSHF "\n\tpop %[f]\n\t"                        \
            "movzbl %%ah, %%eax\n\tmov %%rax, %[r]"                          \
            : [r] "=&r"(res_), [f] "=&r"(f_) : "S"(in_) : "rax", "cc");      \
    }                                                                        \
    dc_emit(NAME, 8, (A) & 0xff, 0, res_, f_, DEF);                          \
} while (0)
#endif /* __x86_64__ */

/* NOT/NEG on a low byte register */
#define ONE(MNEM, NAME, A, DEF) do {                                         \
    unsigned char r_ = 0; unsigned long f_ = 0;                              \
    for (int w_ = 0; w_ < WARM; w_++) {                                      \
        r_ = (unsigned char)(A);                                             \
        __asm__ volatile(MNEM " %b[r]\n\t" PUSHF "\n\tpop %[f]"              \
            : [r] "+q"(r_), [f] "=r"(f_) :: "cc"); }                         \
    dc_emit(NAME, 8, (A) & 0xff, 0, r_, f_, DEF);                            \
} while (0)

/* Run every op for one immediate value across all A patterns. */
#define SWEEP_IMM(IMM) do {                                                  \
    for (size_t i = 0; i < NAP; i++) {                                       \
        uint64_t a = APAT[i];                                                \
        IMM2("add", "addi", a, IMM, FL_ALL);                                 \
        IMM2("or",  "ori",  a, IMM, LOGIC_DEF);                              \
        IMM2("and", "andi", a, IMM, LOGIC_DEF);                              \
        IMM2("sub", "subi", a, IMM, FL_ALL);                                 \
        IMM2("xor", "xori", a, IMM, LOGIC_DEF);                              \
        IMM2_NW("cmp",  "cmpi",  a, IMM, FL_ALL);                            \
        IMM2_NW("test", "testi", a, IMM, LOGIC_DEF);                         \
    }                                                                        \
} while (0)

#if defined(__x86_64__)
#define SWEEP_IMM_HI(IMM) do {                                               \
    for (size_t i = 0; i < NAP; i++) {                                       \
        uint64_t a = APAT[i];                                                \
        IMM2_AH("add", "addh", a, IMM, FL_ALL);                              \
        IMM2_AH("and", "andh", a, IMM, LOGIC_DEF);                           \
        IMM2_AH("sub", "subh", a, IMM, FL_ALL);                              \
        IMM2_AH("xor", "xorh", a, IMM, LOGIC_DEF);                           \
        IMM2_AH("cmp", "cmph", a, IMM, FL_ALL);    /* cmp result == dst */    \
        IMM2_AH("test", "tsth", a, IMM, LOGIC_DEF);/* high-byte TEST r/m8,imm8 */\
        IMM2_R8("add", "add8", a, IMM, FL_ALL);                              \
        IMM2_R8("and", "and8", a, IMM, LOGIC_DEF);                           \
        IMM2_R8("sub", "sub8", a, IMM, FL_ALL);                              \
        IMM2_R8("cmp", "cmp8", a, IMM, FL_ALL);                              \
    }                                                                        \
} while (0)
#endif

static void run_cases(void) {
    SWEEP_IMM(0x00); SWEEP_IMM(0x01); SWEEP_IMM(0x7f); SWEEP_IMM(0x80);
    SWEEP_IMM(0xff); SWEEP_IMM(0x3c); SWEEP_IMM(0xaa); SWEEP_IMM(0x10);
    for (size_t i = 0; i < NAP; i++) {
        uint64_t a = APAT[i];
        ONE("not", "noti", a, 0);
        ONE("neg", "negi", a, FL_ALL);
    }
#if defined(__x86_64__)
    SWEEP_IMM_HI(0x00); SWEEP_IMM_HI(0x01); SWEEP_IMM_HI(0x7f);
    SWEEP_IMM_HI(0x80); SWEEP_IMM_HI(0xff); SWEEP_IMM_HI(0x3c);
    for (size_t i = 0; i < NAP; i++) for (size_t j = 0; j < NAP; j++) {
        uint64_t a = APAT[i], b = APAT[j];
        RR_AH("add", "addrh", a, b, FL_ALL);
        RR_AH("sub", "subrh", a, b, FL_ALL);
        RR_AH("and", "andrh", a, b, LOGIC_DEF);
        RR_AH("xor", "xorrh", a, b, LOGIC_DEF);
        RR_AH("cmp", "cmprh", a, b, FL_ALL);
    }
    for (size_t i = 0; i < NAP; i++) {
        uint64_t a = APAT[i];
        ONE_AH("not", "noth", a, 0);
        ONE_AH("neg", "negh", a, FL_ALL);
    }
#endif
}
