#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "kernel/calls.h"
#include "kernel/errno.h"
#include "kernel/mm.h"
#include "kernel/task.h"
#include "emu/memory.h"
#include "util/list.h"
#include "util/sync.h"

#define IPCOP_SHMAT_   21
#define IPCOP_SHMDT_   22
#define IPCOP_SHMGET_  23
#define IPCOP_SHMCTL_  24

#define IPC_PRIVATE_ 0
#define IPC_CREAT_   01000
#define IPC_EXCL_    02000

#define IPC_RMID_    0
#define IPC_SET_     1
#define IPC_STAT_    2
#define IPC_64_      0x100

#define SHM_RDONLY_  010000
#define SHM_RND_     020000

struct ipc_perm_ {
    dword_t key;
    uid_t_ uid;
    uid_t_ gid;
    uid_t_ cuid;
    uid_t_ cgid;
    word_t mode;
    word_t __pad1;
    word_t seq;
    word_t __pad2;
    dword_t __unused1;
    dword_t __unused2;
};

struct shmid_ds_ {
    struct ipc_perm_ shm_perm;
    dword_t shm_segsz;
    time_t_ shm_atime;
    dword_t __unused1;
    time_t_ shm_dtime;
    dword_t __unused2;
    time_t_ shm_ctime;
    dword_t __unused3;
    pid_t_ shm_cpid;
    pid_t_ shm_lpid;
    dword_t shm_nattch;
    dword_t __unused4;
    dword_t __unused5;
};

struct shm_segment {
    struct list shm_segments;
    dword_t key;
    int id;
    size_t size;
    size_t alloc_size;
    pages_t pages;
    int fd;
    mode_t_ mode;
    uid_t_ uid;
    uid_t_ gid;
    uid_t_ cuid;
    uid_t_ cgid;
    pid_t_ cpid;
    pid_t_ lpid;
    time_t_ atime;
    time_t_ dtime;
    time_t_ ctime;
    unsigned nattch;
    bool removed;
};

struct shm_region {
    struct list mm_regions;
    struct shm_segment *segment;
    guest_addr_t addr;
    pages_t pages;
};

static struct list shm_segments = LIST_INITIALIZER(shm_segments);
static lock_t shm_lock = LOCK_INITIALIZER;
static int shm_next_id = 1;

static time_t_ ipc_now(void) {
    return (time_t_) sys_time(0);
}

static void shm_segment_maybe_destroy(struct shm_segment *segment) {
    if (segment == NULL || !segment->removed || segment->nattch != 0)
        return;
    list_remove(&segment->shm_segments);
    close(segment->fd);
    free(segment);
}

static struct shm_segment *shm_segment_find_by_key(dword_t key) {
    struct shm_segment *segment;
    list_for_each_entry(&shm_segments, segment, shm_segments) {
        if (!segment->removed && segment->key == key)
            return segment;
    }
    return NULL;
}

static struct shm_segment *shm_segment_find_by_id(int id) {
    struct shm_segment *segment;
    list_for_each_entry(&shm_segments, segment, shm_segments) {
        if (segment->id == id)
            return segment;
    }
    return NULL;
}

static int shm_backing_create(size_t size) {
    const char *tmpdir = getenv("TMPDIR");
    if (tmpdir == NULL || tmpdir[0] == '\0')
        tmpdir = "/tmp";
    bool has_trailing_slash = tmpdir[strlen(tmpdir) - 1] == '/';
    size_t path_len = strlen(tmpdir) + sizeof("ish-shm.XXXXXX") + (has_trailing_slash ? 0 : 1);
    char *path = malloc(path_len);
    if (path == NULL)
        return -1;
    snprintf(path, path_len, has_trailing_slash ? "%sish-shm.XXXXXX" : "%s/ish-shm.XXXXXX", tmpdir);
    int fd = mkstemp(path);
    if (fd < 0)
        goto out;
    unlink(path);
    if (ftruncate(fd, (off_t) size) < 0) {
        close(fd);
        fd = -1;
    }
out:
    free(path);
    if (fd < 0)
        return -1;
    return fd;
}

static int shmget_internal(dword_t key, size_t size, int flags) {
    lock(&shm_lock, 0);
    struct shm_segment *segment = key == IPC_PRIVATE_ ? NULL : shm_segment_find_by_key(key);
    if (segment != NULL) {
        if ((flags & IPC_CREAT_) && (flags & IPC_EXCL_)) {
            unlock(&shm_lock);
            return _EEXIST;
        }
        if (size != 0 && size > segment->size) {
            unlock(&shm_lock);
            return _EINVAL;
        }
        int id = segment->id;
        unlock(&shm_lock);
        return id;
    }
    if (!(flags & IPC_CREAT_) && key != IPC_PRIVATE_) {
        unlock(&shm_lock);
        return _ENOENT;
    }
    if (size == 0) {
        unlock(&shm_lock);
        return _EINVAL;
    }

    pages_t pages = PAGE_ROUND_UP(size);
    size_t rounded = pages * PAGE_SIZE;
    int fd = shm_backing_create(rounded);
    if (fd < 0) {
        int err = errno_map();
        unlock(&shm_lock);
        return err;
    }

    segment = calloc(1, sizeof(*segment));
    if (segment == NULL) {
        close(fd);
        unlock(&shm_lock);
        return _ENOMEM;
    }
    *segment = (struct shm_segment) {
        .key = key,
        .id = shm_next_id++,
        .size = size,
        .alloc_size = rounded,
        .pages = pages,
        .fd = fd,
        .mode = flags & 0777,
        .uid = current->euid,
        .gid = current->egid,
        .cuid = current->euid,
        .cgid = current->egid,
        .cpid = current->pid,
        .lpid = current->pid,
        .ctime = ipc_now(),
    };
    list_init(&segment->shm_segments);
    list_add_tail(&shm_segments, &segment->shm_segments);
    int id = segment->id;
    unlock(&shm_lock);
    return id;
}

