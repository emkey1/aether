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
}

void helper_expand_flags(struct cpu_state *cpu) {
    expand_flags(cpu);
}

void helper_collapse_flags(struct cpu_state *cpu) {
    collapse_flags(cpu);
}

void helper_trace_unaligned_atomic(struct cpu_state *cpu, dword_t addr, dword_t tag) {
    static unsigned budget = 32;
    (void) cpu;
    unsigned slot = __atomic_fetch_sub(&budget, 1, __ATOMIC_RELAXED);
    if (slot == 0 || slot > 32 || current == NULL || !current->force_no_jit_cache)
        return;
    printk("tracked i386 atomic unaligned: pid=%d tgid=%d comm=%s addr=%#x tag=%u\n",
            current->pid, current->tgid, current->comm, addr, tag);
}
