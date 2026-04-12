#ifndef CPUID_H
#define CPUID_H

#include "misc.h"
#include "kernel/task.h"
#include "kernel/abi.h"
extern bool isGlibC;

static inline bool cpuid_guest_supports_long_mode(void) {
    return current != NULL && current->abi == GUEST_ABI_AMD64;
}

static inline dword_t cpuid_basic_max_leaf(void) {
    return 0x01;
}

static inline dword_t cpuid_extended_max_leaf(void) {
    return cpuid_guest_supports_long_mode() ? 0x80000001u : 0x80000000u;
}

static inline dword_t cpuid_leaf1_ecx_features(void) {
    return 0;
}

static inline dword_t cpuid_leaf1_edx_features(void) {
    dword_t features = (1 << 0)  // fpu
        | (1 << 15) // cmov
        | (1 << 23) // mmx
        | (1 << 26); // sse2
    if (isGlibC)
        features |= (1 << 25); // sse; musl handles sse being enabled badly. -mke
    return features;
}

static inline dword_t cpuid_leaf80000001_ecx_features(void) {
    return 0;
}

static inline dword_t cpuid_leaf80000001_edx_features(void) {
    dword_t features = 0;
    if (cpuid_guest_supports_long_mode())
        features |= (1 << 29); // lm
    return features;
}

static inline void do_cpuid(dword_t *eax, dword_t *ebx, dword_t *ecx, dword_t *edx) {
    dword_t leaf = *eax;
    switch (leaf) {
        case 0:
            *eax = cpuid_basic_max_leaf();
            *ebx = 0x756e6547; // Genu
            *edx = 0x49656e69; // ineI
            *ecx = 0x6c65746e; // ntel
            break;
        case 1:
            *eax = 0x0; // say nothing about cpu model number
            *ebx = 0x0; // processor number 0, flushes 0 bytes on clflush
            *ecx = cpuid_leaf1_ecx_features();
            *edx = cpuid_leaf1_edx_features();
            break;
        case 0x80000000:
            *eax = cpuid_extended_max_leaf();
            *ebx = 0;
            *ecx = 0;
            *edx = 0;
            break;
        case 0x80000001:
            *eax = 0;
            *ebx = 0;
            *ecx = cpuid_leaf80000001_ecx_features();
            *edx = cpuid_leaf80000001_edx_features();
            break;
        default: // if leaf is too high, use highest supported leaf
            if (leaf >= 0x80000000) {
                *eax = cpuid_extended_max_leaf();
                *ebx = 0;
                *ecx = 0;
                *edx = 0;
            } else {
                *eax = 0x0; // say nothing about cpu model number
                *ebx = 0x0; // processor number 0, flushes 0 bytes on clflush
                *ecx = cpuid_leaf1_ecx_features();
                *edx = cpuid_leaf1_edx_features();
            }
            break;
    }
}

#endif
