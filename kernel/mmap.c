#include <string.h>
#include "debug.h"
#include "kernel/calls.h"
#include "kernel/errno.h"
#include "kernel/fs.h"
#include "kernel/task.h"
#include "fs/fd.h"
#include "emu/memory.h"
#include "kernel/mm.h"
#include "util/sync.h"

extern bool doEnableExtraLocking;
extern struct timespec lock_pause;

static void mem_write_lock_with_pokes(struct mem *mem) {
    atomic_fetch_add_explicit(&mem->quiesce_requested, 1, memory_order_acq_rel);
    for (int attempts = 0; attempts < 64; attempts++) {
        task_poke_shared_mem(current, mem);
        if (trylockw(&mem->lock) == 0)
            return;
        nanosleep(&lock_pause, NULL);
    }

    task_poke_shared_mem(current, mem);
    write_lock(&mem->lock);
}

static void mem_write_unlock_with_pokes(struct mem *mem) {
    write_unlock(&mem->lock);
    atomic_fetch_sub_explicit(&mem->quiesce_requested, 1, memory_order_acq_rel);
}

static bool amd64_vm_failure_trace_enabled(void) {
    return current != NULL && current->abi == GUEST_ABI_AMD64;
}

static void amd64_vm_failure_trace(const char *syscall, qword_t result,
        qword_t a0, qword_t a1, qword_t a2, qword_t a3, qword_t a4, qword_t a5) {
    enum { AMD64_VM_FAILURE_TRACE_BUDGET = 32 };
    static unsigned amd64_vm_failure_trace_count;
    if (!amd64_vm_failure_trace_enabled())
        return;
    if (amd64_vm_failure_trace_count >= AMD64_VM_FAILURE_TRACE_BUDGET)
        return;
    amd64_vm_failure_trace_count++;
    printk("amd64 vm fail: pid=%d comm=%s %s(%#llx, %#llx, %#llx, %#llx, %#llx, %#llx) = %#llx floor=%#llx ceiling=%#llx brk=%#llx start_brk=%#llx\n",
           current->pid, current->comm, syscall,
           (unsigned long long) a0, (unsigned long long) a1,
           (unsigned long long) a2, (unsigned long long) a3,
           (unsigned long long) a4, (unsigned long long) a5,
           (unsigned long long) result,
           (unsigned long long) ((guest_addr_t) current->mem->mmap_floor << PAGE_BITS),
           (unsigned long long) ((guest_addr_t) current->mem->mmap_ceiling << PAGE_BITS),
           (unsigned long long) current->mm->brk,
           (unsigned long long) current->mm->start_brk);
    if (strcmp(syscall, "mmap") == 0 && (sqword_t) a4 >= 0) {
        struct fd *fd = f_get_retain((fd_t) a4);
        if (fd != NULL) {
            char path[MAX_PATH];
            int path_err = generic_getpath(fd, path);
            if (path_err == 0) {
                printk("amd64 vm fail mmap fd: pid=%d fd=%lld path=%s\n",
                       current->pid, (long long) (fd_t) a4, path);
            } else {
                printk("amd64 vm fail mmap fd: pid=%d fd=%lld path_err=%d\n",
                       current->pid, (long long) (fd_t) a4, path_err);
            }
            fd_close(fd);
        }
    }
}

static void mm_apply_abi_layout(struct mm *mm, enum guest_abi abi) {
    struct guest_vm_layout layout = guest_abi_vm_layout(abi);
    mem_set_page_limit(&mm->mem, layout.page_limit);
    mem_set_mmap_window(&mm->mem, layout.mmap_floor, layout.mmap_ceiling);
}

struct mm *mm_new(enum guest_abi abi) {
    struct mm *mm = malloc(sizeof(struct mm));
    if (mm == NULL)
        return NULL;
    mem_init(&mm->mem);
    mm_apply_abi_layout(mm, abi);
    ipc_mm_init(mm);
    mm->start_brk = mm->brk = 0; // should get overwritten by exec
    mm->mlockall_flags = 0;
    mm->exefile = NULL;
    mm->refcount = 1;
    return mm;
}

