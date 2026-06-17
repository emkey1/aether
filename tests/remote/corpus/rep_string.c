/*
 * rep_string — differential test for the rep-prefixed string ops:
 *   rep movs{b,w,l,q}   (copy)        — no flags; verify the dest buffer
 *   rep stos{b,w,l,q}   (fill)        — no flags; verify the dest buffer
 *   repe cmps{b,w,l,q}  (compare)     — sets all arith flags; verify ECX + flags
 *   repne scas{b,w,l,q} (scan)        — sets all arith flags; verify ECX + flags
 *
 * Edges this pushes (the page-batched rep fast path has bitten this project):
 *   - DF = 0 (forward) and DF = 1 (backward)
 *   - cross-page spans (buffer straddles a 4 KiB boundary -> the batch path)
 *   - ECX = 0 (must be a clean no-op)
 *   - 16-bit element (movsw/stosw -- a prior `f3 66 a5` decoder SIGILL)
 *   - repe/repne early termination (stop position + last-compare flags)
 *
 * movs/stos write memory, so the result is the destination buffer: we checksum
 * the whole (pre-zeroed/pre-filled) buffer and emit that. The C checksum is
 * identical on every cell, so a wrong byte anywhere diverges. cmps/scas leave
 * the meaningful result in ECX + EFLAGS.
 */
#include "diff_common.h"

static _Alignas(4096) uint8_t SBUF[8192];
static _Alignas(4096) uint8_t DBUF[8192];

static const int OFFS[] = {64, 4090};          /* page-aligned-ish, cross-page */
static const unsigned CNTS[] = {0, 1, 7, 64, 300};

static uint32_t cksum(const uint8_t *p, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}
static void fill_pattern(uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) p[i] = (uint8_t)(i * 7u + (i >> 3) + 3u);
}

/* No asm labels: a C `if` picks the std/cld variant, so the macro can expand
 * many times in one function without duplicate-label errors. cld restores DF. */
#define REP_MOVS(SUF, SP, DP, CNT, DFV, OC) do {                            \
    void *s_ = (SP), *d_ = (DP); unsigned long c_ = (unsigned long)(CNT);    \
    if (DFV) __asm__ volatile("std\n\trep movs" SUF "\n\tcld"                \
        : "+S"(s_), "+D"(d_), "+c"(c_) : : "memory", "cc");                  \
    else     __asm__ volatile("cld\n\trep movs" SUF "\n\tcld"                \
        : "+S"(s_), "+D"(d_), "+c"(c_) : : "memory", "cc");                  \
    (OC) = c_;                                                               \
} while (0)

#define REP_STOS(SUF, CT, VAL, DP, CNT, DFV, OC) do {                       \
    void *d_ = (DP); unsigned long c_ = (unsigned long)(CNT); CT v_ = (CT)(VAL); \
    if (DFV) __asm__ volatile("std\n\trep stos" SUF "\n\tcld"                \
        : "+D"(d_), "+c"(c_) : "a"(v_) : "memory", "cc");                    \
    else     __asm__ volatile("cld\n\trep stos" SUF "\n\tcld"                \
        : "+D"(d_), "+c"(c_) : "a"(v_) : "memory", "cc");                    \
    (OC) = c_;                                                               \
} while (0)

#define REPE_CMPS(SUF, SP, DP, CNT, OC, OF) do {                            \
    void *s_ = (SP), *d_ = (DP); unsigned long c_ = (unsigned long)(CNT), f_; \
    __asm__ volatile("cld\n\trepe cmps" SUF "\n\t" PUSHF "\n\tpop %[f]"      \
        : "+S"(s_), "+D"(d_), "+c"(c_), [f] "=r"(f_) : : "memory", "cc");    \
    (OC) = c_; (OF) = f_;                                                    \
} while (0)

#define REPNE_SCAS(SUF, CT, VAL, DP, CNT, OC, OF) do {                      \
    void *d_ = (DP); unsigned long c_ = (unsigned long)(CNT), f_; CT v_ = (CT)(VAL); \
    __asm__ volatile("cld\n\trepne scas" SUF "\n\t" PUSHF "\n\tpop %[f]"     \
        : "+D"(d_), "+c"(c_), [f] "=r"(f_) : "a"(v_) : "memory", "cc");      \
    (OC) = c_; (OF) = f_;                                                    \
} while (0)

#define MOVS_CASES(SUF, SZ, OP) do {                                        \
    for (size_t oi = 0; oi < sizeof OFFS / sizeof OFFS[0]; oi++)             \
    for (int df = 0; df < 2; df++)                                          \
    for (size_t ci = 0; ci < sizeof CNTS / sizeof CNTS[0]; ci++) {          \
        int off = OFFS[oi]; unsigned cnt = CNTS[ci];                        \
        size_t len = (size_t)cnt * (SZ);                                    \
        fill_pattern(SBUF, sizeof SBUF); memset(DBUF, 0, sizeof DBUF);      \
        void *sp, *dp;                                                      \
        if (df && cnt) { sp = SBUF + off + len - (SZ); dp = DBUF + off + len - (SZ); } \
        else           { sp = SBUF + off;              dp = DBUF + off; }   \
        unsigned long oc; REP_MOVS(SUF, sp, dp, cnt, df, oc); (void)oc;     \
        dc_emit(OP, 32, ((uint64_t)df << 16) | (unsigned)off, cnt,          \
                cksum(DBUF, sizeof DBUF), 0, 0);                            \
    }                                                                      \
} while (0)

