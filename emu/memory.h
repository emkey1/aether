#ifndef MEMORY_H
#define MEMORY_H

#include <stdatomic.h>
#include <unistd.h>
#include <stdbool.h>
#include <sched.h>
#include "emu/mmu.h"
#include "util/list.h"
#include "util/sync.h"
#include "misc.h"
#if ENGINE_JIT
struct jit;
#endif

struct pt_directory_chunk;

struct mem {
    _Atomic(struct pt_directory_chunk *) *pgdir_root;
    // Set-only bitmap (one bit per pgdir_root entry) of which roots have a
    // chunk. Page-table chunks are never freed until mem_destroy, so this only
    // ever gains bits. mem_next_allocated_leaf_base() bit-scans it to skip empty
    // roots in bulk instead of linearly probing the (32 KiB) pgdir_root array --
    // the dominant pt_find_hole cost on a large/sparse amd64 address space.
    _Atomic uint64_t *pgdir_root_bitmap;
    page_t page_limit;
    page_t mmap_floor;
    page_t mmap_ceiling;
    _Atomic int quiesce_requested;

#if ENGINE_JIT
    struct jit *jit;
#endif
    struct mmu mmu;
    struct {
        atomic_int count; // If positive, don't delete yet, wait_to_delete
        bool ready_to_be_freed; // Should be false initially
    } reference;

    wrlock_t lock;
    // Serializes every structural page-table mutation (map/unmap/protect/COW).
    // Evicting writers hold this *and* take `lock` in write mode + poke siblings.
    // The growth-mmap fast path holds ONLY this: adding never-mapped pages needs
    // no reader eviction (no stale TLB entry) and no rwlock (entry publication is
    // atomic), just mutual exclusion against a concurrent unmap freeing chunks.
    pthread_mutex_t pt_alloc_lock;
};

extern _Atomic long quiesce_reader_naps;

// Wait out a writer holding the mem-quiesce barrier. sched_yield() for the first
// burst hands the CPU straight to the writer so the barrier (a brief page-table
// edit) clears in microseconds instead of waiting out nanosleep's ~13us floor;
// only a long hold (e.g. fork COW of a big address space) falls through to
// nanosleep so it can't hot-spin a core. `spins` is threaded across both the
// inner and outer waits of a single acquire.
static inline void mem_quiesce_backoff(int *spins) {
    if ((*spins)++ < 256) {
        sched_yield();
    } else {
        atomic_fetch_add_explicit(&quiesce_reader_naps, 1, memory_order_relaxed);
        nanosleep(&lock_pause, NULL);
    }
}

static inline void mem_read_lock_quiesce_aware(struct mem *mem) {
    if (mem == NULL)
        return;
    int spins = 0;
    while (true) {
        while (atomic_load_explicit(&mem->quiesce_requested, memory_order_acquire) > 0)
            mem_quiesce_backoff(&spins);
        read_lock(&mem->lock);
        if (atomic_load_explicit(&mem->quiesce_requested, memory_order_acquire) == 0)
            return;
        read_unlock(&mem->lock);
        mem_quiesce_backoff(&spins);
    }
}

static inline void mem_read_unlock_quiesce_aware(struct mem *mem) {
    if (mem != NULL)
        read_unlock(&mem->lock);
}

#define MEM_DEFAULT_PAGE_LIMIT ((page_t) 1 << 20)
#define MEM_DEFAULT_MMAP_FLOOR ((page_t) 0x40000)
#define MEM_DEFAULT_MMAP_CEILING ((page_t) 0xf7ffe)
#define MEM_PTDIR_BITS 10
#define MEM_PTDIR_SIZE (1 << MEM_PTDIR_BITS)
#define MEM_PGDIR_MID_BITS 13
#define MEM_PGDIR_MID_SIZE (1 << MEM_PGDIR_MID_BITS)
#define MEM_PGDIR_ROOT_BITS 12
#define MEM_PGDIR_ROOT_SIZE (1 << MEM_PGDIR_ROOT_BITS)
#define MEM_MAX_PAGE_LIMIT ((page_t) 1 << (MEM_PTDIR_BITS + MEM_PGDIR_MID_BITS + MEM_PGDIR_ROOT_BITS))

