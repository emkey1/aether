#ifndef KERNEL_MM_H
#define KERNEL_MM_H

#include "kernel/abi.h"
#include "emu/memory.h"
#include "misc.h"

// uses mem.lock instead of having a lock of its own
struct mm {
    atomic_uint refcount;
    struct mem mem;
    struct list shm_regions;

    guest_addr_t vdso; // immutable
    guest_addr_t start_brk; // immutable
    guest_addr_t brk;

    // crap for procfs
    guest_addr_t argv_start;
    guest_addr_t argv_end;
    guest_addr_t env_start;
    guest_addr_t env_end;
    guest_addr_t auxv_start;
    guest_addr_t auxv_end;
    guest_addr_t stack_start;
    struct fd *exefile;
};

// Create a new address space
struct mm *mm_new(enum guest_abi abi);
// Clone (COW) the address space
struct mm *mm_copy(struct mm *mm);
// Increment the refcount
void mm_retain(struct mm *mem);
// Decrement the refcount, destroy everything in the space if 0
void mm_release(struct mm *mem);

// SysV shm bookkeeping hooks owned by kernel/ipc.c.
void ipc_mm_init(struct mm *mm);
void ipc_mm_copy(struct mm *dst, struct mm *src);
void ipc_mm_release(struct mm *mm);

#endif
