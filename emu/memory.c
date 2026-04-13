#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>

#define DEFAULT_CHANNEL memory
#include "debug.h"
#include "kernel/errno.h"
#include "kernel/signal.h"
#include "emu/memory.h"
#include "jit/jit.h"
#include "kernel/vdso.h"
#include "kernel/task.h"
#include "fs/fd.h"
#include "util/sync.h"

// The Evil global lock.  Use sparingly or not at all
extern pthread_mutex_t multicore_lock;
// Time to wait between non blocking lock attempts
struct timespec lock_pause = {0 /*secs*/, WAIT_SLEEP /*nanosecs*/};

extern bool doEnableExtraLocking;
extern pthread_mutex_t extra_lock;
extern dword_t extra_lock_pid;
extern const char extra_lock_comm;

// increment the change count
static void mem_changed(struct mem *mem);
static struct mmu_ops mem_mmu_ops;
static _Atomic uint64_t next_mem_change_id = 1;
#define PGDIR_TOP(page) ((page) >> MEM_PTDIR_BITS)
#define PGDIR_BOTTOM(page) ((page) & (MEM_PTDIR_SIZE - 1))

static size_t mem_pgdir_lower_bound(struct mem *mem, page_t top, bool *found) {
    size_t lo = 0;
    size_t hi = mem->pgdir_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (mem->pgdirs[mid].top < top)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (found != NULL)
        *found = lo < mem->pgdir_count && mem->pgdirs[lo].top == top;
    return lo;
}

static struct pt_directory *mem_pgdir_get(struct mem *mem, page_t top) {
    bool found;
    size_t slot = mem_pgdir_lower_bound(mem, top, &found);
    if (!found)
        return NULL;
    return &mem->pgdirs[slot];
}

static struct pt_directory *mem_pgdir_insert(struct mem *mem, page_t top) {
    bool found;
    size_t slot = mem_pgdir_lower_bound(mem, top, &found);
    if (found)
        return &mem->pgdirs[slot];

    struct pt_entry *entries = calloc(MEM_PTDIR_SIZE, sizeof(*entries));
    if (entries == NULL)
        return NULL;

    if (mem->pgdir_count == mem->pgdir_capacity) {
        size_t new_capacity = mem->pgdir_capacity == 0 ? 4 : mem->pgdir_capacity * 2;
        struct pt_directory *new_pgdirs = realloc(mem->pgdirs, new_capacity * sizeof(*new_pgdirs));
        if (new_pgdirs == NULL) {
            free(entries);
            return NULL;
        }
        mem->pgdirs = new_pgdirs;
        mem->pgdir_capacity = new_capacity;
    }

    if (slot < mem->pgdir_count) {
        memmove(&mem->pgdirs[slot + 1], &mem->pgdirs[slot],
                (mem->pgdir_count - slot) * sizeof(*mem->pgdirs));
    }

    mem->pgdirs[slot] = (struct pt_directory) {
        .top = top,
        .entries = entries,
    };
    mem->pgdir_count++;
    return &mem->pgdirs[slot];
}

static void mem_pgdir_remove_if_empty(struct mem *mem, page_t top) {
    struct pt_directory *dir = mem_pgdir_get(mem, top);
    if (dir == NULL)
        return;
    for (int i = 0; i < MEM_PTDIR_SIZE; i++) {
        if (dir->entries[i].data != NULL)
            return;
    }

    size_t slot = dir - mem->pgdirs;
    free(dir->entries);
    if (slot + 1 < mem->pgdir_count) {
        memmove(&mem->pgdirs[slot], &mem->pgdirs[slot + 1],
                (mem->pgdir_count - slot - 1) * sizeof(*mem->pgdirs));
    }
    mem->pgdir_count--;
}

static struct pt_entry *mem_pt_raw(struct mem *mem, page_t page) {
    if (page >= mem->page_limit)
        return NULL;
    struct pt_directory *dir = mem_pgdir_get(mem, PGDIR_TOP(page));
    if (dir == NULL)
        return NULL;
    return &dir->entries[PGDIR_BOTTOM(page)];
}

static bool mem_page_range_valid(struct mem *mem, page_t start, pages_t pages) {
    if (pages == 0)
        return true;
    if (start >= mem->page_limit)
        return false;
    return pages <= mem->page_limit - start;
}

