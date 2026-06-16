#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include "kernel/calls.h"
#include "kernel/mm.h"

#ifndef IOV_MAX
#define IOV_MAX 1024 // glibc only exposes IOV_MAX under _XOPEN_SOURCE
#endif

extern bool doEnableExtraLocking;
extern pthread_mutex_t extra_lock;

#define HTOP_RBX_FIELD_ABS_ADDR ((guest_addr_t) 0xf7f019e0u)
#define HTOP_RBX_FIELD_SIZE 8

static inline bool htop_watch_intersects(guest_addr_t addr, size_t count) {
    if (count == 0)
        return false;
    qword_t start = addr;
    qword_t end = start + count;
    qword_t watch_start = HTOP_RBX_FIELD_ABS_ADDR;
    qword_t watch_end = watch_start + HTOP_RBX_FIELD_SIZE;
    return start < watch_end && end > watch_start;
}

static inline void trace_htop_user_write(struct task *task, struct mem *mem,
        guest_addr_t addr, const void *buf, size_t count, bool ptrace) {
    static int enabled = -1;
    if (enabled < 0)
        enabled = getenv("ISH_TRACE_HTOP_USER_WRITE") != NULL ? 1 : 0;
    if (!enabled)
        return;
    if (task == NULL || strcmp(task->comm, "htop") != 0)
        return;
    if (!htop_watch_intersects(addr, count))
        return;

    uint8_t field[HTOP_RBX_FIELD_SIZE] = {};
    bool have_field = false;
    void *field_ptr = mem_ptr(mem, HTOP_RBX_FIELD_ABS_ADDR,
                              ptrace ? MEM_READ : MEM_READ);
    if (field_ptr != NULL) {
        memcpy(field, field_ptr, sizeof(field));
        have_field = true;
    }

    uint64_t observed = 0;
    size_t observed_size = count < sizeof(observed) ? count : sizeof(observed);
    memcpy(&observed, buf, observed_size);

    printk("htop user_write: addr=%#x count=%zu ptrace=%d value=%#llx\n",
           addr, count, ptrace, (unsigned long long) observed);
    if (have_field) {
        printk("htop user_write field: %02x %02x %02x %02x %02x %02x %02x %02x\n",
               field[0], field[1], field[2], field[3],
               field[4], field[5], field[6], field[7]);
    }
}

struct task_mem_read_handle {
    struct mm *mm;
    struct mem *mem;
};

static struct mem *task_mem_read_lock(struct task *task, struct task_mem_read_handle *handle) {
    struct mem *mem;
    handle->mm = NULL;
    handle->mem = NULL;
    if (task == current) {
        mem = task->mem;
        if (mem != NULL) {
            mem_read_lock_quiesce_aware(mem);
            handle->mem = mem;
        }
        return mem;
    }
    lock(&task->general_lock, 0);
    if (task->mm != NULL) {
        handle->mm = task->mm;
        mm_retain(handle->mm);
        mem = &handle->mm->mem;
        mem_read_lock_quiesce_aware(mem);
        handle->mem = mem;
    } else {
        mem = NULL;
    }
    unlock(&task->general_lock);
    return mem;
}

static void task_mem_read_unlock(struct task_mem_read_handle *handle) {
    if (handle->mem != NULL)
        mem_read_unlock_quiesce_aware(handle->mem);
    if (handle->mm != NULL)
        mm_release(handle->mm);
}

static bool user_range_valid_mem(struct task *task, struct mem *mem, guest_addr_t addr, size_t count) {
    if (!guest_abi_range_valid(task->abi, addr, count))
        return false;
    if (count == 0)
        return true;
    qword_t last = (qword_t) addr + count - 1;
    return PAGE(last) < mem->page_limit;
}

static int __user_read_task_mem(struct task *task, struct mem *mem, guest_addr_t addr, void *buf, size_t count) {
    if (!user_range_valid_mem(task, mem, addr, count))
        return 1;
    char *cbuf = (char *) buf;
    guest_addr_t p = addr;
    qword_t end = (qword_t) addr + count;
    while ((qword_t) p < end) {
        qword_t chunk_end = ((qword_t) PAGE(p) + 1) << PAGE_BITS;
        if (chunk_end > end)
            chunk_end = end;
  
        const char *ptr = mem_ptr(mem, p, MEM_READ);
        
        if (ptr == NULL)
            return 1;
        memcpy(&cbuf[p - addr], ptr, chunk_end - p);
        p = (guest_addr_t) chunk_end;
    }
    return 0;
}