struct mm *mm_copy(struct mm *mm) {
    struct mm *new_mm = malloc(sizeof(struct mm));
    if (new_mm == NULL)
        return NULL;
    *new_mm = *mm;
    // Fix wrlock_init failing because it thinks it's reinitializing the same lock
    memset(&new_mm->mem.lock, 0, sizeof(new_mm->mem.lock));
    new_mm->refcount = 1;
    mem_init(&new_mm->mem);
    mem_set_page_limit(&new_mm->mem, mm->mem.page_limit);
    mem_set_mmap_window(&new_mm->mem, mm->mem.mmap_floor, mm->mem.mmap_ceiling);
    ipc_mm_init(new_mm);
    fd_retain(new_mm->exefile);
    write_lock(&mm->mem.lock);
    pt_copy_on_write(&mm->mem, &new_mm->mem, 0, mm->mem.page_limit);
    ipc_mm_copy(new_mm, mm);
    write_unlock(&mm->mem.lock);
    return new_mm;
}

void mm_retain(struct mm *mm) {
    mm->refcount++;
}

void mm_release(struct mm *mm) {
    if (--mm->refcount == 0) {
        while (mem_ref_cnt_get(&mm->mem) != 0)
            nanosleep(&lock_pause, NULL);
        if (mm->exefile != NULL)
            fd_close(mm->exefile);

        ipc_mm_release(mm);
        mem_destroy(&mm->mem);
        free(mm);
    }
}

static guest_addr_t do_mmap(guest_addr_t addr, qword_t len, dword_t prot, dword_t flags, fd_t fd_no, qword_t offset) {
    int err;
    pages_t pages = PAGE_ROUND_UP(len);
    if (!pages) return _EINVAL;
    page_t page = 0;
    if (addr != 0 && !guest_abi_range_valid(current->abi, addr, len))
        return _ENOMEM;
    if (addr != 0) {
        if (PGOFFSET(addr) != 0)
            return _EINVAL;
        page = PAGE(addr);
        if (!(flags & MMAP_FIXED) && !pt_is_hole(current->mem, page, pages)) {
            addr = 0;
        }
    }
    if (addr == 0) {
        page = pt_find_hole(current->mem, pages);
        if (page == BAD_PAGE)
            return _ENOMEM;
    }
    qword_t mapped_addr = (qword_t) page << PAGE_BITS;
    if (!guest_abi_range_valid(current->abi, mapped_addr, len))
        return _ENOMEM;
    if (flags & MMAP_SHARED)
        prot |= P_SHARED;

    if (flags & MMAP_ANONYMOUS) {
        if ((err = pt_map_nothing(current->mem, page, pages, prot)) < 0)
            return err;
    } else {
        // fd must be valid
        struct fd *fd = f_get(fd_no);
        if (fd == NULL)
            return _EBADF;
        if (fd->ops->mmap == NULL)
            return _ENODEV;
        if ((err = fd->ops->mmap(fd, current->mem, page, pages, offset, prot, flags)) < 0)
            return err;
        mem_pt(current->mem, page)->data->fd = fd_retain(fd);
        mem_pt(current->mem, page)->data->file_offset = offset;
    }
    return mapped_addr;
}

static guest_addr_t mmap_common_guest(guest_addr_t addr, qword_t len, dword_t prot, dword_t flags, fd_t fd_no, qword_t offset) {
    STRACE("mmap(%#llx, %#llx, 0x%x, 0x%x, %d, %#llx)",
           (unsigned long long) addr, (unsigned long long) len, prot, flags, fd_no,
           (unsigned long long) offset);
    if (len == 0)
        return _EINVAL;
    if (prot & ~P_RWX)
        return _EINVAL;
    if ((flags & MMAP_PRIVATE) && (flags & MMAP_SHARED))
        return _EINVAL;

    mem_write_lock_with_pokes(current->mem);
    guest_addr_t res = do_mmap(addr, len, prot, flags, fd_no, offset);
    mem_write_unlock_with_pokes(current->mem);
    if ((sqword_t) res == _ENOMEM)
        amd64_vm_failure_trace("mmap", res, addr, len, prot, flags, (qword_t) fd_no, offset);
    return res;
}

guest_addr_t sys_mmap_guest(guest_addr_t addr, qword_t len, dword_t prot, dword_t flags, fd_t fd_no, qword_t offset) {
    return mmap_common_guest(addr, len, prot, flags, fd_no, offset);
}

addr_t sys_mmap2(addr_t addr, dword_t len, dword_t prot, dword_t flags, fd_t fd_no, dword_t offset) {
    return (addr_t) mmap_common_guest(addr, len, prot, flags, fd_no, offset << PAGE_BITS);
}

enum membarrier_cmd {
    MEMBARRIER_CMD_QUERY = 0,
    MEMBARRIER_SUPPORTS_ALL = 31, // lies
};

