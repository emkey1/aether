#ifndef EMU_CPU_MEM_H
#define EMU_CPU_MEM_H

#include "misc.h"

// Guest page numbers are internal MM bookkeeping. Keep them wide enough for
// future 64-bit address spaces even while guest-visible addr_t stays 32-bit.
typedef qword_t page_t;
typedef qword_t pages_t;
#define BAD_PAGE ((page_t) -1)

#ifndef __KERNEL__
#define PAGE_BITS 12
#undef PAGE_SIZE // defined in system headers somewhere
#define PAGE_SIZE (1 << PAGE_BITS)
#define PAGE(addr) ((addr) >> PAGE_BITS)
#define PGOFFSET(addr) ((addr) & (PAGE_SIZE - 1))
// bytes MUST be unsigned if you would like this to overflow to zero
#define PAGE_ROUND_UP(bytes) (PAGE((bytes) + PAGE_SIZE - 1))
#endif

struct mmu {
    struct mmu_ops *ops;
    struct jit *jit;
    uint64_t changes;
    bool requires_write_revalidate;
};

#define MEM_READ 0
#define MEM_WRITE 1
#define MEM_WRITE_PTRACE 2

struct mmu_ops {
    // type is MEM_READ or MEM_WRITE
    void *(*translate)(struct mmu *mmu, guest_addr_t addr, int type);
};

static inline void *mmu_translate(struct mmu *mmu, guest_addr_t addr, int type) {
    return mmu->ops->translate(mmu, addr, type);
}

#endif
