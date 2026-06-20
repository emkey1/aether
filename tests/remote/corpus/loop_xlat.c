/*
 * loop_xlat — differential test for LOOP/LOOPE/LOOPNE (0xe0-e2) and XLATB (0xd7).
 *
 * All four were unimplemented in BOTH amd64 engines (interp had no case; the JIT had
 * no gadget) -> guest execution SIGILL'd. This campaign adds the interp cases (all
 * four) + a native LOOP/LOOPE/LOOPNE gadget (XLATB bridges to the new interp case).
 * LOOP decrements RCX and branches if RCX!=0; LOOPE also requires ZF==1, LOOPNE ZF==0;
 * none modify flags. XLATB loads AL=[RBX+AL]. The captured value (loop iteration count
 * / xlat result) is compared interp==jit==oracle (Rosetta + real-Intel mint). Each case
 * is warmed so the inner loop's JIT block compiles.
 */
#include "diff_common.h"

#define WARM 48

/* The asm below pins 64-bit registers, so this is x86_64-only; the amd64 gadgets +
 * the new interp cases are what it targets. i386 cells emit no keys (key-based compare
 * handles that). The amd64 oracle is Rosetta/mint x86_64. */
#if defined(__x86_64__)

/* LOOP n times accumulating the running RCX value: result = n + (n-1) + ... + 1. */
static uint64_t do_loop(uint64_t n) {
    uint64_t acc = 0;
    for (int w = 0; w < WARM; w++) {
        acc = 0;
        __asm__ volatile(
            "mov %1, %%rcx\n\t"
            "1:\n\t"
            "add %%rcx, %0\n\t"
            "loop 1b\n\t"
            : "+r"(acc) : "r"(n) : "rcx", "cc");
    }
    return acc;
}

/* LOOPE: count leading bytes == 'A' in buf, up to n (loop while equal && rcx!=0). */
static uint64_t do_loope(const char *buf, uint64_t n) {
    uint64_t iters = 0;
    for (int w = 0; w < WARM; w++) {
        iters = 0;
        __asm__ volatile(
            "mov %2, %%rcx\n\t"
            "xor %%rsi, %%rsi\n\t"
            "1:\n\t"
            "movzbl (%1,%%rsi), %%eax\n\t"
            "add $1, %%rsi\n\t"
            "add $1, %0\n\t"
            "cmp $0x41, %%al\n\t"        /* ZF = (al == 'A') */
            "loope 1b\n\t"
            : "+r"(iters) : "r"(buf), "r"(n) : "rcx", "rsi", "rax", "cc");
    }
    return iters;
}

/* LOOPNE: count bytes until the first 'Z', up to n (loop while not-equal && rcx!=0). */
static uint64_t do_loopne(const char *buf, uint64_t n) {
    uint64_t iters = 0;
    for (int w = 0; w < WARM; w++) {
        iters = 0;
        __asm__ volatile(
            "mov %2, %%rcx\n\t"
            "xor %%rsi, %%rsi\n\t"
            "1:\n\t"
            "movzbl (%1,%%rsi), %%eax\n\t"
            "add $1, %%rsi\n\t"
            "add $1, %0\n\t"
            "cmp $0x5a, %%al\n\t"        /* ZF = (al == 'Z') */
            "loopne 1b\n\t"
            : "+r"(iters) : "r"(buf), "r"(n) : "rcx", "rsi", "rax", "cc");
    }
    return iters;
}

/* XLATB: AL = table[idx]. */
static uint64_t do_xlat(const unsigned char *table, unsigned idx) {
    uint64_t out = 0;
    for (int w = 0; w < WARM; w++) {
        __asm__ volatile(
            "mov %1, %%rbx\n\t"
            "movzbl %2, %%eax\n\t"
            "xlatb\n\t"
            "movzbl %%al, %k0\n\t"
            : "=r"(out) : "r"(table), "r"((unsigned char) idx)
            : "rax", "rbx", "cc");
    }
    return out;
}

static const char STR_A[] = "AAAAAAAAZZZZ----";   /* 8 'A's, then 'Z's */
static const char STR_Z[] = "xyZ-----ZZ------";    /* first 'Z' at index 2 */

static void run_cases(void) {
    unsigned char table[256];
    for (int i = 0; i < 256; i++)
        table[i] = (unsigned char) (i * 7 + 3);    /* arbitrary deterministic map */

    /* n starts at 1: LOOP with RCX=0 is the architectural 2^64-iteration form (correct
     * but an effective infinite loop -- untestable in finite time, same on real hw). */
    for (uint64_t n = 1; n <= 20; n++)
        dc_emit("loop", 64, n, 0, do_loop(n), 0, 0);

    for (uint64_t n = 1; n <= 12; n++) {
        dc_emit("lpe", 64, n, 0x41, do_loope(STR_A, n), 0, 0);
        dc_emit("lpn", 64, n, 0x5a, do_loopne(STR_Z, n), 0, 0);
    }

    for (unsigned idx = 0; idx < 256; idx++)
        dc_emit("xlat", 8, idx, 0, do_xlat(table, idx), 0, 0);
}

#else /* !__x86_64__ */
static void run_cases(void) { }   /* loop/xlatb tests are amd64-only here */
#endif