static guest_addr_t shm_region_attach(struct mm *mm, struct shm_segment *segment, guest_addr_t attach_addr, int shmflg) {
    int prot = PROT_READ;
    unsigned mem_flags = P_READ | P_SHARED;
    if (!(shmflg & SHM_RDONLY_)) {
        prot |= PROT_WRITE;
        mem_flags |= P_WRITE;
    }

    void *mapping = mmap(NULL, segment->alloc_size, prot, MAP_SHARED, segment->fd, 0);
    if (mapping == MAP_FAILED)
        return (guest_addr_t) errno_map();

    page_t page = 0;
    if (attach_addr != 0) {
        if (shmflg & SHM_RND_)
            attach_addr = BYTES_ROUND_DOWN(attach_addr);
        else if (PGOFFSET(attach_addr) != 0) {
            munmap(mapping, segment->alloc_size);
            return (guest_addr_t) _EINVAL;
        }
        page = PAGE(attach_addr);
    }

    write_lock(&mm->mem.lock);
    if (attach_addr != 0) {
        page = PAGE(attach_addr);
        if (!pt_is_hole(&mm->mem, page, segment->pages)) {
            write_unlock(&mm->mem.lock);
            munmap(mapping, segment->alloc_size);
            return (guest_addr_t) _EINVAL;
        }
    } else {
        page = pt_find_hole(&mm->mem, segment->pages);
        if (page == BAD_PAGE) {
            write_unlock(&mm->mem.lock);
            munmap(mapping, segment->alloc_size);
            return (guest_addr_t) _ENOMEM;
        }
        attach_addr = (guest_addr_t) page << PAGE_BITS;
    }

    int err = pt_map(&mm->mem, page, segment->pages, mapping, 0, mem_flags);
    if (err < 0) {
        write_unlock(&mm->mem.lock);
        munmap(mapping, segment->alloc_size);
        return (guest_addr_t) err;
    }

    struct shm_region *region = malloc(sizeof(*region));
    if (region == NULL) {
        pt_unmap_always(&mm->mem, page, segment->pages);
        write_unlock(&mm->mem.lock);
        return (guest_addr_t) _ENOMEM;
    }
    *region = (struct shm_region) {
        .segment = segment,
        .addr = attach_addr,
        .pages = segment->pages,
    };
    list_init(&region->mm_regions);
    list_add_tail(&mm->shm_regions, &region->mm_regions);
    write_unlock(&mm->mem.lock);

    lock(&shm_lock, 0);
    segment->nattch++;
    segment->lpid = current->pid;
    segment->atime = ipc_now();
    unlock(&shm_lock);

    return attach_addr;
}

static guest_addr_t shmat_internal(int id, guest_addr_t shmaddr, int shmflg) {
    lock(&shm_lock, 0);
    struct shm_segment *segment = shm_segment_find_by_id(id);
    unlock(&shm_lock);
    if (segment == NULL || segment->removed)
        return (guest_addr_t) _EINVAL;
    return shm_region_attach(current->mm, segment, shmaddr, shmflg);
}

static int shmdt_internal(struct mm *mm, guest_addr_t addr, pid_t_ lpid, bool from_release) {
    write_lock(&mm->mem.lock);
    struct shm_region *region, *tmp;
    list_for_each_entry_safe(&mm->shm_regions, region, tmp, mm_regions) {
        if (region->addr != addr)
            continue;
        pt_unmap_always(&mm->mem, PAGE(region->addr), region->pages);
        list_remove(&region->mm_regions);
        struct shm_segment *segment = region->segment;
        free(region);
        write_unlock(&mm->mem.lock);

        lock(&shm_lock, 0);
        if (segment->nattch != 0)
            segment->nattch--;
        segment->lpid = lpid;
        segment->dtime = ipc_now();
        shm_segment_maybe_destroy(segment);
        unlock(&shm_lock);
        return 0;
    }
    write_unlock(&mm->mem.lock);
    return from_release ? 0 : _EINVAL;
}

