#ifndef TLB_H
#define TLB_H

#include <string.h>
#include "emu/mmu.h"
#include "debug.h"

struct tlb_entry {
    page_t page;
    page_t page_if_writable;
    uintptr_t data_minus_addr;
};
#define TLB_BITS 10
#define TLB_SIZE (1 << TLB_BITS)
struct tlb {
    struct mmu *mmu;
    page_t dirty_page;
    uint64_t mem_changes;
    // this is basically one of the return values of tlb_handle_miss, tlb_{read,write}, and __tlb_{read,write}_cross_page
    // yes, this sucks
    guest_addr_t segfault_addr;
    struct tlb_entry entries[TLB_SIZE];
};

#define TLB_INDEX(addr) ((((addr >> PAGE_BITS) ^ (addr >> (PAGE_BITS + TLB_BITS)))) & (TLB_SIZE - 1))
#define TLB_PAGE(addr) ((page_t) PAGE(addr) << PAGE_BITS)
#define TLB_PAGE_EMPTY 1
void tlb_refresh(struct tlb *tlb, struct mmu *mmu);
void tlb_free(struct tlb *tlb);
void tlb_flush(struct tlb *tlb);
void *tlb_handle_miss(struct tlb *tlb, guest_addr_t addr, int type);
void *tlb_write_ptr_slow(struct tlb *tlb, guest_addr_t addr);

forceinline __no_instrument void *__tlb_read_ptr(struct tlb *tlb, guest_addr_t addr) {
    if (unlikely(tlb->mem_changes != tlb->mmu->changes))
        tlb_flush(tlb);
    struct tlb_entry entry = tlb->entries[TLB_INDEX(addr)];
    if (entry.page == TLB_PAGE(addr)) {
        void *address = (void *) (entry.data_minus_addr + addr);
        posit(address != NULL);
        return address;
    }
    return tlb_handle_miss(tlb, addr, MEM_READ);
}
bool __tlb_read_cross_page(struct tlb *tlb, guest_addr_t addr, char *out, unsigned size);
forceinline __no_instrument bool tlb_read(struct tlb *tlb, guest_addr_t addr, void *out, unsigned size) {
    if (PGOFFSET(addr) > PAGE_SIZE - size)
        return __tlb_read_cross_page(tlb, addr, out, size);
    void *ptr = __tlb_read_ptr(tlb, addr);
    if (ptr == NULL)
        return false;
    memcpy(out, ptr, size);
    return true;
}

forceinline __no_instrument void *__tlb_write_ptr(struct tlb *tlb, guest_addr_t addr) {
    if (unlikely(tlb->mem_changes != tlb->mmu->changes))
        tlb_flush(tlb);
    struct tlb_entry *entry = &tlb->entries[TLB_INDEX(addr)];
    if (entry->page_if_writable == TLB_PAGE(addr)) {
        // Revalidate cached writable hits against the authoritative MMU view.
        // Host page protections are process-global, so a stale positive write
        // hit is unsafe even if the guest would have taken a fault.
        void *page_ptr = mmu_translate(tlb->mmu, TLB_PAGE(addr), MEM_WRITE);
        if (page_ptr == NULL) {
            entry->page_if_writable = TLB_PAGE_EMPTY;
            return NULL;
        }
        entry->data_minus_addr = (uintptr_t) page_ptr - TLB_PAGE(addr);
        tlb->dirty_page = TLB_PAGE(addr);
        void *address = (void *) (entry->data_minus_addr + addr);
        posit(address != NULL);
        return address;
    }
    return tlb_handle_miss(tlb, addr, MEM_WRITE);
}
bool __tlb_write_cross_page(struct tlb *tlb, guest_addr_t addr, const char *value, unsigned size);
forceinline __no_instrument bool tlb_write(struct tlb *tlb, guest_addr_t addr, const void *value, unsigned size) {
    if (PGOFFSET(addr) > PAGE_SIZE - size)
        return __tlb_write_cross_page(tlb, addr, value, size);
    void *ptr = __tlb_write_ptr(tlb, addr);
    if (ptr == NULL)
        return false;
    memcpy(ptr, value, size);
    return true;
}

#endif
