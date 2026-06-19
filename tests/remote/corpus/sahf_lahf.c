/*
 * sahf_lahf — differential test for SAHF (0x9e) and LAHF (0x9f).
 *
 * LAHF loads AH from SF:ZF:0:AF:0:PF:1:CF (the low byte of EFLAGS). SAHF stores
 * SF/ZF/AF/PF/CF from AH back into EFLAGS, leaving OF and the rest untouched.
 * Neither was implemented in the amd64 engine (interp lacked case 0x9e/0x9f; the
 * JIT had no gadget) — they are added natively by this campaign. Validated
 * interp==jit==oracle (Rosetta + real-Intel mint).
 *
 * LAHF: drive the flags with a byte ALU op, then lahf; the captured AH is the
 * value (it re-encodes the flag state, so a mismatch means a flag was wrong).
 * SAHF: load a known AH, sahf, then pushf; compare only the five SAHF-defined
 * bits (CF/PF/AF/ZF/SF) — OF is preserved from before, so it's masked out.
 *
 * Each case is warmed so its JIT block actually compiles (cold runs interp).
 */
#include "diff_common.h"

#define WARM 64
#define SAHF_DEF (FL_CF | FL_PF | FL_AF | FL_ZF | FL_SF)   /* OF preserved, masked */

static const uint64_t APAT[] = {
    0x00, 0x01, 0x02, 0x7f, 0x80, 0x81, 0xfe, 0xff, 0x55, 0xaa, 0x3c, 0x10, 0x0f
};
#define NAP (sizeof APAT / sizeof APAT[0])

/* op %cl, %bl (byte) to set flags, then lahf; AH is the captured value.
 * bl(+b) holds A and is the op destination; cl(c) holds B; eax(=a) returns AH. */
#define LAHF2(MNEM, NAME, A, B) do {                                         \
    unsigned int bb_ = (unsigned char)(A); unsigned long ah_ = 0;            \
    for (int w_ = 0; w_ < WARM; w_++) {                                      \
        bb_ = (unsigned char)(A);                                            \
        __asm__ volatile(MNEM " %%cl, %%bl\n\tlahf\n\tmovzbl %%ah, %%eax"     \
            : "=a"(ah_), "+b"(bb_) : "c"((unsigned int)(unsigned char)(B))   \
            : "cc"); }                                                       \
    dc_emit(NAME, 8, (A) & 0xff, (B) & 0xff, ah_ & 0xff, 0, 0);              \
} while (0)

/* load AH = V, sahf, capture resulting flags (compare the 5 SAHF-defined bits).
 * AH is loaded via a 32-bit reg-reg mov (ebx=V<<8 -> eax, so AH=V) rather than
 * `movb ...,%ah`: a mov to a legacy high byte falls the JIT block back to interp,
 * which would leave the native sahf gadget unexercised. The 32-bit mov is native,
 * so the block compiles and the sahf gadget actually runs. */
#define SAHF1(NAME, V) do {                                                  \
    unsigned long f_ = 0; unsigned int in_ = ((unsigned int)((V) & 0xff)) << 8;\
    for (int w_ = 0; w_ < WARM; w_++) {                                      \
        __asm__ volatile("mov %%ebx, %%eax\n\tsahf\n\t" PUSHF "\n\tpop %[f]"  \
            : [f] "=r"(f_) : "b"(in_) : "ax", "cc"); }                       \
    dc_emit(NAME, 8, (V) & 0xff, 0, f_ & SAHF_DEF, f_, SAHF_DEF);            \
} while (0)

/* SAHF then LAHF round-trip: load AH=V, sahf, lahf, read AH back. The five
 * defined bits must survive the round-trip exactly (bits 1/3/5 are forced to
 * 1/0/0 by LAHF, so the recovered AH = (V & 0xd5) | 0x02). Same native AH load. */
#define ROUND1(NAME, V) do {                                                 \
    unsigned long ah_ = 0; unsigned int in_ = ((unsigned int)((V) & 0xff)) << 8;\
    for (int w_ = 0; w_ < WARM; w_++) {                                      \
        __asm__ volatile("mov %%ebx, %%eax\n\tsahf\n\tlahf\n\tmovzbl %%ah, %%eax" \
            : "=a"(ah_) : "b"(in_) : "cc"); }                                \
    dc_emit(NAME, 8, (V) & 0xff, 0, ah_ & 0xff, 0, 0);                       \
} while (0)

static void run_cases(void) {
    /* LAHF: every flag combination reachable from byte add/sub/and/or/xor/cmp */
    for (size_t i = 0; i < NAP; i++)
        for (size_t j = 0; j < NAP; j++) {
            uint64_t a = APAT[i], b = APAT[j];
            LAHF2("add", "ladd", a, b);
            LAHF2("sub", "lsub", a, b);
            LAHF2("and", "land", a, b);
            LAHF2("or",  "lor",  a, b);
            LAHF2("xor", "lxor", a, b);
            LAHF2("cmp", "lcmp", a, b);
        }
    /* SAHF: sweep every AH value, plus the SAHF/LAHF round-trip */
    for (unsigned v = 0; v < 256; v++) {
        SAHF1("sahf", v);
        ROUND1("rnd", v);
    }
}
