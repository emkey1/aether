#include <time.h>
#include "emu/cpu.h"
#include "emu/cpuid.h"
#include "kernel/task.h"

void helper_cpuid(dword_t *a, dword_t *b, dword_t *c, dword_t *d) {
    do_cpuid(a, b, c, d);
}

void helper_rdtsc(struct cpu_state *cpu) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    uint64_t tsc = now.tv_sec * 1000000000l + now.tv_nsec;
    cpu->eax = tsc & 0xffffffff;
    cpu->edx = tsc >> 32;
    cpu->amd64_regs[amd64_rax] = cpu->eax;
    cpu->amd64_regs[amd64_rdx] = cpu->edx;
}

void helper_expand_flags(struct cpu_state *cpu) {
    expand_flags(cpu);
}

void helper_collapse_flags(struct cpu_state *cpu) {
    collapse_flags(cpu);
}

void helper_trace_unaligned_atomic(struct cpu_state *cpu, dword_t addr, dword_t tag) {
    (void) cpu;
    (void) addr;
    (void) tag;
}