static int __user_write_task_mem(struct task *task, struct mem *mem, guest_addr_t addr, const void *buf, size_t count, bool ptrace) {
    if (!user_range_valid_mem(task, mem, addr, count))
        return 1;
    const char *cbuf = (const char *) buf;
    guest_addr_t p = addr;
    qword_t end = (qword_t) addr + count;
    while ((qword_t) p < end) {
        qword_t chunk_end = ((qword_t) PAGE(p) + 1) << PAGE_BITS;
        if (chunk_end > end)
            chunk_end = end;
        char *ptr = mem_ptr(mem, p, ptrace ? MEM_WRITE_PTRACE : MEM_WRITE);
        if (ptr == NULL)
            return 1;
        trace_htop_user_write(task, mem, p, &cbuf[p - addr], chunk_end - p, ptrace);
        memcpy(ptr, &cbuf[p - addr], chunk_end - p);
        p = (guest_addr_t) chunk_end;
    }
    return 0;
}

int user_read_task(struct task *task, guest_addr_t addr, void *buf, size_t count) {
    struct task_mem_read_handle handle;
    struct mem *mem = task_mem_read_lock(task, &handle);
    if (mem == NULL)
        return 1;
    int res = __user_read_task_mem(task, mem, addr, buf, count);
    task_mem_read_unlock(&handle);
    return res;
}

int user_read_task_mem(struct task *task, struct mem *mem, guest_addr_t addr, void *buf, size_t count) {
    if (mem == NULL)
        return 1;
    mem_read_lock_quiesce_aware(mem);
    int res = __user_read_task_mem(task, mem, addr, buf, count);
    mem_read_unlock_quiesce_aware(mem);
    return res;
}

int user_read(guest_addr_t addr, void *buf, size_t count) {
    return user_read_task(current, addr, buf, count);
}

static int user_write_task_mem_internal(struct task *task, struct mem *mem, guest_addr_t addr,
                                        const void *buf, size_t count, bool ptrace) {
    if (mem == NULL)
        return 1;
    mem_read_lock_quiesce_aware(mem);
    int res = __user_write_task_mem(task, mem, addr, buf, count, ptrace);
    mem_read_unlock_quiesce_aware(mem);
    return res;
}

int user_write_task_mem(struct task *task, struct mem *mem, guest_addr_t addr, const void *buf, size_t count) {
    return user_write_task_mem_internal(task, mem, addr, buf, count, false);
}

int user_write_task_ptrace_mem(struct task *task, struct mem *mem, guest_addr_t addr, const void *buf, size_t count) {
    return user_write_task_mem_internal(task, mem, addr, buf, count, true);
}

int user_write_task(struct task *task, guest_addr_t addr, const void *buf, size_t count) {
    struct task_mem_read_handle handle;
    struct mem *mem = task_mem_read_lock(task, &handle);
    if (mem == NULL)
        return 1;
    int res = __user_write_task_mem(task, mem, addr, buf, count, false);
    task_mem_read_unlock(&handle);
    return res;
}

int user_write_task_ptrace(struct task *task, guest_addr_t addr, const void *buf, size_t count) {
    struct task_mem_read_handle handle;
    struct mem *mem = task_mem_read_lock(task, &handle);
    if (mem == NULL)
        return 1;
    int res = __user_write_task_mem(task, mem, addr, buf, count, true);
    task_mem_read_unlock(&handle);
    return res;
}

int user_write(guest_addr_t addr, const void *buf, size_t count) {
    return user_write_task(current, addr, buf, count);
}

int user_read_string(guest_addr_t addr, char *buf, size_t max) {
    if (addr == 0)
        return 1;
    if (max == 0)
        return 1;
    if (!guest_abi_addr_valid(current->abi, addr))
        return 1;
    struct task_mem_read_handle handle;
    struct mem *mem = task_mem_read_lock(current, &handle);
    if (mem == NULL)
        return 1;
    size_t i = 0;
    while (i < max) {
        if (!guest_abi_range_valid(current->abi, (qword_t) addr + i, 1)) {
            task_mem_read_unlock(&handle);
            return 1;
        }
        if (__user_read_task_mem(current, mem, addr + i, &buf[i], sizeof(buf[i]))) {
            task_mem_read_unlock(&handle);
            return 1;
        }
        if (buf[i] == '\0')
            break;
        i++;
    }
    task_mem_read_unlock(&handle);
    if (i == max || buf[i] != '\0')
        return 1;
    return 0;
}

int user_write_string(guest_addr_t addr, const char *buf) {
    if (addr == 0) {
        return 1;
    }
    if (!guest_abi_addr_valid(current->abi, addr))
        return 1;
    struct task_mem_read_handle handle;
    struct mem *mem = task_mem_read_lock(current, &handle);
    if (mem == NULL)
        return 1;
    size_t i = 0;
    do {
        if (!guest_abi_range_valid(current->abi, (qword_t) addr + i, 1)) {
            task_mem_read_unlock(&handle);
            return 1;
        }
        if (__user_write_task_mem(current, mem, addr + i, &buf[i], sizeof(buf[i]), false)) {
            task_mem_read_unlock(&handle);
            return 1;
        }
        i++;
    } while (buf[i - 1] != '\0');
    task_mem_read_unlock(&handle);
    return 0;
}

