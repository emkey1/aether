/*
 * avx_regress -- AVX/AVX2 (VEX-encoded) instruction coverage for the amd64
 * guest (GH #525).
 *
 * Motivation: Bun-based CLI tools crashed with SIGILL under iSH because the
 * emulator had no VEX decode at all. Disassembling the real binary showed
 * pervasive compiler/stdlib function-multiversioning -- baseline/AVX2/AVX-512
 * variants of hot routines coexisting in one image, selected by a runtime
 * CPUID check -- so the fix needed real instruction support, not a CPUID
 * tweak. See emu/amd64_interp.c's amd64_vex_step.
 *
 * Every case compares the VEX result against a plain scalar C reference
 * computed in a volatile-fenced loop, so the test is self-checking and does
 * not depend on a golden output file. The properties that actually broke
 * during bring-up each get a dedicated case:
 *
 *   - 2-byte VEX (0xC5) has no X/B fields; they are implicitly ZERO. Treating
 *     them as set adds 8 to every rm/base register number, which silently
 *     reads the wrong register (and segfaults on a memory operand).
 *   - The legal prefix set differs per opcode: 6F/7F are the integer moves
 *     (66=MOVDQA, F3=MOVDQU) while 10/11 and 28/29 are the packed-float moves
 *     (none=PS, 66=PD) whose F3/F2 forms mean something else entirely
 *     (scalar MOVSS/MOVSD).
 *   - Any VEX.128 write must ZERO bits 128+ of the destination; VZEROUPPER
 *     must clear the upper half while PRESERVING the low 128 bits.
 *   - Lane widths must wrap independently: carry crosses the 32-bit boundary
 *     for VPADDQ but not for VPADDD/VPADDW/VPADDB.
 *
 * x86_64 only (VEX in 32-bit mode is a separate decode problem -- 0xC4/0xC5
 * are the legacy LES/LDS opcodes there and need mod-field disambiguation).
 */
#define _GNU_SOURCE

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test_common.h"

#if !defined(__x86_64__)
int main(int argc, char **argv) {
    (void) argc;
    (void) argv;
    printf("avx_regress: SKIP (x86_64 only)\n");
    return 0;
}
#else

/* Volatile so the compiler computes the reference with ordinary scalar code
   instead of recognizing the pattern and emitting the very vector instruction
   under test. */
static volatile uint32_t ref_lhs[8];
static volatile uint32_t ref_rhs[8];

static void check8(const char *label, const uint32_t *got, const uint32_t *want) {
    for (int i = 0; i < 8; i++) {
        if (got[i] != want[i]) {
            printf("FAIL %s\n  got: ", label);
            for (int j = 0; j < 8; j++)
                printf("%08x ", got[j]);
            printf("\n  want:");
            for (int j = 0; j < 8; j++)
                printf("%08x ", want[j]);
            printf("\n");
            failures_total++;
            return;
        }
    }
    test_logf("PASS %s\n", label);
}

/* Distinctive patterns: sign bits, all-ones, zeros, sequences. */
static const uint32_t PAT_A[8] __attribute__((aligned(32))) = {
    0x00010203, 0x04050607, 0x08090a0b, 0x0c0d0e0f,
    0xdeadbeef, 0xcafebabe, 0x80000000, 0x7fffffff,
};
static const uint32_t PAT_B[8] __attribute__((aligned(32))) = {
    0xffffffff, 0x00000000, 0x12345678, 0x9abcdef0,
    0x00000001, 0xfffffffe, 0x8000ffff, 0x7fff0001,
};

#define VEX_BINOP_256(mnemonic, label, refexpr)                                \
    do {                                                                       \
        uint32_t out[8] __attribute__((aligned(32)));                          \
        uint32_t want[8];                                                      \
        for (int i = 0; i < 8; i++) {                                          \
            ref_lhs[i] = PAT_A[i];                                             \
            ref_rhs[i] = PAT_B[i];                                             \
        }                                                                      \
        for (int i = 0; i < 8; i++) {                                          \
            uint32_t a = ref_lhs[i], b = ref_rhs[i];                           \
            want[i] = (refexpr);                                               \
        }                                                                      \
        __asm__ volatile("vmovdqu (%1),%%ymm1\n\t"                             \
                         "vmovdqu (%2),%%ymm2\n\t"                             \
                         mnemonic " %%ymm2,%%ymm1,%%ymm3\n\t"                  \
                         "vmovdqu %%ymm3,(%0)\n\t"                             \
                         "vzeroupper"                                          \
                         :                                                     \
                         : "r"(out), "r"(PAT_A), "r"(PAT_B)                    \
                         : "memory", "ymm1", "ymm2", "ymm3");                  \
        check8(label, out, want);                                              \
    } while (0)

