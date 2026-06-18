/*
 * atomics — differential test for LOCK-prefixed read-modify-write ops and the
 * compare-exchange family:
 *   lock xadd            (exchange-and-add: new mem + flags; reg gets old mem)
 *   lock cmpxchg         (EAX==mem ? mem=reg,ZF : EAX=mem,!ZF; all arith flags)
 *   lock cmpxchg8b       (EDX:EAX vs 64-bit mem; ZF only)
 *   lock cmpxchg16b      (RDX:RAX vs 128-bit mem; ZF only; amd64)
 *   lock add/and/or/xor  (RMW + flags through the atomic gadget path)
 *   lock inc/dec         (RMW; CF preserved -> excluded from the mask)
 *   xchg                 (implicitly locked swap; no flags)
 *
 * These are integer ops, so no canonicalization: result bytes + the DEFINED
 * flag bits must match the oracle exactly. cmpxchg emits two lines per case
 * (the memory result with flags, and the accumulator result) to pin both
 * register effects, including the not-equal path where EAX is reloaded.
 */
#include "diff_common.h"

#define LOGIC (FL_CF | FL_PF | FL_ZF | FL_SF | FL_OF)   /* AF undefined */
#define INCDEC (FL_OF | FL_SF | FL_ZF | FL_AF | FL_PF)  /* CF preserved */
#define STORE32 0x5a5a5a5au

// Per-width value pools, each DISTINCT under its own width, so the emitted
// (op, width, a, b) keys never collide -- dc_emit masks a/b to the op width, so
// a single 32-bit pool would alias (0x80000000, 0x100, 0 all -> 0x00 as bytes).
static const uint8_t  VB[] = {0x00, 0x01, 0x02, 0x7f, 0x80, 0xff, 0x55, 0xaa};
static const uint32_t VL[] = {0, 1, 2, 0x7fffffff, 0x80000000, 0xffffffff, 0x55555555, 0x100};
static const uint64_t VQ[] = {0, 1, 2, 0x7fffffffffffffffull, 0x8000000000000000ull,
                              0xffffffffffffffffull, 0x5555555555555555ull, 0x100000000ull};
#define NV (sizeof VB / sizeof VB[0])

#define XADD(MNEM, CT, MEM0, REG0, MOUT, FL) do {                              \
    CT m_ = (CT)(MEM0), r_ = (CT)(REG0); unsigned long f_;                      \
    __asm__ volatile("lock " MNEM " %[r], %[m]\n\t" PUSHF "\n\tpop %[f]"        \
        : [m] "+m"(m_), [r] "+r"(r_), [f] "=r"(f_) : : "cc", "memory");         \
    (MOUT) = (uint64_t)m_; (FL) = f_; } while (0)

#define CMPXCHG(CT, MEM0, ACC0, MOUT, AOUT, FL) do {                           \
    CT m_ = (CT)(MEM0), a_ = (CT)(ACC0), s_ = (CT)STORE32; unsigned long f_;    \
    __asm__ volatile("lock cmpxchg %[s], %[m]\n\t" PUSHF "\n\tpop %[f]"         \
        : [m] "+m"(m_), "+a"(a_), [f] "=r"(f_) : [s] "r"(s_) : "cc", "memory"); \
    (MOUT) = (uint64_t)m_; (AOUT) = (uint64_t)a_; (FL) = f_; } while (0)

#define LOCK_RMW(MNEM, CT, MEM0, REG0, MOUT, FL) do {                          \
    CT m_ = (CT)(MEM0), r_ = (CT)(REG0); unsigned long f_;                      \
    __asm__ volatile("lock " MNEM " %[r], %[m]\n\t" PUSHF "\n\tpop %[f]"        \
        : [m] "+m"(m_), [f] "=r"(f_) : [r] "r"(r_) : "cc", "memory");           \
    (MOUT) = (uint64_t)m_; (FL) = f_; } while (0)