dword_t sys_membarrier(dword_t cmd, dword_t flags, dword_t cpuid) {
    STRACE("membarrier(0x%x, 0x%x, 0x%x)", cmd, flags, cpuid);
    switch (cmd) {
        case MEMBARRIER_CMD_QUERY:
            return MEMBARRIER_SUPPORTS_ALL;
            //errno = ENOSYS;
            //return -1;
        default:
	 __atomic_thread_fence(__ATOMIC_SEQ_CST);
            // __asm__ __volatile__("" : : : "memory");
    }
    return 0;
}

struct mmap_arg_struct {
    dword_t addr, len, prot, flags, fd, offset;
};

addr_t sys_mmap(addr_t args_addr) {
    struct mmap_arg_struct args;
    STRACE("sys_mmap(0x%x)", args_addr);
    if (user_get(args_addr, args))
        return _EFAULT;
    return (addr_t) mmap_common_guest(args.addr, args.len, args.prot, args.flags, args.fd, args.offset);
}

addr_t sys_mmap_amd64(addr_t addr, dword_t len, dword_t prot, dword_t flags, fd_t fd_no, dword_t offset) {
    STRACE("mmap(0x%x, 0x%x, 0x%x, 0x%x, %d, %d) [amd64]", addr, len, prot, flags, fd_no, offset);
    return (addr_t) mmap_common_guest(addr, len, prot, flags, fd_no, offset);
}

int_t sys_munmap_guest(guest_addr_t addr, qword_t len) {
    STRACE("munmap(%#llx, %#llx)", (unsigned long long) addr, (unsigned long long) len);
    if (PGOFFSET(addr) != 0)
        return _EINVAL;
    if (len == 0)
        return _EINVAL;
    
    mem_write_lock_with_pokes(current->mem);
    int err = pt_unmap_always(current->mem, PAGE(addr), PAGE_ROUND_UP(len));
    mem_write_unlock_with_pokes(current->mem);
    
    if (err < 0)
        return _EINVAL;
    return 0;
}

int_t sys_munmap(addr_t addr, uint_t len) {
    return sys_munmap_guest(addr, len);
}

#define MREMAP_MAYMOVE_ 1
#define MREMAP_FIXED_ 2

guest_addr_t sys_mremap_guest(guest_addr_t addr, qword_t old_len, qword_t new_len, dword_t flags) {
    STRACE("mremap(%#llx, %#llx, %#llx, %d)", (unsigned long long) addr,
           (unsigned long long) old_len, (unsigned long long) new_len, flags);
    if (PGOFFSET(addr) != 0)
        return _EINVAL;
    if (flags & ~(MREMAP_MAYMOVE_ | MREMAP_FIXED_))
        return _EINVAL;
    if (flags & MREMAP_FIXED_) {
        FIXME("missing MREMAP_FIXED");
        return _EINVAL;
    }
    pages_t old_pages = PAGE_ROUND_UP(old_len);
    pages_t new_pages = PAGE_ROUND_UP(new_len);
    guest_addr_t res = _ENOMEM;

    mem_write_lock_with_pokes(current->mem);

    // shrinking always works
    if (new_pages <= old_pages) {
        while(task_ref_cnt_get(current, 0) > 1) { // Sometimes this is one.  Figure out if this is OK.  FIXME
            nanosleep(&lock_pause, NULL);
        }
        int err = pt_unmap(current->mem, PAGE(addr) + new_pages, old_pages - new_pages);
        res = err < 0 ? _EFAULT : addr;
        goto out;
    }

    struct pt_entry *entry = mem_pt(current->mem, PAGE(addr));
    if (entry == NULL) {
        res = _EFAULT;
        goto out;
    }
    dword_t pt_flags = entry->flags;
    for (page_t page = PAGE(addr); page < PAGE(addr) + old_pages; page++) {
        entry = mem_pt(current->mem, page);
        if (entry == NULL || entry->flags != pt_flags) {
            res = _EFAULT;
            goto out;
        }
    }
    if (!(pt_flags & P_ANONYMOUS)) {
        FIXME("mremap grow on file mappings");
        res = _EFAULT;
        goto out;
    }
    page_t extra_start = PAGE(addr) + old_pages;
    pages_t extra_pages = new_pages - old_pages;
    bool extra_is_hole = pt_is_hole(current->mem, extra_start, extra_pages);
    if (extra_is_hole) {
        int err = pt_map_nothing(current->mem, extra_start, extra_pages, pt_flags);
        res = err < 0 ? err : addr;
        goto out;
    }

    if (!(flags & MREMAP_MAYMOVE_)) {
        amd64_vm_failure_trace("mremap", _ENOMEM, addr, old_len, new_len, flags, 0, 0);
        res = _ENOMEM;
        goto out;
    }

    page_t new_page = pt_find_hole(current->mem, new_pages);
    if (new_page == BAD_PAGE) {
        amd64_vm_failure_trace("mremap", _ENOMEM, addr, old_len, new_len, flags, 0, 0);
        res = _ENOMEM;
        goto out;
    }
    int err = pt_map_nothing(current->mem, new_page + old_pages, extra_pages, pt_flags);
    if (err == 0) {
        err = pt_move(current->mem, PAGE(addr), new_page, old_pages);
        if (err < 0)
            pt_unmap_always(current->mem, new_page + old_pages, extra_pages);
    }
    if (err < 0) {
        if (err == _ENOMEM)
            amd64_vm_failure_trace("mremap", err, addr, old_len, new_len, flags, 0, 0);
        res = err;
        goto out;
    }
    res = (guest_addr_t) new_page << PAGE_BITS;

out:
    mem_write_unlock_with_pokes(current->mem);
    return res;
}

