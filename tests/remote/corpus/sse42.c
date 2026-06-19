/*
 * sse42 — differential test for SSE4.2: crc32 (CRC32C) and the four string
 * compare instructions pcmp{e,i}str{m,i}. All are integer/bit operations, so
 * exact byte-identical results are required vs mint's real Intel silicon.
 *
 * The string ops exercise every aggregation mode (equal-any / ranges /
 * equal-each / equal-ordered), both polarities, both element widths, and both
 * output selections (index lsb/msb, bit-mask/expanded-mask), over strings with
 * and without embedded nulls (implicit length) plus explicit EAX/EDX lengths.
 */
#include "diff_common.h"

typedef int v4si __attribute__((vector_size(16)));

/* 16-byte operands; some null-terminated (for implicit length), some full. */
static const uint8_t STR[][16] = {
    {'h','e','l','l','o',0,0,0,0,0,0,0,0,0,0,0},
    {'l','l','o',' ','w','o','r','l','d',0,0,0,0,0,0,0},
    {'a','b','c','d','e','f','g','h','i','j','k','l','m','n','o','p'},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {'A','Z','a','z','0','9',0,0,0,0,0,0,0,0,0,0},   /* ranges: pairs */
    {'m','5','Q',' ','!','x','C','9',0,0,0,0,0,0,0,0},
    {'h','e','l','p',0,0,0,0,0,0,0,0,0,0,0,0},
};
#define NS (sizeof STR / sizeof STR[0])

#define FL_STR (FL_CF | FL_ZF | FL_SF | FL_OF)

static void emit_xmm(const char *op, uint64_t key, const void *xmm) {
    const uint32_t *o = (const uint32_t *)xmm;
    for (int k = 0; k < 4; k++)
        dc_emit(op, 32, key, k, o[k], 0, 0);
}

#define PISTRI(IMM) do {                                                       \
    for (size_t i = 0; i < NS; i++) for (size_t j = 0; j < NS; j++) {           \
        v4si a, b; unsigned ecx = 0; unsigned long fl = 0;                     \
        memcpy(&a, STR[i], 16); memcpy(&b, STR[j], 16);                        \
        __asm__ volatile("pcmpistri $%c[m], %[b], %[a]\n\t" PUSHF "\n\tpop %[f]"\
            : "=c"(ecx), [f] "=r"(fl)                                          \
            : [a] "x"(a), [b] "x"(b), [m] "i"(IMM) : "cc");                    \
        dc_emit("pcmpistri", 32, ((uint64_t)(IMM) << 8) | (i << 4) | j, 0,     \
                (uint32_t)ecx, fl, FL_STR);                                    \
    } } while (0)

#define PISTRM(IMM) do {                                                       \
    for (size_t i = 0; i < NS; i++) for (size_t j = 0; j < NS; j++) {           \
        v4si a, b, m_; uint32_t o[4]; unsigned long fl = 0;                    \
        memcpy(&a, STR[i], 16); memcpy(&b, STR[j], 16);                        \
        __asm__ volatile("pcmpistrm $%c[m], %[b], %[a]\n\tmovdqa %%xmm0, %[o]\n\t" PUSHF "\n\tpop %[f]" \
            : [o] "=x"(m_), [f] "=r"(fl)                                       \
            : [a] "x"(a), [b] "x"(b), [m] "i"(IMM) : "cc", "xmm0");            \
        memcpy(o, &m_, 16);                                                    \
        emit_xmm("pcmpistrm", ((uint64_t)(IMM) << 8) | (i << 4) | j, o);       \
        dc_emit("pcmpistrmF", 32, ((uint64_t)(IMM) << 8) | (i << 4) | j, 0, 0, fl, FL_STR); \
    } } while (0)

#define PESTRI(IMM, LA, LB) do {                                               \
    for (size_t i = 0; i < NS; i++) for (size_t j = 0; j < NS; j++) {           \
        v4si a, b; unsigned ecx = 0; unsigned long fl = 0;                     \
        memcpy(&a, STR[i], 16); memcpy(&b, STR[j], 16);                        \
        __asm__ volatile("pcmpestri $%c[m], %[b], %[a]\n\t" PUSHF "\n\tpop %[f]"\
            : "=c"(ecx), [f] "=r"(fl)                                          \
            : [a] "x"(a), [b] "x"(b), [m] "i"(IMM), "a"(LA), "d"(LB) : "cc");  \
        dc_emit("pcmpestri", 32,                                               \
                ((uint64_t)(IMM) << 16) | ((LA) << 12) | ((LB) << 8) | (i << 4) | j, \
                0, (uint32_t)ecx, fl, FL_STR);                                 \
    } } while (0)