// Initialize the address space
void mem_init(struct mem *mem);
// Uninitialize the address space
void mem_destroy(struct mem *mem);
void mem_set_page_limit(struct mem *mem, page_t limit);
void mem_set_mmap_window(struct mem *mem, page_t floor, page_t ceiling);
// Return the pagetable entry for the given page
struct pt_entry *mem_pt(struct mem *mem, page_t page);
// Increment *page, skipping over unallocated page directories. Intended to be
// used as the incremenent in a for loop to traverse mappings.
void mem_next_page(struct mem *mem, page_t *page);
size_t mem_mapped_page_count(struct mem *mem);
void *mem_ptr_fault(struct mem *mem, guest_addr_t addr, int type);

#define BYTES_ROUND_DOWN(bytes) (PAGE(bytes) << PAGE_BITS)
#define BYTES_ROUND_UP(bytes) (PAGE_ROUND_UP(bytes) << PAGE_BITS)

#define LEAK_DEBUG 0

struct data {
    void *data; // immutable
    size_t size; // also immutable
    atomic_uint refcount;
    uintptr_t shared_key;
    uint8_t *host_page_prot; // cached mirrored host protections, one per host page

    // for display in /proc/pid/maps
    struct fd *fd;
    size_t file_offset;
    const char *name;
#if LEAK_DEBUG
    int pid;
    guest_addr_t dest;
#endif
};
struct pt_entry {
    struct data *data;
    size_t offset;
    unsigned flags;
#if ENGINE_JIT
    struct list blocks[2];
#endif
};
// page flags
// P_READ and P_EXEC are ignored for now
#define P_READ (1 << 0)
#define P_WRITE (1 << 1)
#undef P_EXEC // defined in sys/proc.h on darwin
#define P_EXEC (1 << 2)
#define P_RWX (P_READ | P_WRITE | P_EXEC)
#define P_GROWSDOWN (1 << 3)
#define P_COW (1 << 4)
#define P_WRITABLE(flags) (flags & P_WRITE && !(flags & P_COW))

// mapping was created with pt_map_nothing
#define P_ANONYMOUS (1 << 6)
// mapping was created with MAP_SHARED, should not CoW
#define P_SHARED (1 << 7)

bool pt_is_hole(struct mem *mem, page_t start, pages_t pages);
page_t pt_find_hole(struct mem *mem, pages_t size);

// Map memory + offset into fake memory, unmapping existing mappings. Takes
// ownership of memory. It will be freed with:
// munmap(memory, pages * PAGE_SIZE)
int pt_map(struct mem *mem, page_t start, pages_t pages, void *memory, size_t offset, unsigned flags);
// Map empty space into fake memory
int pt_map_nothing(struct mem *mem, page_t page, pages_t pages, unsigned flags);
// Move an existing mapped range into a hole.
int pt_move(struct mem *mem, page_t old_start, page_t new_start, pages_t pages);
// Unmap fake memory, return -1 if any part of the range isn't mapped and 0 otherwise
int pt_unmap(struct mem *mem, page_t start, pages_t pages);
// like pt_unmap but doesn't care if part of the range isn't mapped
int pt_unmap_always(struct mem *mem, page_t start, pages_t pages);
// Set the flags on memory
int pt_set_flags(struct mem *mem, page_t start, pages_t pages, int flags);
// Copy pages from src memory to dst memory using copy-on-write
int pt_copy_on_write(struct mem *src, struct mem *dst, page_t start, page_t pages);

// Must call with mem read-locked.
void *mem_ptr(struct mem *mem, guest_addr_t addr, int type);
int mem_segv_reason(struct mem *mem, guest_addr_t addr);

// Reference counting is important
void mem_ref_cnt_mod(struct mem *mem, int value);
int mem_ref_cnt_get(struct mem *mem);

extern size_t real_page_size;

#endif