int_t sys_mremap(addr_t addr, dword_t old_len, dword_t new_len, dword_t flags) {
    return (int_t) sys_mremap_guest(addr, old_len, new_len, flags);
}

int_t sys_mprotect_guest(guest_addr_t addr, qword_t len, int_t prot) {
    STRACE("mprotect(%#llx, %#llx, 0x%x)", (unsigned long long) addr,
           (unsigned long long) len, prot);
    if (PGOFFSET(addr) != 0)
        return _EINVAL;
    if (prot & ~P_RWX)
        return _EINVAL;
    pages_t pages = PAGE_ROUND_UP(len);
    mem_write_lock_with_pokes(current->mem);
    int err = pt_set_flags(current->mem, PAGE(addr), pages, prot);
    mem_write_unlock_with_pokes(current->mem);
    if (err == _ENOMEM)
        amd64_vm_failure_trace("mprotect", err, addr, len, prot, 0, 0, 0);
    return err;
}

int_t sys_mprotect(addr_t addr, uint_t len, int_t prot) {
    return sys_mprotect_guest(addr, len, prot);
}

dword_t sys_madvise_guest(guest_addr_t UNUSED(addr), qword_t UNUSED(len), dword_t UNUSED(advice)) {
    // portable applications should not rely on linux's destructive semantics for MADV_DONTNEED.
    return 0;
}

dword_t sys_madvise(addr_t addr, dword_t len, dword_t advice) {
    return sys_madvise_guest(addr, len, advice);
}

dword_t sys_mincore_guest(guest_addr_t addr, qword_t len, guest_addr_t vec_addr) {
    STRACE("mincore(%#llx, %#llx, %#llx)",
           (unsigned long long) addr,
           (unsigned long long) len,
           (unsigned long long) vec_addr);
    if (PGOFFSET(addr) != 0)
        return _EINVAL;
    if (len == 0)
        return _EINVAL;

    pages_t pages = PAGE_ROUND_UP(len);
    page_t start = PAGE(addr);

    // Collect the result before touching guest memory: user_write re-enters
    // mem_read_lock_quiesce_aware, and taking a nested read lock here
    // deadlocks against a pending writer's quiesce request (the inner lock
    // waits for quiesce_requested to drop while the writer waits for the
    // outer read lock to release).
    byte_t *vec = malloc(pages);
    if (vec == NULL)
        return _ENOMEM;
    mem_read_lock_quiesce_aware(current->mem);
    for (pages_t i = 0; i < pages; i++) {
        struct pt_entry *entry = mem_pt(current->mem, start + i);
        if (entry == NULL) {
            mem_read_unlock_quiesce_aware(current->mem);
            free(vec);
            return _ENOMEM;
        }
        vec[i] = 1;
    }
    mem_read_unlock_quiesce_aware(current->mem);
    int err = user_write(vec_addr, vec, pages) ? _EFAULT : 0;
    free(vec);
    return err;
}

dword_t sys_mincore(addr_t addr, dword_t len, addr_t vec_addr) {
    return sys_mincore_guest(addr, len, vec_addr);
}

dword_t sys_mbind(addr_t UNUSED(addr), dword_t UNUSED(len), int_t UNUSED(mode),
        addr_t UNUSED(nodemask), dword_t UNUSED(maxnode), uint_t UNUSED(flags)) {
    return 0;
}

dword_t sys_mbind_guest(guest_addr_t UNUSED(addr), qword_t UNUSED(len), int_t UNUSED(mode),
        guest_addr_t UNUSED(nodemask), qword_t UNUSED(maxnode), uint_t UNUSED(flags)) {
    return 0;
}