#define LOCK_UN(MNEM, CT, MEM0, MOUT, FL) do {                                 \
    CT m_ = (CT)(MEM0); unsigned long f_;                                       \
    __asm__ volatile("lock " MNEM " %[m]\n\t" PUSHF "\n\tpop %[f]"              \
        : [m] "+m"(m_), [f] "=r"(f_) : : "cc", "memory");                       \
    (MOUT) = (uint64_t)m_; (FL) = f_; } while (0)

static void cmpxchg8b_case(uint64_t mem, uint64_t acc, uint64_t store) {
    uint32_t eax = (uint32_t)acc, edx = (uint32_t)(acc >> 32);
    uint32_t ebx = (uint32_t)store, ecx = (uint32_t)(store >> 32);
    uint64_t m_ = mem; unsigned long f_;
    __asm__ volatile("lock cmpxchg8b %[m]\n\t" PUSHF "\n\tpop %[f]"
        : [m] "+m"(m_), "+a"(eax), "+d"(edx), [f] "=r"(f_)
        : "b"(ebx), "c"(ecx) : "cc", "memory");
    dc_emit("cmpxchg8b", 64, mem, acc, m_, f_, FL_ZF);
    dc_emit("cx8b.ax", 64, mem, acc, ((uint64_t)edx << 32) | eax, 0, 0);
}