static int shmctl_internal(int id, int cmd, addr_t ptr) {
    int cmd_base = cmd & ~IPC_64_;
    lock(&shm_lock, 0);
    struct shm_segment *segment = shm_segment_find_by_id(id);
    if (segment == NULL) {
        unlock(&shm_lock);
        return _EINVAL;
    }

    if (cmd_base == IPC_RMID_) {
        segment->removed = true;
        shm_segment_maybe_destroy(segment);
        unlock(&shm_lock);
        return 0;
    }

    if (cmd_base == IPC_SET_) {
        struct shmid_ds_ info;
        unlock(&shm_lock);
        if (ptr == 0)
            return _EFAULT;
        if (user_read(ptr, &info, sizeof(info)))
            return _EFAULT;
        lock(&shm_lock, 0);
        segment = shm_segment_find_by_id(id);
        if (segment == NULL) {
            unlock(&shm_lock);
            return _EINVAL;
        }
        segment->uid = info.shm_perm.uid;
        segment->gid = info.shm_perm.gid;
        segment->mode = info.shm_perm.mode & 0777;
        segment->ctime = ipc_now();
        unlock(&shm_lock);
        return 0;
    }

    if (cmd_base == IPC_STAT_) {
        struct shmid_ds_ info = {
            .shm_perm = {
                .key = segment->key,
                .uid = segment->uid,
                .gid = segment->gid,
                .cuid = segment->cuid,
                .cgid = segment->cgid,
                .mode = segment->mode,
                .seq = 0,
            },
            .shm_segsz = segment->size,
            .shm_atime = segment->atime,
            .shm_dtime = segment->dtime,
            .shm_ctime = segment->ctime,
            .shm_cpid = segment->cpid,
            .shm_lpid = segment->lpid,
            .shm_nattch = segment->nattch,
        };
        unlock(&shm_lock);
        if (ptr == 0)
            return _EFAULT;
        if (user_write(ptr, &info, sizeof(info)))
            return _EFAULT;
        return 0;
    }

    unlock(&shm_lock);
    return _EINVAL;
}

void ipc_mm_init(struct mm *mm) {
    list_init(&mm->shm_regions);
}

void ipc_mm_copy(struct mm *dst, struct mm *src) {
    struct shm_region *region;
    list_for_each_entry(&src->shm_regions, region, mm_regions) {
        struct shm_region *copy = malloc(sizeof(*copy));
        if (copy == NULL)
            continue;
        *copy = (struct shm_region) {
            .segment = region->segment,
            .addr = region->addr,
            .pages = region->pages,
        };
        list_init(&copy->mm_regions);
        list_add_tail(&dst->shm_regions, &copy->mm_regions);

        lock(&shm_lock, 0);
        copy->segment->nattch++;
        unlock(&shm_lock);
    }
}

void ipc_mm_release(struct mm *mm) {
    struct shm_region *region, *tmp;
    list_for_each_entry_safe(&mm->shm_regions, region, tmp, mm_regions) {
        guest_addr_t addr = region->addr;
        shmdt_internal(mm, addr, current ? current->pid : 0, true);
    }
}

int_t sys_ipc(uint_t call, int_t first, int_t second, int_t third, addr_t ptr, int_t fifth) {
    STRACE("ipc(%u, %d, %d, %d, %#x, %d)", call, first, second, third, ptr, fifth);
    use(fifth);

    uint_t version = call >> 16;
    uint_t op = call & 0xffff;
    use(version);

    switch (op) {
        case IPCOP_SHMGET_:
            return shmget_internal((dword_t) first, (size_t) second, third);
        case IPCOP_SHMAT_: {
            guest_addr_t out_addr = shmat_internal(first, ptr, second);
            if (IS_ERR((void *) (uintptr_t) out_addr))
                return (int_t) PTR_ERR((void *) (uintptr_t) out_addr);
            if (user_put((addr_t) third, out_addr))
                return _EFAULT;
            return 0;
        }
        case IPCOP_SHMDT_:
            return shmdt_internal(current->mm, ptr, current->pid, false);
        case IPCOP_SHMCTL_:
            return shmctl_internal(first, second, ptr);
        default:
            return _ENOSYS;
    }
}

int_t sys_shmget(dword_t key, dword_t size, dword_t shmflg) {
    return shmget_internal(key, size, shmflg);
}

int_t sys_shmget_guest(dword_t key, qword_t size, dword_t shmflg) {
    return shmget_internal(key, (size_t) size, shmflg);
}

addr_t sys_shmat(int_t shmid, addr_t shmaddr, int_t shmflg) {
    return shmat_internal(shmid, shmaddr, shmflg);
}

guest_addr_t sys_shmat_guest(int_t shmid, guest_addr_t shmaddr, int_t shmflg) {
    return shmat_internal(shmid, shmaddr, shmflg);
}

int_t sys_shmdt(addr_t shmaddr) {
    return shmdt_internal(current->mm, shmaddr, current->pid, false);
}

int_t sys_shmdt_guest(guest_addr_t shmaddr) {
    return shmdt_internal(current->mm, shmaddr, current->pid, false);
}

int_t sys_shmctl(int_t shmid, int_t cmd, addr_t buf) {
    return shmctl_internal(shmid, cmd, buf);
}
