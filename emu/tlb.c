#include "emu/cpu.h"
#include "emu/tlb.h"
#include "emu/interrupt.h"
#include "kernel/signal.h"
#include "kernel/task.h"

// AdvSIMD LD1/ST1 (load/store multiple single-element structures,
// contiguous form): transfer `count` consecutive V registers (wrapping
// mod 32) of `regbytes` (8 for the .8b/.4h/.2s arrangements, 16 for the
// .16b/... Q=1 arrangements) each, starting at `addr`. Done in C because
// the whole transfer can span page boundaries per register — reusing the
// crosspage-capable tlb_read/tlb_write is far simpler and safer than
// hand-rolling the multi-register crosspage assembly. Returns INT_NONE on
// success, or INT_PF (with cpu->segfault_addr/was_write set from the tlb)
// on the first faulting access; the calling gadget rewinds PC and exits.
// The scalar-write zero-extension (Q=0 clears the upper 64 bits) falls out
// of copying through a zero-initialized union.
//
// Success returns 0, NOT INT_NONE (which is -1) — the gadget branches to
// its fault path on a nonzero result, so INT_NONE would take the fault
// path on every success. (Real bug: it did exactly that — every
// successful ld1 exited INT_PF and the block re-ran forever.)
int arm64_vldst_multi(struct cpu_state *cpu, struct tlb *tlb, guest_addr_t addr,
                      unsigned rt, unsigned count, unsigned regbytes, int is_load) {
    for (unsigned r = 0; r < count; r++) {
        unsigned v = (rt + r) & 31;
        if (is_load) {
            union xmm_reg tmp = {};
            if (!tlb_read(tlb, addr, &tmp, regbytes)) {
                cpu->segfault_addr = tlb->segfault_addr;
                cpu->segfault_was_write = false;
                return INT_PF;
            }
            cpu->arm64_v[v] = tmp;
        } else {
            if (!tlb_write(tlb, addr, &cpu->arm64_v[v], regbytes)) {
                cpu->segfault_addr = tlb->segfault_addr;
                cpu->segfault_was_write = true;
                return INT_PF;
            }
        }
        addr += regbytes;
    }
    return 0;
}

void tlb_refresh(struct tlb *tlb, struct mmu *mmu) {
    if (tlb->mmu == mmu &&
            tlb->mem_changes == atomic_load_explicit(&mmu->changes, memory_order_relaxed)) {
        return;
    }
    tlb->mmu = mmu;
    tlb->dirty_page = TLB_PAGE_EMPTY;
    tlb->mem_changes = atomic_load_explicit(&mmu->changes, memory_order_relaxed);
    tlb_flush(tlb);
}

void tlb_flush(struct tlb *tlb) {
    tlb->mem_changes = atomic_load_explicit(&tlb->mmu->changes, memory_order_relaxed);
    for (unsigned i = 0; i < TLB_SIZE; i++)
        tlb->entries[i] = (struct tlb_entry) {.page = 1, .page_if_writable = 1};
}

void tlb_free(struct tlb *tlb) {
    free(tlb);
}

bool __tlb_read_cross_page(struct tlb *tlb, guest_addr_t addr, char *value, unsigned size) {
    char *ptr1 = __tlb_read_ptr(tlb, addr);
    if (ptr1 == NULL) {
        return false;
    }
    char *ptr2 = __tlb_read_ptr(tlb, (PAGE(addr) + 1) << PAGE_BITS);
    if (ptr2 == NULL) {
        return false;
    }
    size_t part1 = PAGE_SIZE - PGOFFSET(addr);
    assert(part1 < size);
    memcpy(value, ptr1, part1);
    memcpy(value + part1, ptr2, size - part1);
    return true;
}

bool __tlb_write_cross_page(struct tlb *tlb, guest_addr_t addr, const char *value, unsigned size) {
    char *ptr1 = __tlb_write_ptr(tlb, addr);
    if (ptr1 == NULL) {
        return false;
    }
    char *ptr2 = __tlb_write_ptr(tlb, (PAGE(addr) + 1) << PAGE_BITS);
    if (ptr2 == NULL) {
        return false;
    }
    size_t part1 = PAGE_SIZE - PGOFFSET(addr);
    assert(part1 < size);
    memcpy(ptr1, value, part1);
    memcpy(ptr2, value + part1, size - part1);
    return true;
}

__no_instrument void *tlb_handle_miss(struct tlb *tlb, guest_addr_t addr, int type) {
    char *ptr = mmu_translate(tlb->mmu, TLB_PAGE(addr), type);
    if (atomic_load_explicit(&tlb->mmu->changes, memory_order_relaxed) != tlb->mem_changes)
        tlb_flush(tlb);
    if (ptr == NULL) {
        tlb->segfault_addr = addr;
        return NULL;
    }
    tlb->dirty_page = TLB_PAGE(addr);

    struct tlb_entry *tlb_ent = &tlb->entries[TLB_INDEX(addr)];
    tlb_ent->page = TLB_PAGE(addr);
    if (type == MEM_WRITE)
        tlb_ent->page_if_writable = tlb_ent->page;
    else
        // 1 is not a valid page so this won't look like a hit
        tlb_ent->page_if_writable = TLB_PAGE_EMPTY;
    tlb_ent->data_minus_addr = (uintptr_t) ptr - TLB_PAGE(addr);
    return (void *) (tlb_ent->data_minus_addr + addr);
}

__no_instrument void *tlb_write_ptr_slow(struct tlb *tlb, guest_addr_t addr) {
    return __tlb_write_ptr(tlb, addr);
}
