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


/* ---- coverage for the second implementation tier ---- */

/* Saturating arithmetic must clamp, not wrap. */
static void test_saturating(void) {
    uint32_t out[8] __attribute__((aligned(32)));
    /* bytes: 0x7f + 0x7f saturates to 0x7f signed; 0xff + 0xff to 0xff unsigned */
    static const uint32_t x[8] __attribute__((aligned(32))) = {
        0x7f7f7f7f, 0x80808080, 0xffffffff, 0x01010101,
        0x7f7f7f7f, 0x80808080, 0xffffffff, 0x01010101};
    static const uint32_t y[8] __attribute__((aligned(32))) = {
        0x7f7f7f7f, 0x80808080, 0xffffffff, 0x01010101,
        0x7f7f7f7f, 0x80808080, 0xffffffff, 0x01010101};

    __asm__ volatile("vmovdqu (%1),%%ymm1\n\tvmovdqu (%2),%%ymm2\n\t"
                     "vpaddsb %%ymm2,%%ymm1,%%ymm3\n\tvmovdqu %%ymm3,(%0)\n\tvzeroupper"
                     : : "r"(out), "r"(x), "r"(y) : "memory", "ymm1", "ymm2", "ymm3");
    {   /* 0x7f+0x7f->0x7f, 0x80+0x80->0x80, 0xff+0xff = -1 + -1 = -2 = 0xfe, 1+1=2 */
        static const uint32_t want[8] = {
            0x7f7f7f7f, 0x80808080, 0xfefefefe, 0x02020202,
            0x7f7f7f7f, 0x80808080, 0xfefefefe, 0x02020202};
        check8("vpaddsb.clamp", out, want);
    }

    __asm__ volatile("vmovdqu (%1),%%ymm1\n\tvmovdqu (%2),%%ymm2\n\t"
                     "vpaddusb %%ymm2,%%ymm1,%%ymm3\n\tvmovdqu %%ymm3,(%0)\n\tvzeroupper"
                     : : "r"(out), "r"(x), "r"(y) : "memory", "ymm1", "ymm2", "ymm3");
    {   /* unsigned: 0x7f+0x7f=0xfe, 0x80+0x80=0x100->0xff, 0xff+0xff->0xff, 1+1=2 */
        static const uint32_t want[8] = {
            0xfefefefe, 0xffffffff, 0xffffffff, 0x02020202,
            0xfefefefe, 0xffffffff, 0xffffffff, 0x02020202};
        check8("vpaddusb.clamp", out, want);
    }

    __asm__ volatile("vmovdqu (%1),%%ymm1\n\tvmovdqu (%2),%%ymm2\n\t"
                     "vpsubusb %%ymm2,%%ymm1,%%ymm3\n\tvmovdqu %%ymm3,(%0)\n\tvzeroupper"
                     : : "r"(out), "r"(x), "r"(y) : "memory", "ymm1", "ymm2", "ymm3");
    {   /* equal operands -> 0 everywhere, no borrow below zero */
        static const uint32_t want[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        check8("vpsubusb.floor", out, want);
    }
}

/* Shifts: by immediate, by xmm count, and per-lane variable. */
static void test_shifts(void) {
    uint32_t out[8] __attribute__((aligned(32)));
    static const uint32_t v[8] __attribute__((aligned(32))) = {
        0x00000001, 0x80000000, 0xffffffff, 0x00ff00ff,
        0x00000001, 0x80000000, 0xffffffff, 0x00ff00ff};

    __asm__ volatile("vmovdqu (%1),%%ymm1\n\tvpslld $4,%%ymm1,%%ymm3\n\t"
                     "vmovdqu %%ymm3,(%0)\n\tvzeroupper"
                     : : "r"(out), "r"(v) : "memory", "ymm1", "ymm3");
    {
        static const uint32_t want[8] = {
            0x00000010, 0x00000000, 0xfffffff0, 0x0ff00ff0,
            0x00000010, 0x00000000, 0xfffffff0, 0x0ff00ff0};
        check8("vpslld.imm", out, want);
    }

    /* Arithmetic vs logical right shift differ on the sign bit. */
    __asm__ volatile("vmovdqu (%1),%%ymm1\n\tvpsrad $4,%%ymm1,%%ymm3\n\t"
                     "vmovdqu %%ymm3,(%0)\n\tvzeroupper"
                     : : "r"(out), "r"(v) : "memory", "ymm1", "ymm3");
    {
        static const uint32_t want[8] = {
            0x00000000, 0xf8000000, 0xffffffff, 0x000ff00f,
            0x00000000, 0xf8000000, 0xffffffff, 0x000ff00f};
        check8("vpsrad.imm.sign", out, want);
    }
    __asm__ volatile("vmovdqu (%1),%%ymm1\n\tvpsrld $4,%%ymm1,%%ymm3\n\t"
                     "vmovdqu %%ymm3,(%0)\n\tvzeroupper"
                     : : "r"(out), "r"(v) : "memory", "ymm1", "ymm3");
    {
        static const uint32_t want[8] = {
            0x00000000, 0x08000000, 0x0fffffff, 0x000ff00f,
            0x00000000, 0x08000000, 0x0fffffff, 0x000ff00f};
        check8("vpsrld.imm.zero", out, want);
    }

    /* An over-wide count yields 0 (or all sign bits), not C's UB. */
    __asm__ volatile("vmovdqu (%1),%%ymm1\n\tvpslld $32,%%ymm1,%%ymm3\n\t"
                     "vmovdqu %%ymm3,(%0)\n\tvzeroupper"
                     : : "r"(out), "r"(v) : "memory", "ymm1", "ymm3");
    {
        static const uint32_t want[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        check8("vpslld.overshift", out, want);
    }

    /* Count taken from the low 64 bits of an xmm operand. */
    {
        static const uint32_t cnt[4] __attribute__((aligned(16))) = {8, 0, 0, 0};
        __asm__ volatile("vmovdqu (%1),%%ymm1\n\tvmovdqu (%2),%%xmm2\n\t"
                         "vpslld %%xmm2,%%ymm1,%%ymm3\n\tvmovdqu %%ymm3,(%0)\n\tvzeroupper"
                         : : "r"(out), "r"(v), "r"(cnt) : "memory", "ymm1", "ymm2", "ymm3");
        static const uint32_t want[8] = {
            0x00000100, 0x00000000, 0xffffff00, 0xff00ff00,
            0x00000100, 0x00000000, 0xffffff00, 0xff00ff00};
        check8("vpslld.xmmcount", out, want);
    }

    /* Per-lane variable shift: each lane uses its own count. */
    {
        static const uint32_t base[8] __attribute__((aligned(32))) = {1, 1, 1, 1, 1, 1, 1, 1};
        static const uint32_t counts[8] __attribute__((aligned(32))) = {0, 1, 2, 3, 4, 5, 6, 7};
        static const uint32_t want[8] = {1, 2, 4, 8, 16, 32, 64, 128};
        __asm__ volatile("vmovdqu (%1),%%ymm1\n\tvmovdqu (%2),%%ymm2\n\t"
                         "vpsllvd %%ymm2,%%ymm1,%%ymm3\n\tvmovdqu %%ymm3,(%0)\n\tvzeroupper"
                         : : "r"(out), "r"(base), "r"(counts) : "memory", "ymm1", "ymm2", "ymm3");
        check8("vpsllvd.perlane", out, want);
    }

    /* VPSLLDQ is a BYTE shift within each 128-bit lane, not a bit shift and
       not across the lane boundary. */
    {
        static const uint32_t seq[8] __attribute__((aligned(32))) = {
            0x03020100, 0x07060504, 0x0b0a0908, 0x0f0e0d0c,
            0x13121110, 0x17161514, 0x1b1a1918, 0x1f1e1d1c};
        static const uint32_t want[8] = {
            0x02010000, 0x06050403, 0x0a090807, 0x0e0d0c0b,
            0x12111000, 0x16151413, 0x1a191817, 0x1e1d1c1b};
        __asm__ volatile("vmovdqu (%1),%%ymm1\n\tvpslldq $1,%%ymm1,%%ymm3\n\t"
                         "vmovdqu %%ymm3,(%0)\n\tvzeroupper"
                         : : "r"(out), "r"(seq) : "memory", "ymm1", "ymm3");
        check8("vpslldq.lane_local", out, want);
    }
}

/* VPSHUFB is lane-local and its index bit 7 zeroes the output byte. */
static void test_pshufb(void) {
    uint32_t out[8] __attribute__((aligned(32)));
    static const uint32_t data[8] __attribute__((aligned(32))) = {
        0x03020100, 0x07060504, 0x0b0a0908, 0x0f0e0d0c,
        0x13121110, 0x17161514, 0x1b1a1918, 0x1f1e1d1c};
    /* select byte 0 everywhere in lane 0; 0x80 -> zero in lane 1 */
    static const uint32_t idx[8] __attribute__((aligned(32))) = {
        0x00000000, 0x00000000, 0x00000000, 0x00000000,
        0x80808080, 0x80808080, 0x80808080, 0x80808080};
    static const uint32_t want[8] = {
        0x00000000, 0x00000000, 0x00000000, 0x00000000,
        0x00000000, 0x00000000, 0x00000000, 0x00000000};
    __asm__ volatile("vmovdqu (%1),%%ymm1\n\tvmovdqu (%2),%%ymm2\n\t"
                     "vpshufb %%ymm2,%%ymm1,%%ymm3\n\tvmovdqu %%ymm3,(%0)\n\tvzeroupper"
                     : : "r"(out), "r"(data), "r"(idx) : "memory", "ymm1", "ymm2", "ymm3");
    check8("vpshufb.zero_and_lane0", out, want);

    /* Index 15 in the high lane must select byte 15 OF THAT LANE (0x1f), not
       byte 15 of the whole register -- the classic cross-lane mistake. */
    {
        static const uint32_t idx2[8] __attribute__((aligned(32))) = {
            0x0f0f0f0f, 0x0f0f0f0f, 0x0f0f0f0f, 0x0f0f0f0f,
            0x0f0f0f0f, 0x0f0f0f0f, 0x0f0f0f0f, 0x0f0f0f0f};
        static const uint32_t want2[8] = {
            0x0f0f0f0f, 0x0f0f0f0f, 0x0f0f0f0f, 0x0f0f0f0f,
            0x1f1f1f1f, 0x1f1f1f1f, 0x1f1f1f1f, 0x1f1f1f1f};
        __asm__ volatile("vmovdqu (%1),%%ymm1\n\tvmovdqu (%2),%%ymm2\n\t"
                         "vpshufb %%ymm2,%%ymm1,%%ymm3\n\tvmovdqu %%ymm3,(%0)\n\tvzeroupper"
                         : : "r"(out), "r"(data), "r"(idx2) : "memory", "ymm1", "ymm2", "ymm3");
        check8("vpshufb.lane_local", out, want2);
    }
}

static void test_unpack_and_pack(void) {
    uint32_t out[8] __attribute__((aligned(32)));
    static const uint32_t x[8] __attribute__((aligned(32))) = {
        0x03020100, 0x07060504, 0x0b0a0908, 0x0f0e0d0c,
        0x03020100, 0x07060504, 0x0b0a0908, 0x0f0e0d0c};
    static const uint32_t y[8] __attribute__((aligned(32))) = {
        0x13121110, 0x17161514, 0x1b1a1918, 0x1f1e1d1c,
        0x13121110, 0x17161514, 0x1b1a1918, 0x1f1e1d1c};

    __asm__ volatile("vmovdqu (%1),%%ymm1\n\tvmovdqu (%2),%%ymm2\n\t"
                     "vpunpcklbw %%ymm2,%%ymm1,%%ymm3\n\tvmovdqu %%ymm3,(%0)\n\tvzeroupper"
                     : : "r"(out), "r"(x), "r"(y) : "memory", "ymm1", "ymm2", "ymm3");
    {
        static const uint32_t want[8] = {
            0x11011000, 0x13031202, 0x15051404, 0x17071606,
            0x11011000, 0x13031202, 0x15051404, 0x17071606};
        check8("vpunpcklbw", out, want);
    }

    __asm__ volatile("vmovdqu (%1),%%ymm1\n\tvmovdqu (%2),%%ymm2\n\t"
                     "vpunpckhdq %%ymm2,%%ymm1,%%ymm3\n\tvmovdqu %%ymm3,(%0)\n\tvzeroupper"
                     : : "r"(out), "r"(x), "r"(y) : "memory", "ymm1", "ymm2", "ymm3");
    {
        static const uint32_t want[8] = {
            0x0b0a0908, 0x1b1a1918, 0x0f0e0d0c, 0x1f1e1d1c,
            0x0b0a0908, 0x1b1a1918, 0x0f0e0d0c, 0x1f1e1d1c};
        check8("vpunpckhdq", out, want);
    }

    /* packuswb clamps signed words into unsigned bytes: negative -> 0. */
    {
        static const uint32_t w1[8] __attribute__((aligned(32))) = {
            0x00ff0001, 0xffff0100, 0x00010002, 0x00030004,
            0x00ff0001, 0xffff0100, 0x00010002, 0x00030004};
        static const uint32_t w2[8] __attribute__((aligned(32))) = {
            0x00000000, 0x00000000, 0x00000000, 0x00000000,
            0x00000000, 0x00000000, 0x00000000, 0x00000000};
        /* words of w1 lane0: 0001,00ff,0100,ffff,0002,0001,0004,0003
           -> bytes:          01,  ff,  ff,  00,  02,  01,  04,  03   */
        static const uint32_t want[8] = {
            0x00ffff01, 0x03040102, 0x00000000, 0x00000000,
            0x00ffff01, 0x03040102, 0x00000000, 0x00000000};
        __asm__ volatile("vmovdqu (%1),%%ymm1\n\tvmovdqu (%2),%%ymm2\n\t"
                         "vpackuswb %%ymm2,%%ymm1,%%ymm3\n\tvmovdqu %%ymm3,(%0)\n\tvzeroupper"
                         : : "r"(out), "r"(w1), "r"(w2) : "memory", "ymm1", "ymm2", "ymm3");
        check8("vpackuswb.clamp", out, want);
    }
}

static void test_broadcast_and_widen(void) {
    uint32_t out[8] __attribute__((aligned(32)));

    {
        static const uint32_t src[4] __attribute__((aligned(16))) = {0xdeadbeef, 0, 0, 0};
        static const uint32_t want[8] = {
            0xdeadbeef, 0xdeadbeef, 0xdeadbeef, 0xdeadbeef,
            0xdeadbeef, 0xdeadbeef, 0xdeadbeef, 0xdeadbeef};
        __asm__ volatile("vmovdqu (%1),%%xmm1\n\tvpbroadcastd %%xmm1,%%ymm3\n\t"
                         "vmovdqu %%ymm3,(%0)\n\tvzeroupper"
                         : : "r"(out), "r"(src) : "memory", "ymm1", "ymm3");
        check8("vpbroadcastd", out, want);
    }
    {
        static const uint32_t src[4] __attribute__((aligned(16))) = {0x000000ab, 0, 0, 0};
        static const uint32_t want[8] = {
            0xabababab, 0xabababab, 0xabababab, 0xabababab,
            0xabababab, 0xabababab, 0xabababab, 0xabababab};
        __asm__ volatile("vmovdqu (%1),%%xmm1\n\tvpbroadcastb %%xmm1,%%ymm3\n\t"
                         "vmovdqu %%ymm3,(%0)\n\tvzeroupper"
                         : : "r"(out), "r"(src) : "memory", "ymm1", "ymm3");
        check8("vpbroadcastb", out, want);
    }
    /* Sign- vs zero-extension of the same negative bytes. */
    {
        static const uint32_t src[4] __attribute__((aligned(16))) = {
            0xff01ff01, 0xff01ff01, 0, 0};
        /* 0xff01ff01 stored little-endian is bytes 01,ff,01,ff -- the FIRST
           widened lane comes from byte 0 (0x01), not from the high byte. */
        static const uint32_t want_z[8] = {
            0x00000001, 0x000000ff, 0x00000001, 0x000000ff,
            0x00000001, 0x000000ff, 0x00000001, 0x000000ff};
        static const uint32_t want_s[8] = {
            0x00000001, 0xffffffff, 0x00000001, 0xffffffff,
            0x00000001, 0xffffffff, 0x00000001, 0xffffffff};
        __asm__ volatile("vmovdqu (%1),%%xmm1\n\tvpmovzxbd %%xmm1,%%ymm3\n\t"
                         "vmovdqu %%ymm3,(%0)\n\tvzeroupper"
                         : : "r"(out), "r"(src) : "memory", "ymm1", "ymm3");
        check8("vpmovzxbd", out, want_z);
        __asm__ volatile("vmovdqu (%1),%%xmm1\n\tvpmovsxbd %%xmm1,%%ymm3\n\t"
                         "vmovdqu %%ymm3,(%0)\n\tvzeroupper"
                         : : "r"(out), "r"(src) : "memory", "ymm1", "ymm3");
        check8("vpmovsxbd", out, want_s);
    }
}

/* Cross-lane operations -- the ones a lane-local implementation gets wrong. */
static void test_cross_lane(void) {
    uint32_t out[8] __attribute__((aligned(32)));
    static const uint32_t seq[8] __attribute__((aligned(32))) = {0, 1, 2, 3, 4, 5, 6, 7};

    /* vextracti128 $1 pulls the HIGH 128 bits down into an xmm. */
    __asm__ volatile("vmovdqu (%1),%%ymm1\n\tvextracti128 $1,%%ymm1,%%xmm3\n\t"
                     "vmovdqu %%ymm3,(%0)\n\tvzeroupper"
                     : : "r"(out), "r"(seq) : "memory", "ymm1", "ymm3");
    {
        static const uint32_t want[8] = {4, 5, 6, 7, 0, 0, 0, 0};
        check8("vextracti128.high", out, want);
    }

    /* vinserti128 $1 replaces the high half, keeping the low half of vvvv. */
    {
        static const uint32_t ins[4] __attribute__((aligned(16))) = {
            0xaaaaaaaa, 0xbbbbbbbb, 0xcccccccc, 0xdddddddd};
        static const uint32_t want[8] = {
            0, 1, 2, 3, 0xaaaaaaaa, 0xbbbbbbbb, 0xcccccccc, 0xdddddddd};
        __asm__ volatile("vmovdqu (%1),%%ymm1\n\tvmovdqu (%2),%%xmm2\n\t"
                         "vinserti128 $1,%%xmm2,%%ymm1,%%ymm3\n\t"
                         "vmovdqu %%ymm3,(%0)\n\tvzeroupper"
                         : : "r"(out), "r"(seq), "r"(ins) : "memory", "ymm1", "ymm2", "ymm3");
        check8("vinserti128.high", out, want);
    }

    /* vpermq crosses lanes: reverse the four qwords. */
    __asm__ volatile("vmovdqu (%1),%%ymm1\n\tvpermq $0x1b,%%ymm1,%%ymm3\n\t"
                     "vmovdqu %%ymm3,(%0)\n\tvzeroupper"
                     : : "r"(out), "r"(seq) : "memory", "ymm1", "ymm3");
    {
        static const uint32_t want[8] = {6, 7, 4, 5, 2, 3, 0, 1};
        check8("vpermq.reverse", out, want);
    }

    /* vperm2i128: imm 0x01 -> low=src1.hi, high=src1.lo (a 128-bit swap). */
    __asm__ volatile("vmovdqu (%1),%%ymm1\n\t"
                     "vperm2i128 $0x01,%%ymm1,%%ymm1,%%ymm3\n\t"
                     "vmovdqu %%ymm3,(%0)\n\tvzeroupper"
                     : : "r"(out), "r"(seq) : "memory", "ymm1", "ymm3");
    {
        static const uint32_t want[8] = {4, 5, 6, 7, 0, 1, 2, 3};
        check8("vperm2i128.swap", out, want);
    }

    /* imm bit 3 of a selector forces that whole 128-bit half to zero. */
    __asm__ volatile("vmovdqu (%1),%%ymm1\n\t"
                     "vperm2i128 $0x81,%%ymm1,%%ymm1,%%ymm3\n\t"
                     "vmovdqu %%ymm3,(%0)\n\tvzeroupper"
                     : : "r"(out), "r"(seq) : "memory", "ymm1", "ymm3");
    {
        /* Each 4-bit selector's bit 3 forces its half to zero, independently
           of the 2-bit source choice: low selector 0x1 -> src1.hi = {4,5,6,7};
           high selector 0x8 -> zeroed. */
        static const uint32_t want[8] = {4, 5, 6, 7, 0, 0, 0, 0};
        check8("vperm2i128.zero_half", out, want);
    }

    /* vpermd: fully general cross-lane dword gather (indices in vvvv). */
    {
        static const uint32_t idx[8] __attribute__((aligned(32))) = {7, 6, 5, 4, 3, 2, 1, 0};
        static const uint32_t want[8] = {7, 6, 5, 4, 3, 2, 1, 0};
        __asm__ volatile("vmovdqu (%1),%%ymm1\n\tvmovdqu (%2),%%ymm2\n\t"
                         "vpermd %%ymm1,%%ymm2,%%ymm3\n\t"
                         "vmovdqu %%ymm3,(%0)\n\tvzeroupper"
                         : : "r"(out), "r"(seq), "r"(idx) : "memory", "ymm1", "ymm2", "ymm3");
        check8("vpermd.reverse", out, want);
    }
}

/* vpalignr concatenates per 128-bit lane and slides a 16-byte window. */
static void test_palignr(void) {
    uint32_t out[8] __attribute__((aligned(32)));
    static const uint32_t hi[8] __attribute__((aligned(32))) = {
        0x13121110, 0x17161514, 0x1b1a1918, 0x1f1e1d1c,
        0x13121110, 0x17161514, 0x1b1a1918, 0x1f1e1d1c};
    static const uint32_t lo[8] __attribute__((aligned(32))) = {
        0x03020100, 0x07060504, 0x0b0a0908, 0x0f0e0d0c,
        0x03020100, 0x07060504, 0x0b0a0908, 0x0f0e0d0c};
    /* imm=4: window starts 4 bytes into the low source */
    static const uint32_t want[8] = {
        0x07060504, 0x0b0a0908, 0x0f0e0d0c, 0x13121110,
        0x07060504, 0x0b0a0908, 0x0f0e0d0c, 0x13121110};
    __asm__ volatile("vmovdqu (%1),%%ymm1\n\tvmovdqu (%2),%%ymm2\n\t"
                     "vpalignr $4,%%ymm2,%%ymm1,%%ymm3\n\tvmovdqu %%ymm3,(%0)\n\tvzeroupper"
                     : : "r"(out), "r"(hi), "r"(lo) : "memory", "ymm1", "ymm2", "ymm3");
    check8("vpalignr.imm4", out, want);
}

static void test_pmovmskb_and_madd(void) {
    uint32_t out[8] __attribute__((aligned(32)));

    /* vpmovmskb: one bit per byte MSB, 32 bits for a ymm source. */
    {
        static const uint32_t src[8] __attribute__((aligned(32))) = {
            0x80008000, 0x00000000, 0x00000000, 0x00000000,
            0x00000000, 0x00000000, 0x00000000, 0x80000000};
        uint32_t mask = 0;
        __asm__ volatile("vmovdqu (%1),%%ymm1\n\tvpmovmskb %%ymm1,%0\n\tvzeroupper"
                         : "=r"(mask) : "r"(src) : "ymm1");
        /* bytes 1 and 3 set in dword 0; byte 31 set */
        uint32_t want = (1u << 1) | (1u << 3) | (1u << 31);
        if (mask != want) {
            printf("FAIL vpmovmskb got=%08x want=%08x\n", mask, want);
            failures_total++;
        } else {
            test_logf("PASS vpmovmskb\n");
        }
    }

    /* vpmaddwd: signed word pairs multiplied and summed into dwords. */
    {
        static const uint32_t x[8] __attribute__((aligned(32))) = {
            0x00020001, 0xffffffff, 0x00010001, 0x00010001,
            0x00020001, 0xffffffff, 0x00010001, 0x00010001};
        static const uint32_t y[8] __attribute__((aligned(32))) = {
            0x00040003, 0x00020002, 0x00010001, 0x00010001,
            0x00040003, 0x00020002, 0x00010001, 0x00010001};
        /* dword0: 1*3 + 2*4 = 11; dword1: (-1)*2 + (-1)*2 = -4 */
        static const uint32_t want[8] = {
            11, (uint32_t) -4, 2, 2,
            11, (uint32_t) -4, 2, 2};
        __asm__ volatile("vmovdqu (%1),%%ymm1\n\tvmovdqu (%2),%%ymm2\n\t"
                         "vpmaddwd %%ymm2,%%ymm1,%%ymm3\n\tvmovdqu %%ymm3,(%0)\n\tvzeroupper"
                         : : "r"(out), "r"(x), "r"(y) : "memory", "ymm1", "ymm2", "ymm3");
        check8("vpmaddwd.signed", out, want);
    }

    /* vpsadbw: sum of absolute byte differences per 64-bit lane. */
    {
        static const uint32_t x[8] __attribute__((aligned(32))) = {
            0x01010101, 0x01010101, 0, 0, 0, 0, 0, 0};
        static const uint32_t y[8] __attribute__((aligned(32))) = {
            0x03030303, 0x03030303, 0, 0, 0, 0, 0, 0};
        /* |1-3| = 2, eight bytes -> 16 in the first 64-bit lane */
        static const uint32_t want[8] = {16, 0, 0, 0, 0, 0, 0, 0};
        __asm__ volatile("vmovdqu (%1),%%ymm1\n\tvmovdqu (%2),%%ymm2\n\t"
                         "vpsadbw %%ymm2,%%ymm1,%%ymm3\n\tvmovdqu %%ymm3,(%0)\n\tvzeroupper"
                         : : "r"(out), "r"(x), "r"(y) : "memory", "ymm1", "ymm2", "ymm3");
        check8("vpsadbw", out, want);
    }
}

static void test_minmax_and_mul(void) {
    uint32_t out[8] __attribute__((aligned(32)));
    static const uint32_t x[8] __attribute__((aligned(32))) = {
        0x00000005, 0xfffffffb, 0x00000100, 0x7fffffff,
        0x00000005, 0xfffffffb, 0x00000100, 0x7fffffff};
    static const uint32_t y[8] __attribute__((aligned(32))) = {
        0x00000003, 0x00000003, 0x00000200, 0x80000000,
        0x00000003, 0x00000003, 0x00000200, 0x80000000};

    /* signed min: -5 < 3, and 0x7fffffff > 0x80000000 (= -2^31) */
    __asm__ volatile("vmovdqu (%1),%%ymm1\n\tvmovdqu (%2),%%ymm2\n\t"
                     "vpminsd %%ymm2,%%ymm1,%%ymm3\n\tvmovdqu %%ymm3,(%0)\n\tvzeroupper"
                     : : "r"(out), "r"(x), "r"(y) : "memory", "ymm1", "ymm2", "ymm3");
    {
        static const uint32_t want[8] = {
            3, 0xfffffffb, 0x100, 0x80000000,
            3, 0xfffffffb, 0x100, 0x80000000};
        check8("vpminsd.signed", out, want);
    }
    /* unsigned min on the same data picks differently -- catches a
       signed/unsigned mixup that a positive-only test would miss. */
    __asm__ volatile("vmovdqu (%1),%%ymm1\n\tvmovdqu (%2),%%ymm2\n\t"
                     "vpminud %%ymm2,%%ymm1,%%ymm3\n\tvmovdqu %%ymm3,(%0)\n\tvzeroupper"
                     : : "r"(out), "r"(x), "r"(y) : "memory", "ymm1", "ymm2", "ymm3");
    {
        static const uint32_t want[8] = {
            3, 3, 0x100, 0x7fffffff,
            3, 3, 0x100, 0x7fffffff};
        check8("vpminud.unsigned", out, want);
    }

    /* vpmuludq: low 32 bits of each 64-bit lane -> full 64-bit product. */
    {
        static const uint32_t a2[8] __attribute__((aligned(32))) = {
            0xffffffff, 0xdeadbeef, 2, 0, 0xffffffff, 0, 2, 0};
        static const uint32_t b2[8] __attribute__((aligned(32))) = {
            0xffffffff, 0xcafebabe, 3, 0, 0xffffffff, 0, 3, 0};
        /* 0xffffffff * 0xffffffff = 0xfffffffe00000001; upper dwords ignored */
        static const uint32_t want[8] = {
            0x00000001, 0xfffffffe, 6, 0,
            0x00000001, 0xfffffffe, 6, 0};
        __asm__ volatile("vmovdqu (%1),%%ymm1\n\tvmovdqu (%2),%%ymm2\n\t"
                         "vpmuludq %%ymm2,%%ymm1,%%ymm3\n\tvmovdqu %%ymm3,(%0)\n\tvzeroupper"
                         : : "r"(out), "r"(a2), "r"(b2) : "memory", "ymm1", "ymm2", "ymm3");
        check8("vpmuludq.ignores_high", out, want);
    }

    /* vpmulld keeps only the low 32 bits of each product. */
    {
        static const uint32_t a2[8] __attribute__((aligned(32))) = {
            0x00010001, 2, 3, 4, 0x00010001, 2, 3, 4};
        static const uint32_t b2[8] __attribute__((aligned(32))) = {
            0x00010001, 2, 3, 4, 0x00010001, 2, 3, 4};
        static const uint32_t want[8] = {
            0x00020001, 4, 9, 16, 0x00020001, 4, 9, 16};
        __asm__ volatile("vmovdqu (%1),%%ymm1\n\tvmovdqu (%2),%%ymm2\n\t"
                         "vpmulld %%ymm2,%%ymm1,%%ymm3\n\tvmovdqu %%ymm3,(%0)\n\tvzeroupper"
                         : : "r"(out), "r"(a2), "r"(b2) : "memory", "ymm1", "ymm2", "ymm3");
        check8("vpmulld.low32", out, want);
    }
}

static void test_blend_and_insert(void) {
    uint32_t out[8] __attribute__((aligned(32)));
    static const uint32_t x[8] __attribute__((aligned(32))) = {0, 1, 2, 3, 4, 5, 6, 7};
    static const uint32_t y[8] __attribute__((aligned(32))) = {
        0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7};

    /* vpblendd imm bit i selects src2 for dword i. */
    __asm__ volatile("vmovdqu (%1),%%ymm1\n\tvmovdqu (%2),%%ymm2\n\t"
                     "vpblendd $0xa5,%%ymm2,%%ymm1,%%ymm3\n\tvmovdqu %%ymm3,(%0)\n\tvzeroupper"
                     : : "r"(out), "r"(x), "r"(y) : "memory", "ymm1", "ymm2", "ymm3");
    {   /* 0xa5 = 1010 0101 -> dwords 0,2,5,7 from src2 */
        static const uint32_t want[8] = {0xa0, 1, 0xa2, 3, 4, 0xa5, 6, 0xa7};
        check8("vpblendd.imm", out, want);
    }

    /* vpblendvb selects per BYTE using the mask register's high bits. */
    {
        static const uint32_t mask[8] __attribute__((aligned(32))) = {
            0x80000080, 0, 0, 0, 0, 0, 0, 0};
        static const uint32_t want[8] = {0xa0, 1, 2, 3, 4, 5, 6, 7};
        /* bytes 0 and 3 of dword 0 come from src2; src2 dword0 is 0xa0 and
           src1 dword0 is 0 -> byte0 = 0xa0, byte3 = 0 either way */
        __asm__ volatile("vmovdqu (%1),%%ymm1\n\tvmovdqu (%2),%%ymm2\n\t"
                         "vmovdqu (%3),%%ymm4\n\t"
                         "vpblendvb %%ymm4,%%ymm2,%%ymm1,%%ymm3\n\t"
                         "vmovdqu %%ymm3,(%0)\n\tvzeroupper"
                         : : "r"(out), "r"(x), "r"(y), "r"(mask)
                         : "memory", "ymm1", "ymm2", "ymm3", "ymm4");
        check8("vpblendvb.bytemask", out, want);
    }

    /* vpinsrd / vpextrd round trip through lane 2. */
    {
        uint32_t got = 0;
        __asm__ volatile("vmovdqu (%1),%%xmm1\n\t"
                         "vpinsrd $2,%2,%%xmm1,%%xmm3\n\t"
                         "vpextrd $2,%%xmm3,%0"
                         : "=r"(got) : "r"(x), "r"(0xfeedfaceu) : "xmm1", "xmm3");
        if (got != 0xfeedfaceu) {
            printf("FAIL vpinsrd.roundtrip got=%08x want=feedface\n", got);
            failures_total++;
        } else {
            test_logf("PASS vpinsrd.roundtrip\n");
        }
    }
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
    test_saturating();
    test_shifts();
    test_pshufb();
    test_unpack_and_pack();
    test_broadcast_and_widen();
    test_cross_lane();
    test_palignr();
    test_pmovmskb_and_madd();
    test_minmax_and_mul();
    test_blend_and_insert();

    return finish_suite("avx_regress");
}

#endif /* __x86_64__ */