struct guest_iovec_ *user_read_iovecs_abi(struct task *task, enum guest_abi abi, guest_addr_t iov_addr, dword_t iov_count) {
    if (iov_count == 0)
        return NULL;
    if (iov_count > IOV_MAX)
        return ERR_PTR(_EINVAL);

    size_t guest_size;
    if (abi == GUEST_ABI_AMD64) {
        guest_size = sizeof(struct amd64_iovec_);
    } else {
        guest_size = sizeof(struct i386_iovec_);
    }

    size_t raw_size = guest_size * iov_count;
    void *raw_iov = malloc(raw_size);
    if (raw_iov == NULL)
        return ERR_PTR(_ENOMEM);
    if (user_read_task(task, iov_addr, raw_iov, raw_size)) {
        free(raw_iov);
        return ERR_PTR(_EFAULT);
    }

    struct guest_iovec_ *iov = malloc(sizeof(*iov) * iov_count);
    if (iov == NULL) {
        free(raw_iov);
        return ERR_PTR(_ENOMEM);
    }

    for (dword_t i = 0; i < iov_count; i++) {
        qword_t base;
        qword_t len;
        if (abi == GUEST_ABI_AMD64) {
            struct amd64_iovec_ *amd64_iov = raw_iov;
            base = amd64_iov[i].base;
            len = amd64_iov[i].len;
        } else {
            struct i386_iovec_ *i386_iov = raw_iov;
            base = i386_iov[i].base;
            len = i386_iov[i].len;
        }
        if (!guest_abi_addr_valid(abi, base) || len > SIZE_MAX) {
            free(raw_iov);
            free(iov);
            return ERR_PTR(_EINVAL);
        }
        iov[i] = (struct guest_iovec_) {
            .base = base,
            .len = (size_t) len,
        };
    }
    free(raw_iov);
    return iov;
}

dword_t sys_process_vm_readv_guest(pid_t_ pid, guest_addr_t local_iov_addr, dword_t liovcnt,
                             guest_addr_t remote_iov_addr, dword_t riovcnt, dword_t flags) {
    if (flags != 0)
        return _EINVAL;

    struct task *task = pid_get_task_ref(pid);
    if (task == NULL)
        return _ESRCH;
    if (task != current && task->parent != current && current->parent != task) {
        task_ref_cnt_mod(task, -1);
        return _EPERM;
    }

    struct guest_iovec_ *local_iov = user_read_iovecs_abi(current, current->abi, local_iov_addr, liovcnt);
    if (IS_ERR(local_iov)) {
        task_ref_cnt_mod(task, -1);
        return PTR_ERR(local_iov);
    }
    struct guest_iovec_ *remote_iov = user_read_iovecs_abi(current, current->abi, remote_iov_addr, riovcnt);
    if (IS_ERR(remote_iov)) {
        free(local_iov);
        task_ref_cnt_mod(task, -1);
        return PTR_ERR(remote_iov);
    }

    dword_t local_index = 0, remote_index = 0;
    size_t local_off = 0, remote_off = 0;
    dword_t total = 0;

    while (local_index < liovcnt && remote_index < riovcnt) {
        while (local_index < liovcnt && local_iov[local_index].len == local_off) {
            local_index++;
            local_off = 0;
        }
        while (remote_index < riovcnt && remote_iov[remote_index].len == remote_off) {
            remote_index++;
            remote_off = 0;
        }
        if (local_index >= liovcnt || remote_index >= riovcnt)
            break;

        size_t local_left = local_iov[local_index].len - local_off;
        size_t remote_left = remote_iov[remote_index].len - remote_off;
        size_t chunk = local_left < remote_left ? local_left : remote_left;
        if (chunk == 0)
            break;

        char buf[4096];
        size_t done = 0;
        while (done < chunk) {
            size_t step = chunk - done;
            if (step > sizeof(buf))
                step = sizeof(buf);
            if (user_read_task(task, remote_iov[remote_index].base + remote_off + done, buf, step)) {
                free(local_iov);
                free(remote_iov);
                return total ? total : _EFAULT;
            }
            if (user_write(local_iov[local_index].base + local_off + done, buf, step)) {
                free(local_iov);
                free(remote_iov);
                return total ? total : _EFAULT;
            }
            done += step;
            total += step;
        }

        local_off += chunk;
        remote_off += chunk;
    }

    free(local_iov);
    free(remote_iov);
    task_ref_cnt_mod(task, -1);
    return total;
}

dword_t sys_process_vm_readv(pid_t_ pid, addr_t local_iov_addr, dword_t liovcnt,
                             addr_t remote_iov_addr, dword_t riovcnt, dword_t flags) {
    return sys_process_vm_readv_guest(pid, local_iov_addr, liovcnt, remote_iov_addr, riovcnt, flags);
}