long sys_get_mempolicy(int *UNUSED(mode), unsigned long *UNUSED(nodemask), unsigned long UNUSED(maxnode), void *UNUSED(addr), unsigned long UNUSED(flags)) {
    return 0;
}

long sys_get_mempolicy_guest(guest_addr_t UNUSED(mode_addr), guest_addr_t UNUSED(nodemask_addr),
        qword_t UNUSED(maxnode), guest_addr_t UNUSED(addr), qword_t UNUSED(flags)) {
    return 0;
}

long sys_set_mempolicy(int UNUSED(mode), const unsigned long *UNUSED(nodemask), unsigned long UNUSED(maxnode)) {
    return 0;
}

long sys_set_mempolicy_guest(int UNUSED(mode), guest_addr_t UNUSED(nodemask_addr), qword_t UNUSED(maxnode)) {
    return 0;
}

int_t sys_mlock_guest(guest_addr_t UNUSED(addr), qword_t UNUSED(len)) {
    return 0;
}

int_t sys_mlock(addr_t addr, dword_t len) {
    return sys_mlock_guest(addr, len);
}

int_t sys_munlock_guest(guest_addr_t UNUSED(addr), qword_t UNUSED(len)) {
    return 0;
}

int_t sys_munlock(addr_t addr, dword_t len) {
    return sys_munlock_guest(addr, len);
}

#define MCL_CURRENT_ 0x1
#define MCL_FUTURE_ 0x2
#define MCL_ONFAULT_ 0x4

int_t sys_mlockall_guest(qword_t flags) {
    qword_t supported = MCL_CURRENT_ | MCL_FUTURE_ | MCL_ONFAULT_;
    if ((flags & ~supported) != 0)
        return _EINVAL;
    if ((flags & (MCL_CURRENT_ | MCL_FUTURE_)) == 0)
        return _EINVAL;
    if ((flags & MCL_ONFAULT_) != 0 && (flags & (MCL_CURRENT_ | MCL_FUTURE_)) == 0)
        return _EINVAL;
    current->mm->mlockall_flags = (dword_t) flags;
    return 0;
}

int_t sys_mlockall(dword_t flags) {
    return sys_mlockall_guest(flags);
}

int_t sys_munlockall_guest(void) {
    current->mm->mlockall_flags = 0;
    return 0;
}

int_t sys_munlockall(void) {
    return sys_munlockall_guest();
}

int_t sys_msync_guest(guest_addr_t UNUSED(addr), qword_t UNUSED(len), int_t UNUSED(flags)) {
    return 0;
}

int_t sys_msync(addr_t addr, dword_t len, int_t flags) {
    return sys_msync_guest(addr, len, flags);
}

guest_addr_t sys_brk_guest(guest_addr_t new_brk) {
    STRACE("brk(%#llx)", (unsigned long long) new_brk);
    struct mm *mm = current->mm;
    bool expand_failed = false;

    mem_write_lock_with_pokes(&mm->mem);
    if (new_brk < mm->start_brk)
        goto out;
    guest_addr_t old_brk = mm->brk;

    if (new_brk > old_brk) {
        // expand heap: map region from old_brk to new_brk
        // round up because of the definition of brk: "the first location after the end of the uninitialized data segment." (brk(2))
        // if the brk is 0x2000, page 0x2000 shouldn't be mapped, but it should be if the brk is 0x2001.
        page_t start = PAGE_ROUND_UP(old_brk);
        pages_t size = PAGE_ROUND_UP(new_brk) - PAGE_ROUND_UP(old_brk);
        if (!pt_is_hole(&mm->mem, start, size)) {
            expand_failed = true;
            goto out;
        }
        int err = pt_map_nothing(&mm->mem, start, size, P_WRITE);
        if (err < 0) {
            expand_failed = true;
            goto out;
        }
    } else if (new_brk < old_brk) {
        // shrink heap: unmap region from new_brk to old_brk
        // first page to unmap is PAGE(new_brk)
        // last page to unmap is PAGE(old_brk)
        pt_unmap_always(&mm->mem, PAGE(new_brk), PAGE(old_brk) - PAGE(new_brk));
    }

    mm->brk = new_brk;
out:;
    guest_addr_t brk = mm->brk;
    mem_write_unlock_with_pokes(&mm->mem);
    if (expand_failed)
        amd64_vm_failure_trace("brk", brk, new_brk, old_brk, 0, 0, 0, 0);
    return brk;
}

addr_t sys_brk(addr_t new_brk) {
    return (addr_t) sys_brk_guest(new_brk);
}
