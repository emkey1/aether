/*
 * sse_cvt — differential test for SSE/SSE2 float math + conversions.
 *
 * THE CATCH: iSH runs arm64 NEON gadgets; the oracle is real x86 SSE. For
 * normal finite IEEE-754 ops (add/sub/mul/div/sqrt, round-to-nearest-even) the
 * two are bit-identical, and that is the bulk of what we validate. But they
 * legitimately DIVERGE on:
 *   - NaN sign/payload (x86 sqrt(-1) = 0xffc00000, arm64 = 0x7fc00000; NaN
 *     propagation differs) -> we canonicalize ANY NaN result to one payload.
 *   - out-of-range / NaN float->int (x86 -> integer-indefinite 0x80000000,
 *     arm64 -> saturates) -> we detect the unsafe input and emit a fixed marker
 *     instead of the divergent integer.
 *   - rsqrt/rcp approximations -> excluded entirely.
 * So this compares the architecturally-AGREED behavior bit-exact, the same way
 * the integer tests mask undefined EFLAGS. A divergence here is a real bug.
 */
#include "diff_common.h"

#define MARK_I32 0xdeadbeefu   /* emitted for unsafe (out-of-range/NaN) f->int */

static uint32_t f2b(float f)  { uint32_t u; memcpy(&u, &f, 4); return u; }
static uint64_t d2b(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }
static float    b2f(uint32_t u){ float f;  memcpy(&f, &u, 4); return f; }
static double   b2d(uint64_t u){ double d; memcpy(&d, &u, 8); return d; }

/* any NaN -> a single canonical payload so arm64/x86 NaN differences don't
 * false-diverge (both agree the result IS a NaN; only the bits differ). */
static uint32_t canon32(uint32_t b) {
    if ((b & 0x7f800000u) == 0x7f800000u && (b & 0x007fffffu)) return 0x7fc00000u;
    return b;
}
static uint64_t canon64(uint64_t b) {
    if ((b & 0x7ff0000000000000ull) == 0x7ff0000000000000ull &&
        (b & 0x000fffffffffffffull)) return 0x7ff8000000000000ull;
    return b;
}
/* in [-2^31, 2^31) and finite -> x86 cvtt and arm64 agree; else they diverge */
static int safe_f32_i32(float f)  { return f >= -2147483648.0f && f < 2147483648.0f; }
static int safe_f64_i32(double d) { return d >= -2147483648.0  && d < 2147483648.0; }

static const uint32_t F32[] = {
    0x00000000, 0x80000000, 0x3f800000, 0xbf800000, 0x3f000000, 0x40490fdb,
    0x4f000000, 0x7f800000, 0xff800000, 0x7fc00000, 0x00000001, 0x4b800001,
};
#define NF32 (sizeof F32 / sizeof F32[0])
static const uint64_t F64[] = {
    0x0000000000000000ull, 0x8000000000000000ull, 0x3ff0000000000000ull,
    0xbff0000000000000ull, 0x3fe0000000000000ull, 0x400921fb54442d18ull,
    0x41e0000000000000ull, 0x7ff0000000000000ull, 0xfff0000000000000ull,
    0x7ff8000000000000ull, 0x0000000000000001ull, 0x4330000000000001ull,
};
#define NF64 (sizeof F64 / sizeof F64[0])
static const int32_t I32[] = {0, 1, -1, 1000, -1000, 0x7fffffff, (int)0x80000000, 0x01000001};
#define NI32 (sizeof I32 / sizeof I32[0])

/* scalar binary: MNEM b,a ; a is in/out (xmm), b in (xmm) */
#define SS_BIN(MNEM, AF, BF, OUT) do { float a_=(AF),b_=(BF); \
    __asm__ volatile(MNEM " %[b], %[a]" : [a]"+x"(a_) : [b]"x"(b_)); (OUT)=a_; } while(0)