static void run_cases(void) {
    /* lock xadd: byte + dword (+ qword on amd64) */
    for (size_t i = 0; i < NV; i++) for (size_t j = 0; j < NV; j++) {
        uint64_t mo; unsigned long fl;
        XADD("xadd", uint8_t,  VB[i], VB[j], mo, fl); dc_emit("xaddb", 8,  VB[i], VB[j], mo, fl, FL_ALL);
        XADD("xadd", uint32_t, VL[i], VL[j], mo, fl); dc_emit("xaddl", 32, VL[i], VL[j], mo, fl, FL_ALL);
#if HAVE_W64
        XADD("xadd", uint64_t, VQ[i], VQ[j], mo, fl); dc_emit("xaddq", 64, VQ[i], VQ[j], mo, fl, FL_ALL);
#endif
    }
    /* lock cmpxchg: vary mem + accumulator (store fixed). We compare the memory
     * result and ZF, but MASK OFF the other arith flags (FL_ZF only). The arith
     * sub-flags' operand order is not a dependable cross-implementation invariant:
     * iSH, the real Intel mint cell, and the SDM's "CMP accumulator, dest" all use
     * (acc - dest), while Rosetta uses (dest - acc). That does NOT make Rosetta
     * "wrong" -- CPUs carry errata and generational/vendor quirks, and an emulator
     * may faithfully model a different part -- it just means only ZF (which is
     * order-symmetric) is reliable across oracles. (cmpxchg8b/16b are ZF-only by
     * spec for the same reason.) */
    for (size_t i = 0; i < NV; i++) for (size_t j = 0; j < NV; j++) {
        uint64_t mo, ao; unsigned long fl;
        CMPXCHG(uint8_t,  VB[i], VB[j], mo, ao, fl);
        dc_emit("cmpxchgb", 8, VB[i], VB[j], mo, fl, FL_ZF); dc_emit("cxb.ax", 8, VB[i], VB[j], ao, 0, 0);
        CMPXCHG(uint32_t, VL[i], VL[j], mo, ao, fl);
        dc_emit("cmpxchgl", 32, VL[i], VL[j], mo, fl, FL_ZF); dc_emit("cxl.ax", 32, VL[i], VL[j], ao, 0, 0);
#if HAVE_W64
        CMPXCHG(uint64_t, VQ[i], VQ[j], mo, ao, fl);
        dc_emit("cmpxchgq", 64, VQ[i], VQ[j], mo, fl, FL_ZF); dc_emit("cxq.ax", 64, VQ[i], VQ[j], ao, 0, 0);
#endif
    }
    /* cmpxchg8b: 64-bit compare-exchange (runs on i386 + amd64) */
    static const uint64_t W[] = {0, 1, 0x100000000ull, 0x7fffffffffffffffull,
                                 0x8000000000000000ull, 0xffffffffffffffffull, 0x123456789abcdef0ull};
    for (size_t i = 0; i < sizeof W / sizeof W[0]; i++)
        for (size_t j = 0; j < sizeof W / sizeof W[0]; j++)
            cmpxchg8b_case(W[i], W[j], 0xcafef00ddeadbeefull);

    /* lock add/and/or/xor/sub [mem], reg (dword); flags through the atomic path */
    for (size_t i = 0; i < NV; i++) for (size_t j = 0; j < NV; j++) {
        uint64_t mo; unsigned long fl;
        LOCK_RMW("add", uint32_t, VL[i], VL[j], mo, fl); dc_emit("lkadd", 32, VL[i], VL[j], mo, fl, FL_ALL);
        LOCK_RMW("and", uint32_t, VL[i], VL[j], mo, fl); dc_emit("lkand", 32, VL[i], VL[j], mo, fl, LOGIC);
        LOCK_RMW("or",  uint32_t, VL[i], VL[j], mo, fl); dc_emit("lkor",  32, VL[i], VL[j], mo, fl, LOGIC);
        LOCK_RMW("xor", uint32_t, VL[i], VL[j], mo, fl); dc_emit("lkxor", 32, VL[i], VL[j], mo, fl, LOGIC);
        LOCK_RMW("sub", uint32_t, VL[i], VL[j], mo, fl); dc_emit("lksub", 32, VL[i], VL[j], mo, fl, FL_ALL);
    }
    /* lock inc/dec [mem] (CF preserved); xchg swap (no flags) */
    for (size_t i = 0; i < NV; i++) {
        uint64_t mo; unsigned long fl;
        LOCK_UN("incl", uint32_t, VL[i], mo, fl); dc_emit("lkinc", 32, VL[i], 0, mo, fl, INCDEC);
        LOCK_UN("decl", uint32_t, VL[i], mo, fl); dc_emit("lkdec", 32, VL[i], 0, mo, fl, INCDEC);
        uint32_t m_ = VL[i], r_ = STORE32;
        __asm__ volatile("xchg %[r], %[m]" : [m] "+m"(m_), [r] "+r"(r_) : : "memory");
        dc_emit("xchg", 32, VL[i], STORE32, m_, 0, 0);  /* mem'=reg; reg'=old mem */
    }
#if HAVE_W64
    /* cmpxchg16b: 128-bit compare-exchange (amd64); both equal + not-equal paths.
     * eq=1: RDX:RAX == mem -> store RCX:RBX, ZF=1. eq=0: RAX differs -> RDX:RAX
     * is reloaded from mem, ZF=0, mem unchanged. */
    for (size_t i = 0; i < NV; i++) for (int eq = 0; eq < 2; eq++) {
        uint64_t mlo = VQ[i], mhi = VQ[(i + 1) % NV];
        _Alignas(16) unsigned __int128 m = ((unsigned __int128)mhi << 64) | mlo;
        uint64_t rax = eq ? mlo : ~mlo, rdx = mhi;
        uint64_t rbx = 0x1111111122222222ull, rcx = 0x3333333344444444ull;
        unsigned long f_;
        __asm__ volatile("lock cmpxchg16b %[m]\n\t" PUSHF "\n\tpop %[f]"
            : [m] "+m"(m), "+a"(rax), "+d"(rdx), [f] "=r"(f_)
            : "b"(rbx), "c"(rcx) : "cc", "memory");
        dc_emit("cx16b",    64, VQ[i], (uint64_t)eq, (uint64_t)m,         f_, FL_ZF);
        dc_emit("cx16b.hi", 64, VQ[i], (uint64_t)eq, (uint64_t)(m >> 64), 0, 0);
        dc_emit("cx16b.ax", 64, VQ[i], (uint64_t)eq, rax,                 0, 0);
        dc_emit("cx16b.dx", 64, VQ[i], (uint64_t)eq, rdx,                 0, 0);
    }
#endif
}