static page_t mem_next_mapped_page(struct mem *mem, page_t page) {
    if (page >= mem->page_limit)
        return BAD_PAGE;

    page_t top = PGDIR_TOP(page);
    bool found;
    size_t slot = mem_pgdir_lower_bound(mem, top, &found);
    for (; slot < mem->pgdir_count; slot++) {
        struct pt_directory *dir = &mem->pgdirs[slot];
        page_t dir_start = dir->top << MEM_PTDIR_BITS;
        int start_index = 0;
        if (dir->top == top)
            start_index = (int) PGDIR_BOTTOM(page);
        for (int i = start_index; i < MEM_PTDIR_SIZE; i++) {
            if (dir->entries[i].data == NULL)
                continue;
            page_t mapped = dir_start + (page_t) i;
            return mapped < mem->page_limit ? mapped : BAD_PAGE;
        }
    }
    return BAD_PAGE;
}

void mem_init(struct mem *mem) {
    mem->pgdirs = NULL;
    mem->pgdir_count = 0;
    mem->pgdir_capacity = 0;
    mem->page_limit = MEM_DEFAULT_PAGE_LIMIT;
    mem->mmap_floor = MEM_DEFAULT_MMAP_FLOOR;
    mem->mmap_ceiling = MEM_DEFAULT_MMAP_CEILING;
    mem->mmu.ops = &mem_mmu_ops;
#if ENGINE_JIT
    mem->mmu.jit = jit_new(&mem->mmu);
#endif
    // Seed each new address space with a unique change id so a per-thread TLB
    // flushes even if malloc reuses the same mmu address after exec/exit.
    mem->mmu.changes = atomic_fetch_add_explicit(&next_mem_change_id, 1, memory_order_relaxed);
    wrlock_init(&mem->lock);
    mem->reference.count = 0;
    mem->reference.ready_to_be_freed = false;
    int rc = pthread_mutex_init(&mem->reference.lock, NULL);
    if (rc != 0) {
        // Handle error
    }
}

void mem_destroy(struct mem *mem) {
    write_lock(&mem->lock);
    pt_unmap_always(mem, 0, mem->page_limit);

#if ENGINE_JIT
    jit_free(mem->mmu.jit);
#endif
    for (size_t i = 0; i < mem->pgdir_count; i++) {
        free(mem->pgdirs[i].entries);
    }
    free(mem->pgdirs);
    mem->pgdirs = NULL;
    mem->pgdir_count = 0;
    mem->pgdir_capacity = 0;

    write_unlock_and_destroy(&mem->lock);
}

void mem_set_page_limit(struct mem *mem, page_t limit) {
    mem->page_limit = limit;
}

void mem_set_mmap_window(struct mem *mem, page_t floor, page_t ceiling) {
    mem->mmap_floor = floor;
    mem->mmap_ceiling = ceiling;
}

static struct pt_entry *mem_pt_new(struct mem *mem, page_t page) {
    if (page >= mem->page_limit)
        return NULL;
    struct pt_directory *dir = mem_pgdir_insert(mem, PGDIR_TOP(page));
    if (dir == NULL)
        return NULL;
    return &dir->entries[PGDIR_BOTTOM(page)];
}

struct pt_entry *mem_pt(struct mem *mem, page_t page) {
    struct pt_entry *entry = mem_pt_raw(mem, page);
    if (entry == NULL || entry->data == NULL)
        return NULL;
    return entry;
}

static void mem_pt_del(struct mem *mem, page_t page) {
    struct pt_entry *entry = mem_pt_raw(mem, page);
    if (entry == NULL)
        return;
    entry->data = NULL;
    mem_pgdir_remove_if_empty(mem, PGDIR_TOP(page));
}

void mem_next_page(struct mem *mem, page_t *page) {
    (*page)++;
    if (*page >= mem->page_limit) {
        *page = mem->page_limit;
        return;
    }
    if (mem_pgdir_get(mem, PGDIR_TOP(*page)) != NULL)
        return;
    bool found;
    size_t slot = mem_pgdir_lower_bound(mem, PGDIR_TOP(*page), &found);
    if (slot >= mem->pgdir_count) {
        *page = mem->page_limit;
        return;
    }
    *page = mem->pgdirs[slot].top << MEM_PTDIR_BITS;
}

