/*
 * sse_mxcsr — ldmxcsr/stmxcsr round-trip (0F AE /2, /3), the instructions Free
 * Pascal's FPU init uses (it crashed amd64 with "illegal instruction" before the
 * MXCSR store/load was implemented). The emulator doesn't honor MXCSR's rounding/
 * exception bits, but the control word must store and load. We compare the
 * round-tripped value (not the initial default, which is environment-specific):
 * ldmxcsr V; stmxcsr -> must read back V for every defined bit pattern.
 */
#include "diff_common.h"

/* amd64 only: iSH's i386 decoder (emu/decode.h) currently treats all of 0F AE
 * as a no-op "fence", so i386 stmxcsr never stores -- a separate, noted gap.
 * pascal runs as amd64, which is what this guards. */
#if HAVE_W64
static uint32_t mxcsr_roundtrip(uint32_t v) {
    uint32_t out = 0;
    __asm__ volatile("ldmxcsr %1\n\tstmxcsr %0" : "=m"(out) : "m"(v) : );
    return out;
}

/* Values exercise RC (13-14), exception masks (7-12), flags (0-5), DAZ (6),
 * FTZ (15); all keep the masks set so no later FP op can fault. */
static const uint32_t VALS[] = {
    0x1f80, 0x3f80, 0x5f80, 0x7f80, 0x9f80, 0x1fa0, 0x1fc0, 0x1fbf, 0xff80,
};
#define NV (sizeof VALS / sizeof VALS[0])
#endif

static void run_cases(void) {
#if HAVE_W64
    for (size_t i = 0; i < NV; i++) {
        uint32_t got = mxcsr_roundtrip(VALS[i]);
        dc_emit("ldst", 32, VALS[i], 0, got, 0, 0);
    }
    uint32_t def = 0x1f80; /* leave MXCSR at the default */
    __asm__ volatile("ldmxcsr %0" : : "m"(def));
#endif
}