#define PESTRM(IMM, LA, LB) do {                                               \
    for (size_t i = 0; i < NS; i++) for (size_t j = 0; j < NS; j++) {           \
        v4si a, b, m_; uint32_t o[4]; unsigned long fl = 0;                    \
        memcpy(&a, STR[i], 16); memcpy(&b, STR[j], 16);                        \
        __asm__ volatile("pcmpestrm $%c[m], %[b], %[a]\n\tmovdqa %%xmm0, %[o]\n\t" PUSHF "\n\tpop %[f]" \
            : [o] "=x"(m_), [f] "=r"(fl)                                       \
            : [a] "x"(a), [b] "x"(b), [m] "i"(IMM), "a"(LA), "d"(LB) : "cc", "xmm0"); \
        memcpy(o, &m_, 16);                                                    \
        emit_xmm("pcmpestrm",                                                  \
                ((uint64_t)(IMM) << 16) | ((LA) << 12) | ((LB) << 8) | (i << 4) | j, o); \
    } } while (0)

__attribute__((target("sse4.2")))
static void run_cases(void) {
    /* ---- crc32 (CRC32C accumulate) ---- */
    static const uint32_t CINIT[] = { 0u, 0xffffffffu, 0x12345678u };
    static const uint64_t CDATA[] = { 0u, 0x42u, 0xdeadbeefu, 0x89abcdefu, 0x0123456789abcdefULL };
    for (size_t a = 0; a < sizeof CINIT / sizeof CINIT[0]; a++)
        for (size_t d = 0; d < sizeof CDATA / sizeof CDATA[0]; d++) {
            uint32_t acc; uint8_t b8 = (uint8_t)CDATA[d];
            uint16_t w16 = (uint16_t)CDATA[d]; uint32_t d32 = (uint32_t)CDATA[d];
            acc = CINIT[a];
            __asm__ volatile("crc32b %[s], %[d]" : [d] "+r"(acc) : [s] "r"(b8));
            dc_emit("crc32b", 32, (a << 4) | d, 0, acc, 0, 0);
            acc = CINIT[a];
            __asm__ volatile("crc32w %[s], %[d]" : [d] "+r"(acc) : [s] "r"(w16));
            dc_emit("crc32w", 32, (a << 4) | d, 0, acc, 0, 0);
            acc = CINIT[a];
            __asm__ volatile("crc32l %[s], %[d]" : [d] "+r"(acc) : [s] "r"(d32));
            dc_emit("crc32l", 32, (a << 4) | d, 0, acc, 0, 0);
#if defined(__x86_64__)
            uint64_t acc64 = CINIT[a], q64 = CDATA[d];
            __asm__ volatile("crc32q %[s], %[d]" : [d] "+r"(acc64) : [s] "r"(q64));
            dc_emit("crc32q_lo", 32, (a << 4) | d, 0, (uint32_t)acc64, 0, 0);
            dc_emit("crc32q_hi", 32, (a << 4) | d, 0, (uint32_t)(acc64 >> 32), 0, 0);
#endif
        }

    /* ---- pcmpistri / pcmpistrm (implicit length) ---- */
    /* imm: fmt[1:0] | agg[3:2] | polarity[5:4] | outsel[6]; literals (imm must be const) */
    PISTRI(0x00); PISTRI(0x40); PISTRI(0x04); PISTRI(0x44); PISTRI(0x08); PISTRI(0x48);
    PISTRI(0x0c); PISTRI(0x4c); PISTRI(0x14); PISTRI(0x18); PISTRI(0x1c); PISTRI(0x30);
    PISTRI(0x38); PISTRI(0x01); PISTRI(0x05); PISTRI(0x09); PISTRI(0x0d);
    PISTRM(0x00); PISTRM(0x40); PISTRM(0x04); PISTRM(0x44); PISTRM(0x08); PISTRM(0x4c);
    PISTRM(0x18); PISTRM(0x01); PISTRM(0x45);

    /* ---- pcmpestri / pcmpestrm (explicit length: full, partial, zero) ---- */
    PESTRI(0x00, 16, 16); PESTRI(0x0c, 5, 9); PESTRI(0x04, 6, 4);
    PESTRI(0x18, 8, 16);  PESTRI(0x40, 3, 7); PESTRI(0x01, 4, 6);
    PESTRM(0x00, 16, 16); PESTRM(0x0c, 5, 9); PESTRM(0x44, 6, 4);
}
