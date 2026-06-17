/*
 * muldiv — differential test for mul/imul/div/idiv.
 *   mul  (1-op, unsigned): hi:lo result; CF=OF defined (set if hi != 0). SF/ZF/
 *        AF/PF undefined -> not compared.
 *   imul (2-op, signed): truncated product; CF=OF defined (set on signed
 *        overflow). Others undefined.
 *   div/idiv (1-op): quotient + remainder; ALL flags undefined -> compared as
 *        result only. Operands chosen so the quotient always fits (no #DE).
 *   #DE delivery: div-by-zero and INT_MIN/-1 idiv overflow must raise SIGFPE in
 *        the guest; we catch it and record that delivery happened (exercises the
 *        emulator's exception path, JIT and interp).
 */
#include "diff_common.h"
#include <signal.h>
#include <setjmp.h>

static const uint64_t PAT[] = { 0x0, 0x1, 0x2, 0x7f, 0x80, 0xff, 0x8000, 0xdeadbeef };
#define NPAT (sizeof PAT / sizeof PAT[0])
static const uint64_t DIVS[] = { 0x1, 0x2, 0x3, 0x7, 0x10, 0x64, 0xff };  /* positive, nonzero */
#define NDIV (sizeof DIVS / sizeof DIVS[0])

/* mul (1-op, unsigned): DX:AX / EDX:EAX / RDX:RAX = A * src */
#define MUL1(T, W, AIN, SRCIN) do {                                            \
    T a_ = (T)(AIN), d_ = 0, s_ = (T)(SRCIN); unsigned long f_;                \
    __asm__("mul %[s]\n\t" PUSHF "\n\tpop %[f]"                                 \
        : "+a"(a_), "=d"(d_), [f] "=r"(f_) : [s] "r"(s_) : "cc");               \
    dc_emit("mul.lo", W, (uint64_t)(T)(AIN), (uint64_t)(T)(SRCIN), (uint64_t)a_, f_, FL_CF | FL_OF); \
    dc_emit("mul.hi", W, (uint64_t)(T)(AIN), (uint64_t)(T)(SRCIN), (uint64_t)d_, 0, 0); \
} while (0)

/* imul (2-op, signed): dst = dst * src (truncated) */
#define IMUL2(T, W, AIN, SRCIN) do {                                           \
    T d_ = (T)(AIN), s_ = (T)(SRCIN); unsigned long f_;                        \
    __asm__("imul %[s], %[d]\n\t" PUSHF "\n\tpop %[f]"                          \
        : [d] "+r"(d_), [f] "=r"(f_) : [s] "r"(s_) : "cc");                     \
    dc_emit("imul2", W, (uint64_t)(T)(AIN), (uint64_t)(T)(SRCIN), (uint64_t)d_, f_, FL_CF | FL_OF); \
} while (0)

/* div (1-op, unsigned): high half 0, so quotient = A / src always fits */
#define DIV1(T, W, AIN, SRCIN) do {                                            \
    T a_ = (T)(AIN), s_ = (T)(SRCIN); T q_, r_;                                \
    __asm__("div %[s]" : "=a"(q_), "=d"(r_) : "a"(a_), "d"((T)0), [s] "r"(s_) : "cc"); \
    dc_emit("div.q", W, (uint64_t)a_, (uint64_t)s_, (uint64_t)q_, 0, 0);       \
    dc_emit("div.r", W, (uint64_t)a_, (uint64_t)s_, (uint64_t)r_, 0, 0);       \
} while (0)

/* idiv (1-op, signed): high half = sign of A; positive divisor => no overflow */
#define IDIV1(T, W, AIN, SRCIN) do {                                           \
    T a_ = (T)(AIN), s_ = (T)(SRCIN);                                          \
    T hi_ = ((a_ >> ((W) - 1)) & 1) ? (T)~(T)0 : (T)0;                         \
    T q_, r_;                                                                  \
    __asm__("idiv %[s]" : "=a"(q_), "=d"(r_) : "a"(a_), "d"(hi_), [s] "r"(s_) : "cc"); \
    dc_emit("idiv.q", W, (uint64_t)a_, (uint64_t)s_, (uint64_t)q_, 0, 0);      \
    dc_emit("idiv.r", W, (uint64_t)a_, (uint64_t)s_, (uint64_t)r_, 0, 0);      \
} while (0)

static sigjmp_buf de_jmp;
static volatile int de_caught;
static void de_handler(int sig) { (void)sig; de_caught = 1; siglongjmp(de_jmp, 1); }

#define MULDIV_WIDTH(T, W) do {                                                \
    for (size_t i = 0; i < NPAT; i++) for (size_t j = 0; j < NPAT; j++) {      \
        MUL1(T, W, PAT[i], PAT[j]);                                           \
        IMUL2(T, W, PAT[i], PAT[j]);                                          \
    }                                                                          \
    for (size_t i = 0; i < NPAT; i++) for (size_t j = 0; j < NDIV; j++) {      \
        DIV1(T, W, PAT[i], DIVS[j]);                                          \
        IDIV1(T, W, PAT[i], DIVS[j]);                                         \
    }                                                                          \
} while (0)

static void run_cases(void) {
    MULDIV_WIDTH(uint16_t, 16);
    MULDIV_WIDTH(uint32_t, 32);
#if HAVE_W64
    MULDIV_WIDTH(uint64_t, 64);
#endif

    /* #DE delivery: each must raise SIGFPE, get caught, and record 1 */
    signal(SIGFPE, de_handler);
    de_caught = 0;
    if (sigsetjmp(de_jmp, 1) == 0) {
        volatile uint32_t a = 1, z = 0, q;
        __asm__ volatile("div %[z]" : "=a"(q) : "a"(a), "d"(0u), [z] "r"(z) : "cc");
    }
    dc_emit("de_div0", 32, 0, 0, (uint64_t)de_caught, 0, 0);

    de_caught = 0;
    if (sigsetjmp(de_jmp, 1) == 0) {
        volatile int32_t a = (int32_t)0x80000000, m = -1, q;
        __asm__ volatile("idiv %[m]" : "=a"(q) : "a"(a), "d"((int32_t)0xffffffff), [m] "r"(m) : "cc");
    }
    dc_emit("de_idivovf", 32, 0, 0, (uint64_t)de_caught, 0, 0);
}