page_t pt_find_hole(struct mem *mem, pages_t size) {
    if (size == 0 || mem->mmap_ceiling <= mem->mmap_floor)
        return BAD_PAGE;
    if (size > mem->mmap_ceiling - mem->mmap_floor)
        return BAD_PAGE;

    page_t best = BAD_PAGE;
    page_t prev_end = mem->mmap_floor;
    page_t page = mem_next_mapped_page(mem, mem->mmap_floor);
    while (page != BAD_PAGE && page < mem->mmap_ceiling) {
        if (page > prev_end && page - prev_end >= size)
            best = page - size;

        page_t region_page = page;
        struct pt_entry *start_pt = mem_pt(mem, region_page);
        struct data *data = start_pt->data;
        while (region_page < mem->mmap_ceiling) {
            struct pt_entry *pt = mem_pt(mem, region_page);
            if (pt == NULL)
                break;
            if ((pt->flags & P_RWX) != (start_pt->flags & P_RWX))
                break;
            if (!(pt->data == data || ((pt->flags & P_ANONYMOUS) && (start_pt->flags & P_ANONYMOUS))))
                break;
            mem_next_page(mem, &region_page);
        }
        prev_end = region_page;
        page = mem_next_mapped_page(mem, region_page);
    }
    if (mem->mmap_ceiling - prev_end >= size)
        best = mem->mmap_ceiling - size;
    return best;
}

bool pt_is_hole(struct mem *mem, page_t start, pages_t pages) {
    if (!mem_page_range_valid(mem, start, pages))
        return false;
    for (page_t page = start; page < start + pages; page++) {
        if (mem_pt(mem, page) != NULL)
            return false;
    }
    return true;
}

int pt_map(struct mem *mem, page_t start, pages_t pages, void *memory, size_t offset, unsigned flags) {
    if (!mem_page_range_valid(mem, start, pages))
        return _ENOMEM;
    if (memory == MAP_FAILED)
        return errno_map();

    // If this fails, the munmap in pt_unmap would probably fail.
    assert((uintptr_t) memory % real_page_size == 0 || memory == vdso_data);

    struct data *data = malloc(sizeof(struct data));
    if (data == NULL)
        return _ENOMEM;
    *data = (struct data) {
        .data = memory,
        .size = pages * PAGE_SIZE + offset,

#if LEAK_DEBUG
        .pid = current ? current->pid : 0,
        .dest = start << PAGE_BITS,
#endif
    };

    for (page_t page = start; page < start + pages; page++) {
        if (mem_pt(mem, page) != NULL)
            pt_unmap(mem, page, 1);
        data->refcount++;
        struct pt_entry *pt = mem_pt_new(mem, page);
        if (pt == NULL) {
            data->refcount--;
            if (data->refcount == 0)
                free(data);
            return _ENOMEM;
        }
        pt->data = data;
        pt->offset = ((page - start) << PAGE_BITS) + offset;
        pt->flags = flags;
    }
    mem_changed(mem);
    return 0;
}

int pt_unmap(struct mem *mem, page_t start, pages_t pages) {
    if (!mem_page_range_valid(mem, start, pages))
        return -1;
    for (page_t page = start; page < start + pages; page++)
        if (mem_pt(mem, page) == NULL)
            return -1;
    return pt_unmap_always(mem, start, pages);
}

int pt_unmap_always(struct mem *mem, page_t start, pages_t pages) {
    if (!mem_page_range_valid(mem, start, pages))
        return -1;
    for (page_t page = start; page < start + pages; mem_next_page(mem, &page)) {
        struct pt_entry *pt = mem_pt(mem, page);
        if (pt == NULL)
            continue;
#if ENGINE_JIT
        jit_invalidate_page(mem->mmu.jit, page);
#endif
        struct data *data = pt->data;
        mem_pt_del(mem, page);
        if (--data->refcount == 0) {
            // vdso wasn't allocated with mmap, it's just in our data segment
            if (data->data != vdso_data) {
                int err = munmap(data->data, data->size);
                if (err != 0)
                    die("munmap(%p, %lu) failed: %s", data->data, data->size, strerror(errno));
            }
            if (data->fd != NULL) {
                fd_close(data->fd);
            }
            free(data);
        }
    }
    mem_changed(mem);
    return 0;
}