#define STOS_CASES(SUF, CT, SZ, VAL, OP) do {                               \
    for (size_t oi = 0; oi < sizeof OFFS / sizeof OFFS[0]; oi++)             \
    for (int df = 0; df < 2; df++)                                          \
    for (size_t ci = 0; ci < sizeof CNTS / sizeof CNTS[0]; ci++) {          \
        int off = OFFS[oi]; unsigned cnt = CNTS[ci];                        \
        size_t len = (size_t)cnt * (SZ);                                    \
        memset(DBUF, 0, sizeof DBUF);                                       \
        void *dp = (df && cnt) ? (void *)(DBUF + off + len - (SZ)) : (void *)(DBUF + off); \
        unsigned long oc; REP_STOS(SUF, CT, VAL, dp, cnt, df, oc); (void)oc; \
        dc_emit(OP, 32, ((uint64_t)df << 16) | (unsigned)off, cnt,          \
                cksum(DBUF, sizeof DBUF), 0, 0);                            \
    }                                                                      \
} while (0)

#define CMPS_CASES(SUF, SZ, OP) do {                                        \
    static const unsigned CC[] = {16, 300};                                \
    for (size_t oi = 0; oi < sizeof OFFS / sizeof OFFS[0]; oi++)             \
    for (int mode = 0; mode < 2; mode++)                                   \
    for (size_t ci = 0; ci < sizeof CC / sizeof CC[0]; ci++) {              \
        int off = OFFS[oi]; unsigned cnt = CC[ci];                         \
        fill_pattern(SBUF, sizeof SBUF); memcpy(DBUF, SBUF, sizeof DBUF);   \
        if (mode) DBUF[off + (cnt / 2) * (SZ)] ^= 0xff;  /* mismatch midway */ \
        unsigned long oc, of;                                              \
        REPE_CMPS(SUF, SBUF + off, DBUF + off, cnt, oc, of);               \
        dc_emit(OP, 32, ((uint64_t)mode << 16) | (unsigned)off, cnt, oc, of, FL_ALL); \
    }                                                                      \
} while (0)

#define SCAS_CASES(SUF, CT, SZ, NEEDLE, OP) do {                            \
    static const unsigned CC[] = {16, 300};                                \
    for (size_t oi = 0; oi < sizeof OFFS / sizeof OFFS[0]; oi++)             \
    for (int mode = 0; mode < 2; mode++)                                   \
    for (size_t ci = 0; ci < sizeof CC / sizeof CC[0]; ci++) {              \
        int off = OFFS[oi]; unsigned cnt = CC[ci];                         \
        memset(DBUF, 0x11, sizeof DBUF);  /* 0x11.. never equals NEEDLE */  \
        if (mode) *(CT *)(void *)(DBUF + off + (cnt / 2) * (SZ)) = (CT)(NEEDLE); \
        unsigned long oc, of;                                              \
        REPNE_SCAS(SUF, CT, NEEDLE, DBUF + off, cnt, oc, of);              \
        dc_emit(OP, 32, ((uint64_t)mode << 16) | (unsigned)off, cnt, oc, of, FL_ALL); \
    }                                                                      \
} while (0)

static void run_cases(void) {
    MOVS_CASES("b", 1, "movsb");
    MOVS_CASES("w", 2, "movsw");
    MOVS_CASES("l", 4, "movsl");
    STOS_CASES("b", uint8_t,  1, 0xab,       "stosb");
    STOS_CASES("w", uint16_t, 2, 0xabcd,     "stosw");
    STOS_CASES("l", uint32_t, 4, 0xdeadbeef, "stosl");
    CMPS_CASES("b", 1, "cmpsb");
    CMPS_CASES("w", 2, "cmpsw");
    CMPS_CASES("l", 4, "cmpsl");
    SCAS_CASES("b", uint8_t,  1, 0x99,       "scasb");
    SCAS_CASES("w", uint16_t, 2, 0x9999,     "scasw");
    SCAS_CASES("l", uint32_t, 4, 0x99999999, "scasl");
#if HAVE_W64
    MOVS_CASES("q", 8, "movsq");
    STOS_CASES("q", uint64_t, 8, 0xdeadbeefcafebabeULL, "stosq");
    CMPS_CASES("q", 8, "cmpsq");
    SCAS_CASES("q", uint64_t, 8, 0x9999999999999999ULL, "scasq");
#endif
}
