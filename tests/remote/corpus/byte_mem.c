/*
 * byte_mem — differential test for amd64 BYTE (8-bit) ALU on MEMORY operands:
 * TEST byte [mem], imm8 (0xf6 /0) and op byte [mem], imm8 (0x80 /0,/1,/4,/5,/6,/7)
 * RMW. These are the cc1 byte memory bridges the native gadgets target. Result
 * (the byte in memory after the op) + DEFINED EFLAGS captured via pushf, compared
 * interp==jit==oracle. Warm loop forces the JIT block to compile.
 */
#include "diff_common.h"

#define LOGIC_DEF (FL_ALL & ~FL_AF)
#define WARM 64

static const uint64_t APAT[] = { 0x00, 0x01, 0x02, 0x7f, 0x80, 0x81, 0xfe, 0xff, 0x55, 0xaa, 0x3c };
#define NAP (sizeof APAT / sizeof APAT[0])

static volatile unsigned char membuf;

/* op byte [mem], imm8 (RMW): result is the byte left in memory */
#define MEM_OP(MNEM, NAME, A, IMM, DEF) do {                                 \
    unsigned long f_ = 0;                                                    \
    for (int w_ = 0; w_ < WARM; w_++) {                                      \
        membuf = (unsigned char)(A);                                         \
        __asm__ volatile(MNEM " $" #IMM ", %[m]\n\t" PUSHF "\n\tpop %[f]"     \
            : [m] "+m"(membuf), [f] "=r"(f_) :: "cc");                       \
    }                                                                        \
    dc_emit(NAME, 8, (A) & 0xff, (IMM) & 0xff, membuf, f_, DEF);             \
} while (0)

/* cmp/test byte [mem], imm8: flags only, memory unchanged */
#define MEM_NW(MNEM, NAME, A, IMM, DEF) do {                                 \
    unsigned long f_ = 0;                                                    \
    for (int w_ = 0; w_ < WARM; w_++) {                                      \
        membuf = (unsigned char)(A);                                         \
        __asm__ volatile(MNEM " $" #IMM ", %[m]\n\t" PUSHF "\n\tpop %[f]"     \
            : [f] "=r"(f_) : [m] "m"(membuf) : "cc");                        \
    }                                                                        \
    dc_emit(NAME, 8, (A) & 0xff, (IMM) & 0xff, (A) & 0xff, f_, DEF);         \
} while (0)

#define SWEEP_MEM(IMM) do {                                                  \
    for (size_t i = 0; i < NAP; i++) {                                       \
        uint64_t a = APAT[i];                                                \
        MEM_OP("addb", "addm", a, IMM, FL_ALL);                              \
        MEM_OP("orb",  "orm",  a, IMM, LOGIC_DEF);                           \
        MEM_OP("andb", "andm", a, IMM, LOGIC_DEF);                           \
        MEM_OP("subb", "subm", a, IMM, FL_ALL);                              \
        MEM_OP("xorb", "xorm", a, IMM, LOGIC_DEF);                           \
        MEM_NW("cmpb",  "cmpm",  a, IMM, FL_ALL);                            \
        MEM_NW("testb", "testm", a, IMM, LOGIC_DEF);                         \
    }                                                                        \
} while (0)

static void run_cases(void) {
    SWEEP_MEM(0x00); SWEEP_MEM(0x01); SWEEP_MEM(0x7f); SWEEP_MEM(0x80);
    SWEEP_MEM(0xff); SWEEP_MEM(0x3c); SWEEP_MEM(0xaa); SWEEP_MEM(0x10);
}
