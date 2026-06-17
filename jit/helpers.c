#include <time.h>
#include "emu/cpu.h"
#include "emu/cpuid.h"
#include "emu/tlb.h"
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

// ---------------------------------------------------------------------------
// Page-batched rep movs / rep stos fast path.
//
// The per-element string gadgets in jit/gadgets-aarch64/string.S re-run the
// full inline TLB hash (read_prep/write_prep) on EVERY element of a rep loop,
// so a `rep movsb` of N bytes pays ~2N translation sequences even though the
// host page base is invariant until the address crosses a 4 KB boundary.
//
// This helper resolves the host page once per page-run (via the same
// __tlb_read_ptr/__tlb_write_ptr the C TLB path uses, so it inherits all the
// miss / COW / cross-page / write-revalidate / staleness-flush handling) and
// bulk-copies the run with memmove/memset. It is invoked only for the forward
// (DF=0) direction with ecx >= a small threshold; the gadget keeps the existing
// per-element loop for backward, tiny, and data-dependent (cmps/scas) reps.
//
// Operates directly on cpu->{esi,edi,ecx,eax}; the gadget spills the
// host-register-cached copies before the call and reloads them after. On a
// fault it returns 1 (source/read) or 2 (dest/write) with cpu->{esi,edi,ecx}
// left pointing at the element about to be processed (x86 #PF restartability),
// and tlb->segfault_addr already set by the miss handler, so the gadget jumps
// to the existing segfault_read/segfault_write exit.
//
// Overlapping forward movs (edi>esi within the run) is the x86 "smear" case
// that memmove would NOT reproduce, so it falls back to single-element copies
// through guest memory, which smears identically to the hardware.
// ---------------------------------------------------------------------------
int rep_string_fast(struct cpu_state *cpu, struct tlb *tlb, unsigned elem_size, int is_movs) {
    uint32_t ecx = cpu->ecx;
    uint32_t edi = cpu->edi;
    uint32_t esi = cpu->esi;
    uint32_t eax = cpu->eax;

    while (ecx != 0) {
        uint32_t run = (PAGE_SIZE - PGOFFSET(edi)) / elem_size; // whole elems left in dst page
        if (run > ecx)
            run = ecx;
        if (is_movs) {
            uint32_t src_room = (PAGE_SIZE - PGOFFSET(esi)) / elem_size;
            if (run > src_room)
                run = src_room;
        }

        // memmove only matches x86 ascending semantics when the run does not
        // forward-overlap (edi <= esi, or no overlap within the run).
        int overlap_smear = is_movs && edi > esi &&
            (uint64_t) (edi - esi) < (uint64_t) run * elem_size;

        if (run >= 1 && !overlap_smear) {
            char *dst = (char *) __tlb_write_ptr(tlb, edi);
            if (dst == NULL)
                return 2;
            if (is_movs) {
                char *src = (char *) __tlb_read_ptr(tlb, esi);
                if (src == NULL)
                    return 1;
                memmove(dst, src, (size_t) run * elem_size);
                esi += run * elem_size;
            } else if (elem_size == 1) {
                memset(dst, eax & 0xff, run);
            } else if (elem_size == 2) {
                uint16_t v = eax & 0xffff;
                for (uint32_t i = 0; i < run; i++)
                    ((uint16_t *) dst)[i] = v;
            } else {
                for (uint32_t i = 0; i < run; i++)
                    ((uint32_t *) dst)[i] = eax;
            }
            edi += run * elem_size;
            ecx -= run;
        } else {
            // One element via the cross-page-safe path: handles an element
            // straddling a page boundary (run == 0) and the overlap-smear case.
            if (is_movs) {
                char tmp[8];
                if (!tlb_read(tlb, esi, tmp, elem_size))
                    return 1;
                if (!tlb_write(tlb, edi, tmp, elem_size))
                    return 2;
                esi += elem_size;
            } else {
                if (!tlb_write(tlb, edi, &eax, elem_size))
                    return 2;
            }
            edi += elem_size;
            ecx -= 1;
        }

        cpu->ecx = ecx;
        cpu->edi = edi;
        cpu->esi = esi;
    }
    return 0;
}