#define SD_BIN(MNEM, AD, BD, OUT) do { double a_=(AD),b_=(BD); \
    __asm__ volatile(MNEM " %[b], %[a]" : [a]"+x"(a_) : [b]"x"(b_)); (OUT)=a_; } while(0)

#define EMIT_SS_BIN(MNEM) do { for (size_t i=0;i<NF32;i++) for (size_t j=0;j<NF32;j++) { \
    float r; SS_BIN(MNEM, b2f(F32[i]), b2f(F32[j]), r); \
    dc_emit(MNEM, 32, F32[i], F32[j], canon32(f2b(r)), 0, 0); } } while(0)
#define EMIT_SD_BIN(MNEM) do { for (size_t i=0;i<NF64;i++) for (size_t j=0;j<NF64;j++) { \
    double r; SD_BIN(MNEM, b2d(F64[i]), b2d(F64[j]), r); \
    dc_emit(MNEM, 64, F64[i], F64[j], canon64(d2b(r)), 0, 0); } } while(0)

__attribute__((target("sse2")))
static void run_cases(void) {
    /* scalar f32 / f64 arithmetic (NaN-canonicalized) */
    EMIT_SS_BIN("addss"); EMIT_SS_BIN("subss"); EMIT_SS_BIN("mulss");
    EMIT_SS_BIN("divss"); EMIT_SS_BIN("minss"); EMIT_SS_BIN("maxss");
    EMIT_SD_BIN("addsd"); EMIT_SD_BIN("subsd"); EMIT_SD_BIN("mulsd");
    EMIT_SD_BIN("divsd"); EMIT_SD_BIN("minsd"); EMIT_SD_BIN("maxsd");

    for (size_t i = 0; i < NF32; i++) {              /* sqrtss */
        float a = b2f(F32[i]), r;
        __asm__ volatile("sqrtss %[a], %[r]" : [r]"=x"(r) : [a]"x"(a));
        dc_emit("sqrtss", 32, F32[i], 0, canon32(f2b(r)), 0, 0);
    }
    for (size_t i = 0; i < NF64; i++) {              /* sqrtsd */
        double a = b2d(F64[i]), r;
        __asm__ volatile("sqrtsd %[a], %[r]" : [r]"=x"(r) : [a]"x"(a));
        dc_emit("sqrtsd", 64, F64[i], 0, canon64(d2b(r)), 0, 0);
    }

    for (size_t i = 0; i < NI32; i++) {              /* cvtsi2ss: int -> f32 */
        float r; int32_t v = I32[i];
        __asm__ volatile("cvtsi2ss %[v], %[r]" : [r]"=x"(r) : [v]"r"(v));
        dc_emit("si2ss", 32, (uint32_t)v, 0, f2b(r), 0, 0);
    }
    for (size_t i = 0; i < NI32; i++) {              /* cvtsi2sd: int -> f64 */
        double r; int32_t v = I32[i];
        __asm__ volatile("cvtsi2sd %[v], %[r]" : [r]"=x"(r) : [v]"r"(v));
        dc_emit("si2sd", 64, (uint32_t)v, 0, d2b(r), 0, 0);
    }
    for (size_t i = 0; i < NF32; i++) {              /* cvttss2si: f32 -> int (trunc) */
        float a = b2f(F32[i]); int32_t r;
        __asm__ volatile("cvttss2si %[a], %[r]" : [r]"=r"(r) : [a]"x"(a));
        uint32_t v = safe_f32_i32(a) ? (uint32_t)r : MARK_I32;
        dc_emit("ttss2si", 32, F32[i], 0, v, 0, 0);
    }
    for (size_t i = 0; i < NF64; i++) {              /* cvttsd2si: f64 -> int (trunc) */
        double a = b2d(F64[i]); int32_t r;
        __asm__ volatile("cvttsd2si %[a], %[r]" : [r]"=r"(r) : [a]"x"(a));
        uint32_t v = safe_f64_i32(a) ? (uint32_t)r : MARK_I32;
        dc_emit("ttsd2si", 64, F64[i], 0, v, 0, 0);
    }
    for (size_t i = 0; i < NF32; i++) {              /* cvtss2sd: f32 -> f64 (widen) */
        float a = b2f(F32[i]); double r;
        __asm__ volatile("cvtss2sd %[a], %[r]" : [r]"=x"(r) : [a]"x"(a));
        dc_emit("ss2sd", 64, F32[i], 0, canon64(d2b(r)), 0, 0);
    }
    for (size_t i = 0; i < NF64; i++) {              /* cvtsd2ss: f64 -> f32 (narrow) */
        double a = b2d(F64[i]); float r;
        __asm__ volatile("cvtsd2ss %[a], %[r]" : [r]"=x"(r) : [a]"x"(a));
        dc_emit("sd2ss", 64, F64[i], 0, canon32(f2b(r)), 0, 0);
    }

    /* packed: exercise the 128-bit gadget path. Build 4-lane / 2-lane vectors
     * from the scalar pools, run the op, emit each lane (canonicalized). */
    typedef float v4sf __attribute__((vector_size(16)));
    typedef int   v4si __attribute__((vector_size(16)));
    static const size_t Q[][4] = {{0,2,3,4},{5,6,7,8},{9,10,11,0},{2,4,7,11},{0,1,0,1}};
    for (size_t q = 0; q < sizeof Q / sizeof Q[0]; q++) {
        v4sf a, b; uint32_t la[4], lb[4];
        for (int k=0;k<4;k++){ la[k]=F32[Q[q][k]]; lb[k]=F32[Q[q][(k+1)&3]]; }
        memcpy(&a, la, 16); memcpy(&b, lb, 16);
        v4sf r; uint32_t out[4];

        r=a; __asm__ volatile("addps %[b],%[r]":[r]"+x"(r):[b]"x"(b)); memcpy(out,&r,16);
        for(int k=0;k<4;k++) dc_emit("addps", 32, la[k], lb[k], canon32(out[k]), 0, 0);
        r=a; __asm__ volatile("mulps %[b],%[r]":[r]"+x"(r):[b]"x"(b)); memcpy(out,&r,16);
        for(int k=0;k<4;k++) dc_emit("mulps", 32, la[k], lb[k], canon32(out[k]), 0, 0);
        r=a; __asm__ volatile("minps %[b],%[r]":[r]"+x"(r):[b]"x"(b)); memcpy(out,&r,16);
        for(int k=0;k<4;k++) dc_emit("minps", 32, la[k], lb[k], canon32(out[k]), 0, 0);
        r=a; __asm__ volatile("maxps %[b],%[r]":[r]"+x"(r):[b]"x"(b)); memcpy(out,&r,16);
        for(int k=0;k<4;k++) dc_emit("maxps", 32, la[k], lb[k], canon32(out[k]), 0, 0);
        __asm__ volatile("sqrtps %[a],%[r]":[r]"=x"(r):[a]"x"(a)); memcpy(out,&r,16);
        for(int k=0;k<4;k++) dc_emit("sqrtps", 32, la[k], 0, canon32(out[k]), 0, 0);

        v4si ri; __asm__ volatile("cvttps2dq %[a],%[r]":[r]"=x"(ri):[a]"x"(a)); memcpy(out,&ri,16);
        for(int k=0;k<4;k++){ uint32_t v = safe_f32_i32(b2f(la[k])) ? out[k] : MARK_I32;
            dc_emit("ttps2dq", 32, la[k], 0, v, 0, 0); }

        v4sf rf; v4si ii; for(int k=0;k<4;k++) la[k]=(uint32_t)I32[(q+k)%NI32];
        memcpy(&ii, la, 16);
        __asm__ volatile("cvtdq2ps %[a],%[r]":[r]"=x"(rf):[a]"x"(ii)); memcpy(out,&rf,16);
        for(int k=0;k<4;k++) dc_emit("dq2ps", 32, la[k], 0, out[k], 0, 0);
    }
    (void)b2d; (void)canon64;
}