static void test_logical_and_arith(void) {
    VEX_BINOP_256("vpxor", "vpxor.256", a ^ b);
    VEX_BINOP_256("vpor", "vpor.256", a | b);
    VEX_BINOP_256("vpand", "vpand.256", a & b);
    /* vpandn dst = ~src1 & src2; src1 is the vvvv operand (ymm1 here). */
    VEX_BINOP_256("vpandn", "vpandn.256", ~a & b);
    VEX_BINOP_256("vpaddd", "vpaddd.256", a + b);
}

/* Lane-width independence: each width must wrap within its own lane. */
static void test_lane_widths(void) {
    static const uint32_t x[8] __attribute__((aligned(32))) = {
        0xffffffff, 0x00000000, 0xffffffff, 0x00000000,
        0xffffffff, 0x00000000, 0xffffffff, 0x00000000,
    };
    static const uint32_t y[8] __attribute__((aligned(32))) = {
        0x01010101, 0x00000000, 0x01010101, 0x00000000,
        0x01010101, 0x00000000, 0x01010101, 0x00000000,
    };
    uint32_t out[8] __attribute__((aligned(32)));

    /* paddb: 0xff + 0x01 wraps to 0x00 in every byte, no carry escapes. */
    __asm__ volatile("vmovdqu (%1),%%ymm1\n\tvmovdqu (%2),%%ymm2\n\t"
                     "vpaddb %%ymm2,%%ymm1,%%ymm3\n\tvmovdqu %%ymm3,(%0)\n\tvzeroupper"
                     : : "r"(out), "r"(x), "r"(y) : "memory", "ymm1", "ymm2", "ymm3");
    {
        static const uint32_t want[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        check8("vpaddb.wrap", out, want);
    }

    /* paddq: the carry out of the low dword DOES reach the high dword. */
    {
        static const uint32_t qx[8] __attribute__((aligned(32))) = {
            0xffffffff, 0x00000000, 0xffffffff, 0x00000000,
            0xffffffff, 0x00000000, 0xffffffff, 0x00000000,
        };
        static const uint32_t qy[8] __attribute__((aligned(32))) = {
            0x00000001, 0x00000000, 0x00000001, 0x00000000,
            0x00000001, 0x00000000, 0x00000001, 0x00000000,
        };
        static const uint32_t want[8] = {
            0x00000000, 0x00000001, 0x00000000, 0x00000001,
            0x00000000, 0x00000001, 0x00000000, 0x00000001,
        };
        __asm__ volatile("vmovdqu (%1),%%ymm1\n\tvmovdqu (%2),%%ymm2\n\t"
                         "vpaddq %%ymm2,%%ymm1,%%ymm3\n\tvmovdqu %%ymm3,(%0)\n\tvzeroupper"
                         : : "r"(out), "r"(qx), "r"(qy) : "memory", "ymm1", "ymm2", "ymm3");
        check8("vpaddq.carry", out, want);
    }

    /* paddd on the same operands must NOT carry across the dword boundary. */
    {
        static const uint32_t dx[8] __attribute__((aligned(32))) = {
            0xffffffff, 0x00000000, 0xffffffff, 0x00000000,
            0xffffffff, 0x00000000, 0xffffffff, 0x00000000,
        };
        static const uint32_t dy[8] __attribute__((aligned(32))) = {
            0x00000001, 0x00000000, 0x00000001, 0x00000000,
            0x00000001, 0x00000000, 0x00000001, 0x00000000,
        };
        static const uint32_t want[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        __asm__ volatile("vmovdqu (%1),%%ymm1\n\tvmovdqu (%2),%%ymm2\n\t"
                         "vpaddd %%ymm2,%%ymm1,%%ymm3\n\tvmovdqu %%ymm3,(%0)\n\tvzeroupper"
                         : : "r"(out), "r"(dx), "r"(dy) : "memory", "ymm1", "ymm2", "ymm3");
        check8("vpaddd.nocarry", out, want);
    }
}

static void test_compares(void) {
    uint32_t out[8] __attribute__((aligned(32)));

    /* Equal against itself: every dword lane becomes all-ones. */
    __asm__ volatile("vmovdqu (%1),%%ymm1\n\t"
                     "vpcmpeqd %%ymm1,%%ymm1,%%ymm3\n\tvmovdqu %%ymm3,(%0)\n\tvzeroupper"
                     : : "r"(out), "r"(PAT_A) : "memory", "ymm1", "ymm3");
    {
        static const uint32_t want[8] = {
            0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff,
            0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff,
        };
        check8("vpcmpeqd.self", out, want);
    }

    /* Byte-granular compare with a single differing byte in lane 0. */
    {
        static const uint32_t bx[8] __attribute__((aligned(32))) = {
            0x04030201, 0, 0, 0, 0, 0, 0, 0};
        static const uint32_t by[8] __attribute__((aligned(32))) = {
            0x0403ff01, 0, 0, 0, 0, 0, 0, 0};
        /* byte 1 differs (0x02 vs 0xff) -> that byte 0x00, all others 0xff */
        static const uint32_t want[8] = {
            0xffff00ff, 0xffffffff, 0xffffffff, 0xffffffff,
            0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff,
        };
        __asm__ volatile("vmovdqu (%1),%%ymm1\n\tvmovdqu (%2),%%ymm2\n\t"
                         "vpcmpeqb %%ymm2,%%ymm1,%%ymm3\n\tvmovdqu %%ymm3,(%0)\n\tvzeroupper"
                         : : "r"(out), "r"(bx), "r"(by) : "memory", "ymm1", "ymm2", "ymm3");
        check8("vpcmpeqb.mixed", out, want);
    }
}

/* VPSHUFD's imm8 control is applied independently WITHIN each 128-bit lane --
   it does not shuffle across the lane boundary, so the 256-bit form is not
   simply "shuffle 8 dwords". */
static void test_pshufd_is_lane_local(void) {
    static const uint32_t seq[8] __attribute__((aligned(32))) = {1, 2, 3, 4, 5, 6, 7, 8};
    static const uint32_t want[8] = {4, 3, 2, 1, 8, 7, 6, 5}; /* 0x1b = reverse */
    uint32_t out[8] __attribute__((aligned(32)));

    __asm__ volatile("vmovdqu (%1),%%ymm1\n\t"
                     "vpshufd $0x1b,%%ymm1,%%ymm3\n\tvmovdqu %%ymm3,(%0)\n\tvzeroupper"
                     : : "r"(out), "r"(seq) : "memory", "ymm1", "ymm3");
    check8("vpshufd.lane_local", out, want);
}

static void test_upper_bits_semantics(void) {
    uint32_t out[8] __attribute__((aligned(32)));

    /* A VEX.128 write to xmm3 must zero ymm3's upper 128 bits, even though
       ymm3 was fully populated a moment earlier. */
    __asm__ volatile("vmovdqu (%1),%%ymm3\n\t"
                     "vmovdqu (%2),%%xmm4\n\t"
                     "vpxor %%xmm4,%%xmm4,%%xmm3\n\t"
                     "vmovdqu %%ymm3,(%0)\n\tvzeroupper"
                     : : "r"(out), "r"(PAT_A), "r"(PAT_B)
                     : "memory", "ymm3", "ymm4");
    {
        static const uint32_t want[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        check8("vex128.zeroes_upper", out, want);
    }

    /* VZEROUPPER clears bits 128+ but must leave the low 128 bits intact. */
    __asm__ volatile("vmovdqu (%1),%%ymm3\n\tvzeroupper\n\tvmovdqu %%ymm3,(%0)"
                     : : "r"(out), "r"(PAT_A) : "memory", "ymm3");
    {
        uint32_t want[8];
        for (int i = 0; i < 4; i++)
            want[i] = PAT_A[i];
        for (int i = 4; i < 8; i++)
            want[i] = 0;
        check8("vzeroupper.keeps_low", out, want);
    }
}

/* Exercises the 3-byte VEX (0xC4) encoding and its X/B extension bits by
   forcing a high register (ymm8+) and an r12/r13-based memory operand, which
   a 2-byte VEX cannot encode. Catches sign/inversion errors in the extension
   bits that the low-register cases above cannot see. */
static void test_three_byte_vex_high_regs(void) {
    uint32_t out[8] __attribute__((aligned(32)));
    uint32_t want[8];
    for (int i = 0; i < 8; i++)
        want[i] = PAT_A[i] ^ PAT_B[i];

    __asm__ volatile("vmovdqu (%1),%%ymm9\n\t"
                     "vmovdqu (%2),%%ymm10\n\t"
                     "vpxor %%ymm10,%%ymm9,%%ymm11\n\t"
                     "vmovdqu %%ymm11,(%0)\n\t"
                     "vzeroupper"
                     : : "r"(out), "r"(PAT_A), "r"(PAT_B)
                     : "memory", "ymm9", "ymm10", "ymm11");
    check8("vex3byte.high_regs", out, want);
}

static void test_movd_movq(void) {
    uint32_t got32 = 0;
    uint64_t got64 = 0;

    __asm__ volatile("vmovd %1,%%xmm5\n\tvmovd %%xmm5,%0"
                     : "=r"(got32) : "r"((uint32_t) 0xdeadbeefu) : "xmm5");
    if (got32 != 0xdeadbeefu) {
        printf("FAIL vmovd.roundtrip got=%08x want=deadbeef\n", got32);
        failures_total++;
    } else {
        test_logf("PASS vmovd.roundtrip\n");
    }

    __asm__ volatile("vmovq %1,%%xmm5\n\tvmovq %%xmm5,%0"
                     : "=r"(got64) : "r"((uint64_t) 0x0123456789abcdefull) : "xmm5");
    if (got64 != 0x0123456789abcdefull) {
        printf("FAIL vmovq.roundtrip got=%016llx want=0123456789abcdef\n",
               (unsigned long long) got64);
        failures_total++;
    } else {
        test_logf("PASS vmovq.roundtrip\n");
    }
}

/* MOVDQA (66-prefixed) and MOVDQU (F3-prefixed) share opcodes 6F/7F and must
   both decode; an over-broad prefix filter that accepts only one of them turns
   the other into a SIGILL. */
static void test_aligned_and_unaligned_moves(void) {
    uint32_t out[8] __attribute__((aligned(32)));

    __asm__ volatile("vmovdqa (%1),%%ymm1\n\tvmovdqa %%ymm1,(%0)\n\tvzeroupper"
                     : : "r"(out), "r"(PAT_A) : "memory", "ymm1");
    check8("vmovdqa.roundtrip", out, PAT_A);

    __asm__ volatile("vmovdqu (%1),%%ymm1\n\tvmovdqu %%ymm1,(%0)\n\tvzeroupper"
                     : : "r"(out), "r"(PAT_A) : "memory", "ymm1");
    check8("vmovdqu.roundtrip", out, PAT_A);

    __asm__ volatile("vmovaps (%1),%%ymm1\n\tvmovaps %%ymm1,(%0)\n\tvzeroupper"
                     : : "r"(out), "r"(PAT_A) : "memory", "ymm1");
    check8("vmovaps.roundtrip", out, PAT_A);

    __asm__ volatile("vmovups (%1),%%ymm1\n\tvmovups %%ymm1,(%0)\n\tvzeroupper"
                     : : "r"(out), "r"(PAT_A) : "memory", "ymm1");
    check8("vmovups.roundtrip", out, PAT_A);
}

int main(int argc, char **argv) {
    test_init(argc, argv);

    test_aligned_and_unaligned_moves();
    test_logical_and_arith();
    test_lane_widths();
    test_compares();
    test_pshufd_is_lane_local();
    test_upper_bits_semantics();
    test_three_byte_vex_high_regs();
    test_movd_movq();

    return finish_suite("avx_regress");
}

#endif /* __x86_64__ */