int pt_map_nothing(struct mem *mem, page_t start, pages_t pages, unsigned flags) {
    if (pages == 0) return 0;
    if (pages > SIZE_MAX / PAGE_SIZE)
        return _ENOMEM;
    void *memory = mmap(NULL, pages * PAGE_SIZE,
            PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
    return pt_map(mem, start, pages, memory, 0, flags | P_ANONYMOUS);
}

int pt_set_flags(struct mem *mem, page_t start, pages_t pages, int flags) {
    if (!mem_page_range_valid(mem, start, pages))
        return _ENOMEM;
    for (page_t page = start; page < start + pages; page++)
        if (mem_pt(mem, page) == NULL)
            return _ENOMEM;
    for (page_t page = start; page < start + pages; page++) {
        struct pt_entry *entry = mem_pt(mem, page);
        int old_flags = entry->flags;
        entry->flags = flags;
        // check if protection is increasing
        if ((flags & ~old_flags) & (P_READ|P_WRITE)) {
            void *data = (char *) entry->data->data + entry->offset;
            // force to be page aligned
            data = (void *) ((uintptr_t) data & ~(real_page_size - 1));
            int prot = PROT_READ;
            if (flags & P_WRITE) prot |= PROT_WRITE;
            if (mprotect(data, real_page_size, prot) < 0)
                return errno_map();
        }
    }
    mem_changed(mem);
    return 0;
}

int pt_copy_on_write(struct mem *src, struct mem *dst, page_t start, page_t pages) {
    if (!mem_page_range_valid(src, start, pages) || !mem_page_range_valid(dst, start, pages))
        return -1;
    mem_ref_cnt_mod(src, 1);
    mem_ref_cnt_mod(dst, 1);
    for (page_t page = start; page < start + pages; mem_next_page(src, &page)) {
        struct pt_entry *entry = mem_pt(src, page);
        if (entry == NULL)
            continue;
        if (pt_unmap_always(dst, page, 1) < 0)
            return -1;
        if (!(entry->flags & P_SHARED))
            entry->flags |= P_COW;
        entry->data->refcount++;
        struct pt_entry *dst_entry = mem_pt_new(dst, page);
        if (dst_entry == NULL) {
            entry->data->refcount--;
            return -1;
        }
        dst_entry->data = entry->data;
        dst_entry->offset = entry->offset;
        dst_entry->flags = entry->flags;
    }
    mem_changed(src);
    mem_changed(dst);
    mem_ref_cnt_mod(src, -1);
    mem_ref_cnt_mod(dst, -1);
    
    return 0;
}

static void mem_changed(struct mem *mem) {
    mem->mmu.changes++;
}

// This version will return NULL instead of making necessary pagetable changes.
// Used by the emulator to avoid deadlocks.
static void *mem_ptr_nofault(struct mem *mem, guest_addr_t addr, int type) {
    struct pt_entry *entry = mem_pt(mem, PAGE(addr));
    if (entry == NULL)
        return NULL;
    if (type == MEM_WRITE && !P_WRITABLE(entry->flags))
        return NULL;
    return entry->data->data + entry->offset + PGOFFSET(addr);
}

void *mem_ptr(struct mem *mem, guest_addr_t addr, int type) {
    void *old_ptr = mem_ptr_nofault(mem, addr, type); // just for an assert

    page_t page = PAGE(addr);
    struct pt_entry *entry = mem_pt(mem, page);

    if (entry == NULL) {
        // page does not exist
        // look to see if the next VM region is willing to grow down
        page_t p = page + 1;
        p = mem_next_mapped_page(mem, p);
        if (p == BAD_PAGE || p >= mem->page_limit)
            return NULL;
        if (!(mem_pt(mem, p)->flags & P_GROWSDOWN))
            return NULL;

        // Changing memory maps must be done with the write lock. But this is
        // called with the read lock.
        // This locking stuff is copy/pasted for all the code in this function
        // which changes memory maps.
        read_to_write_lock(&mem->lock);
        pt_map_nothing(mem, page, 1, P_WRITE | P_GROWSDOWN);
        write_to_read_lock(&mem->lock);

        entry = mem_pt(mem, page);
    }

    if (entry != NULL && (type == MEM_WRITE || type == MEM_WRITE_PTRACE)) {
        // if page is unwritable, well tough luck
        if (type != MEM_WRITE_PTRACE && !(entry->flags & P_WRITE))
            return NULL;
        
        if (type == MEM_WRITE_PTRACE) {
            // TODO: Is P_WRITE really correct? The page shouldn't be writable without ptrace.
            entry->flags |= P_WRITE | P_COW;
        }
#if ENGINE_JIT
        // get rid of any compiled blocks in this page
        jit_invalidate_page(mem->mmu.jit, page);
#endif
        
        // if page is cow, ~~milk~~ copy it
        
        if (entry->flags & P_COW) {
            bool locked_general_lock = false;
            // Some callers, including do_exit() via clear_tid, already hold
            // general_lock. Re-locking it here self-deadlocks while trying to
            // resolve the final COW write into user memory.
            if (current != NULL && !pthread_equal(current->general_lock.owner, pthread_self())) {
                lock(&current->general_lock, 0);  // prevent elf_exec from doing mm_release while we are in flight
                locked_general_lock = true;
            }
            read_to_write_lock(&mem->lock);
            entry = mem_pt(mem, page);
            if (entry == NULL) {
                if (locked_general_lock)
                    unlock(&current->general_lock);
                write_to_read_lock(&mem->lock);
                return NULL;
            }
            if (type != MEM_WRITE_PTRACE && !(entry->flags & P_WRITE)) {
                if (locked_general_lock)
                    unlock(&current->general_lock);
                write_to_read_lock(&mem->lock);
                return NULL;
            }
            if (!(entry->flags & P_COW)) {
                if (locked_general_lock)
                    unlock(&current->general_lock);
                write_to_read_lock(&mem->lock);
                goto done_write_fault;
            }
            void *copy = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
            void *data = (char *) entry->data->data + entry->offset;

            // copy/paste from above
            mem_ref_cnt_mod(mem, 1);
            memcpy(copy, data, PAGE_SIZE);  //mkemkemke  Crashes here a lot when running both the go and parallel make test. 01 June 2022
            mem_ref_cnt_mod(mem, -1);
            pt_map(mem, page, 1, copy, 0, entry->flags &~ P_COW);
            if (locked_general_lock)
                unlock(&current->general_lock);
            write_to_read_lock(&mem->lock);
            
        }
        
    }

done_write_fault:
    void *ptr = mem_ptr_nofault(mem, addr, type);
    assert(old_ptr == NULL || old_ptr == ptr || type == MEM_WRITE_PTRACE);
    return ptr;
}

static void *mem_mmu_translate(struct mmu *mmu, guest_addr_t addr, int type) {
    return mem_ptr_nofault(container_of(mmu, struct mem, mmu), addr, type);
}

static struct mmu_ops mem_mmu_ops = {
    .translate = mem_mmu_translate,
};

int mem_segv_reason(struct mem *mem, guest_addr_t addr) {
    struct pt_entry *pt = mem_pt(mem, PAGE(addr));
    if (pt == NULL)
        return SEGV_MAPERR_;
    return SEGV_ACCERR_;
}

size_t real_page_size;
__attribute__((constructor)) static void get_real_page_size(void) {
    real_page_size = sysconf(_SC_PAGESIZE);
}

void mem_coredump(struct mem *mem, const char *file) {
    int fd = open(file, O_CREAT | O_RDWR | O_TRUNC, 0666);
    if (fd < 0) {
        perror("open");
        return;
    }
    if (ftruncate(fd, 0xffffffff) < 0) {
        perror("ftruncate");
        return;
    }

    int pages = 0;
    for (page_t page = 0; page < mem->page_limit; mem_next_page(mem, &page)) {
        struct pt_entry *entry = mem_pt(mem, page);
        if (entry == NULL)
            continue;
        pages++;
        if (lseek(fd, page << PAGE_BITS, SEEK_SET) < 0) {
            perror("lseek");
            return;
        }
        if (write(fd, entry->data->data, PAGE_SIZE) < 0) {
            perror("write");
            return;
        }
    }
    printk("WARNING: dumped %d pages\n", pages);
    close(fd);
}

void mem_ref_cnt_mod(struct mem *mem, int value) { // value Should only be -1 or 1.  -mke
    // Keep track of how many threads are referencing this task
    if(!doEnableExtraLocking) {// If they want to fly by the seat of their pants...  -mke
        return;
    }
    
    if(mem == NULL) {
            return;
    }
    
    pthread_mutex_lock(&mem->reference.lock);
    
    if(((mem->reference.count + value) < 0)) { // Prevent our unsigned value attempting to go negative.  -mke
        printk("ERROR: Attempt to decrement mem reference count to be negative, ignoring(%d:%d)\n", mem->reference.count, value);
        pthread_mutex_unlock(&mem->reference.lock);
        return;
    }
    
    
    mem->reference.count = mem->reference.count + value;
        
    pthread_mutex_unlock(&mem->reference.lock);
}

int mem_ref_cnt_get(struct mem *mem) {
    pthread_mutex_lock(&mem->reference.lock);
    int cnt = mem->reference.count;
    pthread_mutex_unlock(&mem->reference.lock);
    if((cnt < 0) || ( cnt > 1000)) // Stupid kluge while I fix this brain damage
        cnt = 0;
    return cnt;
}
