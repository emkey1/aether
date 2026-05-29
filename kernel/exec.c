#include "kernel/signal.h"
#include "task.h"
#define _GNU_SOURCE
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "debug.h"
#include "misc.h"
#include "kernel/calls.h"
#include "kernel/random.h"
#include "kernel/errno.h"
#include "fs/fd.h"
#include "fs/devices.h"
#include "fs/tty.h"
#include "fs/path.h"
#include "kernel/elf.h"
#include "kernel/vdso.h"
#include "jit/jit.h"
#include "tools/ptraceomatic-config.h"
#include "util/sync.h"

#define ARGV_MAX 32 * PAGE_SIZE

struct exec_args {
    // number of arguments
    size_t count;
    // series of count null-terminated strings, plus an extra null for good measure
    const char *args;
};

struct elf_info {
    enum guest_abi abi;
    byte_t bitness;
    uint16_t type;
    uint16_t machine;
    qword_t entry_point;
    qword_t prghead_off;
    uint16_t phent_size;
    uint16_t phent_count;
};

struct elf_prg_info {
    uint32_t type;
    uint32_t flags;
    qword_t offset;
    qword_t vaddr;
    qword_t filesize;
    qword_t memsize;
    qword_t alignment;
};

static inline guest_addr_t align_stack(guest_addr_t sp);
static inline ssize_t user_strlen(guest_addr_t p);
static inline int user_memset(guest_addr_t start, byte_t val, dword_t len);
static inline guest_addr_t copy_string(guest_addr_t sp, const char *string);
static inline guest_addr_t args_copy(guest_addr_t sp, struct exec_args args);
static size_t args_size(struct exec_args args);
static ssize_t user_read_exec_ptr(guest_addr_t addr, qword_t *ptr_out);
static ssize_t read_execve_user_args(guest_addr_t argv_addr, guest_addr_t envp_addr, ssize_t *argc_out,
        char **argv_out, char **envp_out);
static int read_header(struct fd *fd, struct elf_info *header);
static int read_prg_headers(struct fd *fd, struct elf_info header, struct elf_prg_info **ph_out);
static int load_entry(enum guest_abi abi, struct elf_prg_info ph, guest_addr_t bias, struct fd *fd);
static guest_addr_t find_hole_for_elf(struct elf_info *header, struct elf_prg_info *ph);
static int elf_load_addr_candidate(enum guest_abi abi, struct elf_prg_info ph, guest_addr_t bias,
        guest_addr_t *addr_out);
static void amd64_trace_exec_attempt(const char *file, const char *argv);
static void amd64_trace_exec_loader_failure(const char *stage, const char *file, enum guest_abi abi,
        struct elf_prg_info *ph, guest_addr_t bias, struct fd *fd, int err, const char *interp_name);

static bool elf_abi_detect(byte_t bitness, uint16_t machine, enum guest_abi *abi_out) {
    enum guest_abi abi;
    if (bitness == ELF_64BIT && machine == ELF_X86_64) {
        abi = GUEST_ABI_AMD64;
    } else if (bitness == ELF_32BIT && machine == ELF_X86) {
        abi = GUEST_ABI_I386;
    } else {
        return false;
    }
    if (abi_out != NULL)
        *abi_out = abi;
    return true;
}

static bool elf_value_fits_addr(enum guest_abi abi, qword_t value) {
    return guest_abi_addr_valid(abi, value);
}

static int read_header(struct fd *fd, struct elf_info *header) {
    union {
        struct elf_header elf32;
        struct elf64_header elf64;
    } raw;

    ssize_t err;
    if (fd->ops->lseek(fd, 0, SEEK_SET))
        return _EIO;
    if ((err = fd->ops->read(fd, &raw, sizeof(raw))) < (ssize_t) sizeof(struct elf_header)) {
        if (err < 0)
            return _EIO;
        return _ENOEXEC;
    }

    struct elf_header *ident = &raw.elf32;
    enum guest_abi elf_abi;
    if (memcmp(&ident->magic, ELF_MAGIC, sizeof(ident->magic)) != 0
            || (ident->type != ELF_EXECUTABLE && ident->type != ELF_DYNAMIC)
            || ident->endian != ELF_LITTLEENDIAN
            || ident->elfversion1 != 1
            || !elf_abi_detect(ident->bitness, ident->machine, &elf_abi))
        return _ENOEXEC;

    if (ident->bitness == ELF_32BIT) {
        *header = (struct elf_info) {
            .abi = elf_abi,
            .bitness = ident->bitness,
            .type = raw.elf32.type,
            .machine = raw.elf32.machine,
            .entry_point = raw.elf32.entry_point,
            .prghead_off = raw.elf32.prghead_off,
            .phent_size = raw.elf32.phent_size,
            .phent_count = raw.elf32.phent_count,
        };
    } else if (ident->bitness == ELF_64BIT) {
        if (err < (ssize_t) sizeof(struct elf64_header))
            return _ENOEXEC;
        *header = (struct elf_info) {
            .abi = elf_abi,
            .bitness = ident->bitness,
            .type = raw.elf64.type,
            .machine = raw.elf64.machine,
            .entry_point = raw.elf64.entry_point,
            .prghead_off = raw.elf64.prghead_off,
            .phent_size = raw.elf64.phent_size,
            .phent_count = raw.elf64.phent_count,
        };
    } else {
        return _ENOEXEC;
    }
    return 0;
}

static int read_prg_headers(struct fd *fd, struct elf_info header, struct elf_prg_info **ph_out) {
    size_t ph_size = sizeof(struct elf_prg_info) * header.phent_count;
    struct elf_prg_info *ph = malloc(ph_size);
    if (ph == NULL)
        return _ENOMEM;

    memset(ph, 0, ph_size);
    if (fd->ops->lseek(fd, header.prghead_off, SEEK_SET) < 0) {
        free(ph);
        return _EIO;
    }

    if (header.bitness == ELF_32BIT) {
        if (header.phent_size < sizeof(struct prg_header)) {
            free(ph);
            return _ENOEXEC;
        }
        for (uint16_t i = 0; i < header.phent_count; i++) {
            struct prg_header raw;
            if (fd->ops->read(fd, &raw, sizeof(raw)) != sizeof(raw)) {
                free(ph);
                if (errno != 0)
                    return _EIO;
                return _ENOEXEC;
            }
            if (header.phent_size > sizeof(raw) &&
                    fd->ops->lseek(fd, header.phent_size - sizeof(raw), SEEK_CUR) < 0) {
                free(ph);
                return _EIO;
            }
            ph[i] = (struct elf_prg_info) {
                .type = raw.type,
                .flags = raw.flags,
                .offset = raw.offset,
                .vaddr = raw.vaddr,
                .filesize = raw.filesize,
                .memsize = raw.memsize,
                .alignment = raw.alignment,
            };
        }
    } else if (header.bitness == ELF_64BIT) {
        if (header.phent_size < sizeof(struct prg_header64)) {
            free(ph);
            return _ENOEXEC;
        }
        for (uint16_t i = 0; i < header.phent_count; i++) {
            struct prg_header64 raw;
            if (fd->ops->read(fd, &raw, sizeof(raw)) != sizeof(raw)) {
                free(ph);
                if (errno != 0)
                    return _EIO;
                return _ENOEXEC;
            }
            if (header.phent_size > sizeof(raw) &&
                    fd->ops->lseek(fd, header.phent_size - sizeof(raw), SEEK_CUR) < 0) {
                free(ph);
                return _EIO;
            }
            ph[i] = (struct elf_prg_info) {
                .type = raw.type,
                .flags = raw.flags,
                .offset = raw.offset,
                .vaddr = raw.vaddr,
                .filesize = raw.filesize,
                .memsize = raw.memsize,
                .alignment = raw.alignment,
            };
        }
    } else {
        free(ph);
        return _ENOEXEC;
    }

    *ph_out = ph;
    return 0;
}

static int load_entry(enum guest_abi abi, struct elf_prg_info ph, guest_addr_t bias, struct fd *fd) {
    int err;

    if (!elf_value_fits_addr(abi, ph.vaddr) || !elf_value_fits_addr(abi, ph.offset) ||
            !elf_value_fits_addr(abi, ph.memsize) || !elf_value_fits_addr(abi, ph.filesize))
        return _EOVERFLOW;
    if (ph.vaddr > guest_abi_vm_layout(abi).user_addr_max - bias)
        return _EOVERFLOW;

    guest_addr_t addr = (guest_addr_t) ph.vaddr + bias;
    guest_addr_t offset = (guest_addr_t) ph.offset;
    guest_addr_t memsize = (guest_addr_t) ph.memsize;
    guest_addr_t filesize = (guest_addr_t) ph.filesize;

    int flags = P_READ;
    if (ph.flags & PH_W) flags |= P_WRITE;

    if ((err = fd->ops->mmap(fd, current->mem, PAGE(addr),
                    PAGE_ROUND_UP(filesize + PGOFFSET(addr)),
                    offset - PGOFFSET(addr), flags, MMAP_PRIVATE)) < 0) {
        amd64_trace_exec_loader_failure("segment-mmap", NULL, abi, &ph, bias, fd, err, NULL);
        return err;
    }
    // TODO find a better place for these to avoid code duplication
    mem_pt(current->mem, PAGE(addr))->data->fd = fd_retain(fd);
    mem_pt(current->mem, PAGE(addr))->data->file_offset = offset - PGOFFSET(addr);

    guest_addr_t file_end = addr + filesize;

    // ELF requires the remainder of the final file-backed page in a PT_LOAD
    // segment to read as zero. When the host page size is larger than the
    // guest page size, the mmap above can otherwise expose later file bytes in
    // that guest-visible tail.
    dword_t tail_size = PAGE_SIZE - PGOFFSET(file_end);
    if (tail_size == PAGE_SIZE)
        tail_size = 0;

    if (tail_size != 0 && (flags & P_WRITE)) {
        // Unlock and lock the mem because the user functions must be
        // called without locking mem.
        struct mem *mem = current->mem;
        write_unlock(&mem->lock);

        int memset_err = user_memset(file_end, 0, tail_size);
        write_lock(&mem->lock);
        if (memset_err) {
            amd64_trace_exec_loader_failure("segment-bss-tail", NULL, abi, &ph, bias, fd, _EFAULT, NULL);
            return _EFAULT;
        }
    }

    if (memsize > filesize) {
        dword_t bss_size = memsize - filesize;
        if (tail_size > bss_size)
            tail_size = bss_size;
        dword_t extra_bss_size = bss_size - tail_size;
        if (extra_bss_size != 0) {
            if ((err = pt_map_nothing(current->mem, PAGE_ROUND_UP(file_end),
                            PAGE_ROUND_UP(extra_bss_size), flags)) < 0) {
                amd64_trace_exec_loader_failure("segment-bss-map", NULL, abi, &ph, bias, fd, err, NULL);
                return err;
            }
        }
    }

    return 0;
}

static guest_addr_t find_hole_for_elf(struct elf_info *header, struct elf_prg_info *ph) {
    bool found = false;
    page_t first_page = 0;
    page_t last_page = 0;
    for (int i = 0; i < header->phent_count; i++) {
        if (ph[i].type != PT_LOAD)
            continue;

        qword_t end_vaddr = ph[i].vaddr + ph[i].memsize;
        if (end_vaddr < ph[i].vaddr)
            return 0;
        if (!elf_value_fits_addr(header->abi, end_vaddr) || !elf_value_fits_addr(header->abi, ph[i].vaddr))
            return 0;

        page_t seg_first = PAGE(ph[i].vaddr);
        page_t seg_last = PAGE_ROUND_UP(end_vaddr);
        if (!found) {
            first_page = seg_first;
            last_page = seg_last;
            found = true;
            continue;
        }
        if (seg_first < first_page)
            first_page = seg_first;
        if (seg_last > last_page)
            last_page = seg_last;
    }
    pages_t size = 0;
    if (found) {
        if (last_page < first_page)
            return 0;
        size = last_page - first_page;
    }
    page_t hole = pt_find_hole(current->mem, size);
    if (hole == BAD_PAGE)
        return 0;
    guest_addr_t base = ((guest_addr_t) hole - first_page) << PAGE_BITS;
    return base;
}

static int elf_load_addr_candidate(enum guest_abi abi, struct elf_prg_info ph, guest_addr_t bias,
        guest_addr_t *addr_out) {
    qword_t mapped_load_addr = (qword_t) bias + ph.vaddr;
    if (ph.offset > mapped_load_addr)
        return _EOVERFLOW;
    mapped_load_addr -= ph.offset;
    if (!elf_value_fits_addr(abi, mapped_load_addr))
        return _EOVERFLOW;
    *addr_out = (guest_addr_t) mapped_load_addr;
    return 0;
}

static void amd64_trace_exec_attempt(const char *file, const char *argv) {
    (void) file;
    (void) argv;
}

static void amd64_trace_exec_loader_failure(const char *stage, const char *file, enum guest_abi abi,
        struct elf_prg_info *ph, guest_addr_t bias, struct fd *fd, int err, const char *interp_name) {
    (void) stage;
    (void) file;
    (void) abi;
    (void) ph;
    (void) bias;
    (void) fd;
    (void) err;
    (void) interp_name;
}

static bool i386_force_safe_exec_comm(const char *comm) {
    return comm != NULL &&
        strcmp(comm, "pkcsslotd") == 0;
}

static intptr_t elf_exec(struct fd *fd, const char *file, struct exec_args argv, struct exec_args envp) {
    intptr_t err = 0;
    struct task *save = current;
    bool mem_locked = false;
    struct mm *new_mm = NULL;

    // read the headers
    struct elf_info header;
    if ((err = read_header(fd, &header)) < 0)
        return err;
    size_t guest_word_size = guest_abi_desc(header.abi).pointer_size;
    bool is_64bit = guest_abi_is_64bit(header.abi);
    struct elf_prg_info *ph;
    if ((err = read_prg_headers(fd, header, &ph)) < 0)
        return err;

    // look for an interpreter
    char *interp_name = NULL;
    struct fd *interp_fd = NULL;
    struct elf_info interp_header;
    struct elf_prg_info *interp_ph = NULL;
    for (unsigned i = 0; i < header.phent_count; i++) {
        if (ph[i].type != PT_INTERP)
            continue;
        if (interp_name) {
            err = _EINVAL;
            goto out_free_interp;
        }

        interp_name = malloc(ph[i].filesize);
        err = _ENOMEM;
        if (interp_name == NULL)
            goto out_free_ph;

        err = _EIO;
        if (fd->ops->lseek(fd, ph[i].offset, SEEK_SET) < 0)
            goto out_free_interp;
        size_t interp_size = ph[i].filesize;
        if (fd->ops->read(fd, interp_name, interp_size) != (ssize_t) interp_size)
            goto out_free_interp;

        interp_fd = generic_open(interp_name, O_RDONLY, 0);
        if (IS_ERR(interp_fd)) {
            err = PTR_ERR(interp_fd);
            goto out_free_interp;
        }
        if ((err = read_header(interp_fd, &interp_header)) < 0) {
            if (err == _ENOEXEC)
                err = _ELIBBAD;
            goto out_free_interp;
        }
        if (interp_header.abi != header.abi) {
            err = _ELIBBAD;
            goto out_free_interp;
        }
        if ((err = read_prg_headers(interp_fd, interp_header, &interp_ph)) < 0) {
            if (err == _ENOEXEC)
                err = _ELIBBAD;
            goto out_free_interp;
        }
    }

    new_mm = mm_new(header.abi);
    if (new_mm == NULL) {
        err = _ENOMEM;
        goto out_free_interp;
    }

    // free the process's memory.
    // from this point on, if any error occurs the process will have to be
    // killed before it even starts. please don't be too sad about it, it's
    // just a process.
    //
    // general_lock protects current->mm. otherwise procfs might read the
    // pointer before it's released and then try to lock it after it's
    // released.
    lock(&save->general_lock, 0);
    mm_release(save->mm);
    save->abi = header.abi;
    task_set_mm(save, new_mm);
    new_mm = NULL;
    unlock(&save->general_lock);
    write_lock(&save->mem->lock);
    mem_locked = true;

    save->mm->exefile = fd_retain(fd);

    guest_addr_t load_addr = 0;
    bool load_addr_set = false;
    guest_addr_t bias = 0;

    for (unsigned i = 0; i < header.phent_count; i++) {
        if (ph[i].type != PT_LOAD)
            continue;

        if (!load_addr_set && header.type == ELF_DYNAMIC) {
            if (interp_name && header.abi == GUEST_ABI_I386)
                bias = 0x56555000;
            else
                bias = find_hole_for_elf(&header, ph);
        }

        if ((err = load_entry(header.abi, ph[i], bias, fd)) < 0)
            goto beyond_hope;

        guest_addr_t candidate_load_addr;
        if ((err = elf_load_addr_candidate(header.abi, ph[i], bias, &candidate_load_addr)) < 0)
            goto beyond_hope;
        if (!load_addr_set || candidate_load_addr < load_addr) {
            load_addr = candidate_load_addr;
            load_addr_set = true;
        }

        qword_t brk_q = (qword_t) bias + ph[i].vaddr + ph[i].memsize;
        if (!elf_value_fits_addr(header.abi, brk_q)) {
            err = _EOVERFLOW;
            goto beyond_hope;
        }
        guest_addr_t brk = (guest_addr_t) brk_q;
        if (brk > save->mm->start_brk)
            save->mm->start_brk = save->mm->brk = BYTES_ROUND_UP(brk);
    }

    qword_t entry_q = (qword_t) bias + header.entry_point;
    if (!elf_value_fits_addr(header.abi, entry_q)) {
        err = _EOVERFLOW;
        goto beyond_hope;
    }
    guest_addr_t entry = (guest_addr_t) entry_q;
    guest_addr_t interp_base = 0;

    if (interp_name) {
        interp_base = find_hole_for_elf(&interp_header, interp_ph);
        for (int i = interp_header.phent_count - 1; i >= 0; i--) {
            if (interp_ph[i].type != PT_LOAD)
                continue;
            if ((err = load_entry(interp_header.abi, interp_ph[i], interp_base, interp_fd)) < 0)
                goto beyond_hope;
        }
        entry_q = (qword_t) interp_base + interp_header.entry_point;
        if (!elf_value_fits_addr(interp_header.abi, entry_q)) {
            err = _EOVERFLOW;
            goto beyond_hope;
        }
        entry = (guest_addr_t) entry_q;
    }

    guest_addr_t vdso_entry = 0;
    if (!is_64bit) {
        err = _ENOMEM;
        pages_t vdso_pages = sizeof(vdso_data) >> PAGE_BITS;
        page_t vdso_page = pt_find_hole(save->mem, vdso_pages + 1);
        if (vdso_page == BAD_PAGE)
            goto beyond_hope;
        vdso_page += 1;
        if ((err = pt_map(save->mem, vdso_page, vdso_pages, (void *) vdso_data, 0, 0)) < 0)
            goto beyond_hope;
        mem_pt(save->mem, vdso_page)->data->name = "[vdso]";
        save->mm->vdso = vdso_page << PAGE_BITS;
        vdso_entry = save->mm->vdso + ((struct elf_header *) vdso_data)->entry_point;

        page_t vvar_page = pt_find_hole(save->mem, VVAR_PAGES);
        if (vvar_page == BAD_PAGE)
            goto beyond_hope;
        if ((err = pt_map_nothing(save->mem, vvar_page, VVAR_PAGES, 0)) < 0)
            goto beyond_hope;
        mem_pt(save->mem, vvar_page)->data->name = "[vvar]";
    }

    struct guest_vm_layout vm_layout = guest_abi_vm_layout(save->abi);
    if ((err = pt_map_nothing(save->mem, vm_layout.stack_page, 1, P_WRITE | P_GROWSDOWN)) < 0)
        goto beyond_hope;
    write_unlock(&save->mem->lock);
    mem_locked = false;

    guest_addr_t sp = vm_layout.stack_pointer;
    sp -= guest_word_size;

    err = _EFAULT;
    guest_addr_t file_addr = sp = copy_string(sp, file);
    if (sp == 0)
        goto beyond_hope;
    guest_addr_t envp_addr = sp = args_copy(sp, envp);
    if (sp == 0)
        goto beyond_hope;
    save->mm->env_start = sp;
    save->mm->env_end = sp + args_size(envp);
    guest_addr_t argv_addr = sp = args_copy(sp, argv);
    if (sp == 0)
        goto beyond_hope;
    save->mm->argv_start = sp;
    save->mm->argv_end = sp + args_size(argv);
    sp = align_stack(sp);

    guest_addr_t platform_addr = sp = copy_string(sp, task_abi_desc(save).elf_platform);
    if (sp == 0)
        goto beyond_hope;
    char random[16] = {};
    get_random(random, sizeof(random));
    guest_addr_t random_addr = sp -= sizeof(random);
    if (user_put(sp, random))
        goto beyond_hope;

    size_t vector_bytes = ((argv.count + 1) + (envp.count + 1) + 1) * guest_word_size;
    if (!is_64bit) {
        struct aux_ent aux[] = {
            {AX_SYSINFO, vdso_entry},
            {AX_SYSINFO_EHDR, save->mm->vdso},
            {AX_HWCAP, 0},
            {AX_PAGESZ, PAGE_SIZE},
            {AX_CLKTCK, 0x64},
            {AX_PHDR, load_addr + header.prghead_off},
            {AX_PHENT, header.phent_size},
            {AX_PHNUM, header.phent_count},
            {AX_BASE, interp_base},
            {AX_FLAGS, 0},
            {AX_ENTRY, bias + header.entry_point},
            {AX_UID, 0},
            {AX_EUID, 0},
            {AX_GID, 0},
            {AX_EGID, 0},
            {AX_SECURE, 0},
            {AX_RANDOM, random_addr},
            {AX_HWCAP2, 0},
            {AX_EXECFN, file_addr},
            {AX_PLATFORM, platform_addr},
            {0, 0}
        };
        sp -= vector_bytes;
        sp -= sizeof(aux);
        sp = align_stack(sp);

        guest_addr_t p = sp;
        dword_t argc_word = (dword_t) argv.count;
        dword_t zero = 0;
        if (user_put(p, argc_word))
            goto beyond_hope;
        p += guest_word_size;

        size_t argc = argv.count;
        while (argc-- > 0) {
            dword_t argv_word = (dword_t) argv_addr;
            if (user_put(p, argv_word))
                goto beyond_hope;
            ssize_t arg_len = user_strlen(argv_addr);
            if (arg_len < 0)
                goto beyond_hope;
            argv_addr += arg_len + 1;
            p += guest_word_size;
        }
        if (user_put(p, zero))
            goto beyond_hope;
        p += guest_word_size;

        size_t envc = envp.count;
        while (envc-- > 0) {
            dword_t envp_word = (dword_t) envp_addr;
            if (user_put(p, envp_word))
                goto beyond_hope;
            ssize_t env_len = user_strlen(envp_addr);
            if (env_len < 0)
                goto beyond_hope;
            envp_addr += env_len + 1;
            p += guest_word_size;
        }
        if (user_put(p, zero))
            goto beyond_hope;
        p += guest_word_size;

        save->mm->auxv_start = p;
        if (user_put(p, aux))
            goto beyond_hope;
        p += sizeof(aux);
        save->mm->auxv_end = p;
    } else {
        struct aux64_ent aux[] = {
            {AX_HWCAP, 0},
            {AX_PAGESZ, PAGE_SIZE},
            {AX_CLKTCK, 0x64},
            {AX_PHDR, load_addr + header.prghead_off},
            {AX_PHENT, header.phent_size},
            {AX_PHNUM, header.phent_count},
            {AX_BASE, interp_base},
            {AX_FLAGS, 0},
            {AX_ENTRY, bias + header.entry_point},
            {AX_UID, 0},
            {AX_EUID, 0},
            {AX_GID, 0},
            {AX_EGID, 0},
            {AX_SECURE, 0},
            {AX_RANDOM, random_addr},
            {AX_HWCAP2, 0},
            {AX_EXECFN, file_addr},
            {AX_PLATFORM, platform_addr},
            {0, 0}
        };
        sp -= vector_bytes;
        sp -= sizeof(aux);
        sp = align_stack(sp);

        guest_addr_t p = sp;
        qword_t argc_word = (qword_t) argv.count;
        qword_t zero = 0;
        if (user_put(p, argc_word))
            goto beyond_hope;
        p += guest_word_size;

        size_t argc = argv.count;
        while (argc-- > 0) {
            qword_t argv_word = (qword_t) argv_addr;
            if (user_put(p, argv_word))
                goto beyond_hope;
            ssize_t arg_len = user_strlen(argv_addr);
            if (arg_len < 0)
                goto beyond_hope;
            argv_addr += arg_len + 1;
            p += guest_word_size;
        }
        if (user_put(p, zero))
            goto beyond_hope;
        p += guest_word_size;

        size_t envc = envp.count;
        while (envc-- > 0) {
            qword_t envp_word = (qword_t) envp_addr;
            if (user_put(p, envp_word))
                goto beyond_hope;
            ssize_t env_len = user_strlen(envp_addr);
            if (env_len < 0)
                goto beyond_hope;
            envp_addr += env_len + 1;
            p += guest_word_size;
        }
        if (user_put(p, zero))
            goto beyond_hope;
        p += guest_word_size;

        save->mm->auxv_start = p;
        if (user_put(p, aux))
            goto beyond_hope;
        p += sizeof(aux);
        save->mm->auxv_end = p;
    }

    save->mm->stack_start = sp;
    save->cpu.amd64_syscall = (struct amd64_syscall_state) {};
    save->cpu.fcw = 0x37f;

    memset(save->cpu.amd64_regs, 0, sizeof(save->cpu.amd64_regs));
    save->cpu.amd64_rip = entry;
    save->cpu.amd64_regs[amd64_rsp] = sp;
    memset(save->cpu.amd64_store_trace, 0, sizeof(save->cpu.amd64_store_trace));
    save->cpu.amd64_store_trace_next = 0;

    save->cpu.esp = (addr_t) sp;
    save->cpu.eip = (addr_t) entry;
    save->cpu.eax = 0;
    save->cpu.ebx = 0;
    save->cpu.ecx = 0;
    save->cpu.edx = 0;
    save->cpu.esi = 0;
    save->cpu.edi = 0;
    save->cpu.ebp = 0;
    collapse_flags(&save->cpu);
    save->cpu.eflags = 0;

    err = 0;
out_free_interp:
    if (new_mm != NULL)
        mm_release(new_mm);
    if (interp_name != NULL)
        free(interp_name);
    if (interp_fd != NULL && !IS_ERR(interp_fd))
        fd_close(interp_fd);
    if (interp_ph != NULL)
        free(interp_ph);
out_free_ph:
    free(ph);
    return err;

beyond_hope:
    amd64_trace_exec_loader_failure("elf-exec", file, header.abi, NULL, bias, fd, err, interp_name);
    if (mem_locked)
        write_unlock(&save->mem->lock);
    goto out_free_interp;
}

static size_t args_size(struct exec_args args) {
    const char *args_end = args.args;
    for (size_t i = 0; i < args.count; i++) {
        args_end += strlen(args_end) + 1;
    }
    // don't forget the very last null terminator
    assert(args_end[0] == '\0');
    args_end++;
    return args_end - args.args;
}

static inline guest_addr_t align_stack(guest_addr_t sp) {
    return sp &~ 0xf;
}

static inline guest_addr_t copy_string(guest_addr_t sp, const char *string) {
    sp -= strlen(string) + 1;
    if (user_write_string(sp, string))
        return 0;
    return sp;
}

static inline guest_addr_t args_copy(guest_addr_t sp, struct exec_args args) {
    size_t size = args_size(args);
    sp -= size;
    if (user_write(sp, args.args, size))
        return 0;
    return sp;
}

static inline ssize_t user_strlen(guest_addr_t p) {
    size_t i = 0;
    char c;
    do {
        if (user_get(p + i, c))
            return -1;
        i++;
    } while (c != '\0');
    return i - 1;
}

static inline int user_memset(guest_addr_t start, byte_t val, dword_t len) {
    while (len--)
        if (user_put(start++, val))
            return 1;
    return 0;
}

static int format_exec(struct fd *fd, const char *file, struct exec_args argv, struct exec_args envp) {
    int err = (int)elf_exec(fd, file, argv, envp);
    if (err != _ENOEXEC)
        return err;
    // other formats would go here
    return _ENOEXEC;
}

static int shebang_exec(struct fd *fd, const char *file, struct exec_args argv, struct exec_args envp) {
    // read the first 128 bytes to get the shebang line out of
    if (fd->ops->lseek(fd, 0, SEEK_SET))
        return _EIO;
    char header[128];
    ssize_t size = fd->ops->read(fd, header, sizeof(header) - 1);
    if (size < 0)
        return _EIO;
    header[size] = '\0';

    // only look at the first line
    char *newline = strchr(header, '\n');
    if (newline == NULL)
        return _ENOEXEC;
    *newline = '\0';

    // format: #![spaces]interpreter[spaces]argument[spaces]
    char *p = header;
    if (p[0] != '#' || p[1] != '!')
        return _ENOEXEC;
    p += 2;
    while (*p == ' ')
        p++;
    if (*p == '\0')
        return _ENOEXEC;

    char *interpreter = p;
    while (*p != ' ' && *p != '\0')
        p++;
    if (*p != '\0') {
        *p++ = '\0';
        while (*p == ' ')
            p++;
    }

    char *argument = p;
    // strip trailing whitespace
    p = strchr(p, '\0') - 1;
    while (*p == ' ')
        *p-- = '\0';
    if (*argument == '\0')
        argument = NULL;

    struct exec_args argv_rest = {
        .count = argv.count - 1,
        .args = argv.args + strlen(argv.args) + 1,
    };
    size_t args_rest_size = args_size(argv_rest);

    // Bolt: Cache lengths to avoid redundant O(N) traversals
    size_t interpreter_len = strlen(interpreter);
    size_t file_len = strlen(file);
    size_t argument_len = argument ? strlen(argument) : 0;

    size_t extra_args_size = interpreter_len + 1 + file_len + 1;
    if (argument)
        extra_args_size += argument_len + 1;
    if (args_rest_size + extra_args_size >= ARGV_MAX)
        return _E2BIG;

    char *new_argv_buf = malloc(ARGV_MAX);
    if (new_argv_buf == NULL)
        return _ENOMEM;
    struct exec_args new_argv = {.args = new_argv_buf};
    size_t n = 0;

    // Bolt: Use memcpy with cached lengths instead of strcpy + strlen
    memcpy(new_argv_buf, interpreter, interpreter_len + 1);
    new_argv.count++;
    n += interpreter_len + 1;
    if (argument) {
        memcpy(new_argv_buf + n, argument, argument_len + 1);
        new_argv.count++;
        n += argument_len + 1;
    }
    memcpy(new_argv_buf + n, file, file_len + 1);
    n += file_len + 1;
    new_argv.count++;
    memcpy(new_argv_buf + n, argv_rest.args, args_rest_size);
    new_argv.count += argv_rest.count;

    struct fd *interpreter_fd = generic_open(interpreter, O_RDONLY_, 0);
    if (IS_ERR(interpreter_fd)) {
        free(new_argv_buf);
        return (int)PTR_ERR(interpreter_fd);
    }
    int err = format_exec(interpreter_fd, interpreter, new_argv, envp);
    fd_close(interpreter_fd);
    free(new_argv_buf);
    return err;
}

int __do_execve(const char *file, struct exec_args argv, struct exec_args envp) {
    struct fd *fd = generic_open(file, O_RDONLY, 0);
    if (IS_ERR(fd))
        return (int) PTR_ERR(fd);

    struct statbuf stat;
    int err = fd->mount->fs->fstat(fd, &stat);
    if (err < 0) {
        fd_close(fd);
        return err;
    }

    // if nobody has permission to execute, it should be safe to not execute
    if (!(stat.mode & 0111)) {
        fd_close(fd);
        return _EACCES;
    }

    err = format_exec(fd, file, argv, envp);
    if (err == _ENOEXEC)
        err = shebang_exec(fd, file, argv, envp);
    fd_close(fd);
    if (err < 0) {
        amd64_trace_exec_loader_failure("do-execve", file, current->abi, NULL, 0, NULL, err, NULL);
        return err;
    }

    // setuid/setgid
    if (stat.mode & S_ISUID) {
        current->suid = current->euid;
        current->euid = stat.uid;
        current->fsuid = current->euid;
    }
    if (stat.mode & S_ISGID) {
        current->sgid = current->egid;
        current->egid = stat.gid;
        current->fsgid = current->egid;
    }

    // save current->comm
    char old_comm[sizeof(current->comm)];
    lock(&current->general_lock, 0);
    strncpy(old_comm, current->comm, sizeof(old_comm));
    old_comm[sizeof(old_comm) - 1] = '\0';
    const char *basename = strrchr(file, '/');
    if (basename == NULL)
        basename = file;
    else
        basename++;
    strncpy(current->comm, basename, sizeof(current->comm));
    current->comm[sizeof(current->comm) - 1] = '\0';
    unlock(&current->general_lock);

    bool force_safe_i386 = current->abi == GUEST_ABI_I386 &&
            i386_force_safe_exec_comm(current->comm);
    current->force_single_step = (current->abi == GUEST_ABI_I386 &&
            i386_single_step_comm_matches(current->comm)) || force_safe_i386;
    current->force_no_jit_cache = (current->abi == GUEST_ABI_I386 &&
            i386_no_cache_comm_matches(current->comm)) || force_safe_i386;
    if (current->force_no_jit_cache) {
        i386_special_trace_reset(current->tgid, current->comm);
    }

    {
        enum { AMD64_EXEC_TRACE_BUDGET = 64 };
        static unsigned amd64_exec_trace_count;
        lock(&current->group->lock, 0);
        struct tty *tty = current->group->tty;
        unlock(&current->group->lock);
        bool trace_exec = current->abi == GUEST_ABI_AMD64 &&
                tty != NULL &&
                (tty->type == TTY_CONSOLE_MAJOR || tty->type == TTY_PSEUDO_SLAVE_MAJOR);
        bool tracked_exec = strstr(file, "rustc") != NULL || strstr(file, "cargo") != NULL;
        bool tracked_lineage = amd64_trace_is_lineage_tgid(current->tgid);
        if ((trace_exec || tracked_exec || tracked_lineage) &&
                amd64_exec_trace_count < AMD64_EXEC_TRACE_BUDGET)
            amd64_exec_trace_count++;
        if (tracked_exec || tracked_lineage)
            amd64_trace_track_exec(current->pid, current->tgid, file);
    }

    update_thread_name();

    // cloexec
    // consider putting this in fd.c?
    fdtable_do_cloexec(current->files);

    // reset signal handlers
    lock(&current->sighand->lock, 0);
    for (int sig = 0; sig < NUM_SIGS; sig++) {
        struct sigaction_ *action = &current->sighand->action[sig];
        if (action->handler != SIG_IGN_)
            action->handler = SIG_DFL_;
    }
    current->altstack = 0;
    current->altstack_size = 0;
    unlock(&current->sighand->lock);

    current->did_exec = true;
    vfork_notify(current);

    if (current->ptrace.traced) {
        current->ptrace.syscall = current->cpu.eax;
        current->cpu.eax = 0;
        struct siginfo_ info = {
            .sig = SIGTRAP_,
            .code = SI_USER_,
            .kill.pid = current->pid,
            .kill.uid = current->uid,
        };
        if (current->ptrace.options & PTRACE_O_TRACEEXEC_)
            ptrace_event_stop(SIGTRAP_, &info, PTRACE_EVENT_EXEC_, current->pid);
        else
            ptrace_signal_stop(SIGTRAP_, &info);
    }

    return 0;
}

int do_execve(const char *file, size_t argc, const char *argv_p, const char *envp_p) {
    struct exec_args argv = {.count = argc, .args = argv_p};
    struct exec_args envp = {.args = envp_p};
    while (*envp_p != '\0') {
        envp_p += strlen(envp_p) + 1;
        envp.count++;
    }
    return __do_execve(file, argv, envp);
}

static ssize_t user_read_string_array(guest_addr_t addr, char *buf, size_t max) {
    size_t guest_ptr_size = task_abi_desc(current).pointer_size;
    size_t i = 0;
    size_t p = 0;
    for (;;) {
        qword_t str_addr_q;
        ssize_t err = user_read_exec_ptr(addr + i * guest_ptr_size, &str_addr_q);
        if (err < 0)
            return err;
        if (str_addr_q == 0)
            break;
        if (!guest_abi_addr_valid(current->abi, str_addr_q))
            return _EFAULT;
        guest_addr_t str_addr = str_addr_q;
        size_t str_p = 0;
        for (;;) {
            if (p >= max)
                return _E2BIG;
            if (user_get(str_addr + str_p, buf[p]))
                return _EFAULT;
            str_p++;
            p++;
            if (buf[p - 1] == '\0')
                break;
        }
        i++;
    }
    if (p >= max)
        return _E2BIG;
    buf[p] = '\0';
    return i;
}

static ssize_t user_read_exec_ptr(guest_addr_t addr, qword_t *ptr_out) {
    if (task_is_64bit(current)) {
        qword_t ptr;
        if (user_get(addr, ptr))
            return _EFAULT;
        *ptr_out = ptr;
    } else {
        dword_t ptr;
        if (user_get(addr, ptr))
            return _EFAULT;
        *ptr_out = ptr;
    }
    return 0;
}

ssize_t sys_execve(addr_t filename_addr, addr_t argv_addr, addr_t envp_addr) {
    char filename[MAX_PATH];
    if (user_read_string(filename_addr, filename, sizeof(filename)))
        return _EFAULT;

    ssize_t argc;
    char *argv = NULL;
    char *envp = NULL;
    ssize_t err = read_execve_user_args(argv_addr, envp_addr, &argc, &argv, &envp);
    if (err < 0)
        return err;

    STRACE("execve(\"%.1000s\", {", filename);
    const char *args = argv;
    while (*args != '\0') {
        STRACE("\"%.1000s\", ", args);
        args += strlen(args) + 1;
    }
    STRACE("}, {");
    args = envp;
    while (*args != '\0') {
        STRACE("\"%.1000s\", ", args);
        args += strlen(args) + 1;
    }
    STRACE("})");

    amd64_trace_exec_attempt(filename, argv);
    err = do_execve(filename, argc, argv, envp);

    free(envp);
    free(argv);
    return err;
}

ssize_t sys_execve_guest(guest_addr_t filename_addr, guest_addr_t argv_addr, guest_addr_t envp_addr) {
    char filename[MAX_PATH];
    if (user_read_string(filename_addr, filename, sizeof(filename)))
        return _EFAULT;

    ssize_t argc;
    char *argv = NULL;
    char *envp = NULL;
    ssize_t err = read_execve_user_args(argv_addr, envp_addr, &argc, &argv, &envp);
    if (err < 0)
        return err;

    STRACE("execve(\"%.1000s\", {", filename);
    const char *args = argv;
    while (*args != '\0') {
        STRACE("\"%.1000s\", ", args);
        args += strlen(args) + 1;
    }
    STRACE("}, {");
    args = envp;
    while (*args != '\0') {
        STRACE("\"%.1000s\", ", args);
        args += strlen(args) + 1;
    }
    STRACE("})");

    amd64_trace_exec_attempt(filename, argv);
    err = do_execve(filename, argc, argv, envp);

    free(envp);
    free(argv);
    return err;
}

ssize_t sys_execveat(fd_t dirfd, addr_t filename_addr, addr_t argv_addr, addr_t envp_addr, int_t flags) {
    if (flags & ~(AT_EMPTY_PATH_ | AT_SYMLINK_NOFOLLOW_)) {
        if (current != NULL && current->abi == GUEST_ABI_AMD64 && amd64_trace_is_lineage_tgid(current->tgid))
            printk("amd64 execveat invalid flags: pid=%d tgid=%d comm=%s flags=%#x dirfd=%d guest=0\n",
                   current->pid, current->tgid, current->comm, flags, dirfd);
        return _EINVAL;
    }

    char filename[MAX_PATH] = "";
    if (filename_addr != 0 && user_read_string(filename_addr, filename, sizeof(filename)))
        return _EFAULT;

    ssize_t argc;
    char *argv = NULL;
    char *envp = NULL;
    ssize_t err = read_execve_user_args(argv_addr, envp_addr, &argc, &argv, &envp);
    if (err < 0)
        return err;

    char resolved[MAX_PATH];
    if (filename[0] == '\0') {
        if (!(flags & AT_EMPTY_PATH_)) {
            err = _ENOENT;
            goto out_free_args;
        }
        struct fd *fd = (dirfd == AT_FDCWD_) ? AT_PWD : f_get(dirfd);
        if (fd == NULL) {
            err = _EBADF;
            goto out_free_args;
        }
        err = generic_getpath(fd, resolved);
        if (err < 0)
            goto out_free_args;
    } else if (filename[0] == '/') {
        strcpy(resolved, filename);
    } else {
        struct fd *at = (dirfd == AT_FDCWD_) ? AT_PWD : f_get(dirfd);
        if (at == NULL) {
            err = _EBADF;
            goto out_free_args;
        }
        err = path_normalize(at, filename, resolved,
                (flags & AT_SYMLINK_NOFOLLOW_) ? N_SYMLINK_NOFOLLOW : N_SYMLINK_FOLLOW);
        if (err < 0)
            goto out_free_args;
    }

    STRACE("execveat(%d, \"%s\", ..., %#x)", dirfd, filename, flags);
    amd64_trace_exec_attempt(resolved, argv);
    err = do_execve(resolved, argc, argv, envp);

out_free_args:
    free(envp);
    free(argv);
    return err;
}

ssize_t sys_execveat_guest(fd_t dirfd, guest_addr_t filename_addr, guest_addr_t argv_addr, guest_addr_t envp_addr, int_t flags) {
    if (flags & ~(AT_EMPTY_PATH_ | AT_SYMLINK_NOFOLLOW_)) {
        if (current != NULL && current->abi == GUEST_ABI_AMD64 && amd64_trace_is_lineage_tgid(current->tgid))
            printk("amd64 execveat invalid flags: pid=%d tgid=%d comm=%s flags=%#x dirfd=%d guest=1\n",
                   current->pid, current->tgid, current->comm, flags, dirfd);
        return _EINVAL;
    }

    char filename[MAX_PATH] = "";
    if (filename_addr != 0 && user_read_string(filename_addr, filename, sizeof(filename)))
        return _EFAULT;

    ssize_t argc;
    char *argv = NULL;
    char *envp = NULL;
    ssize_t err = read_execve_user_args(argv_addr, envp_addr, &argc, &argv, &envp);
    if (err < 0)
        return err;

    char resolved[MAX_PATH];
    if (filename[0] == '\0') {
        if (!(flags & AT_EMPTY_PATH_)) {
            err = _ENOENT;
            goto out_free_args;
        }
        struct fd *fd = (dirfd == AT_FDCWD_) ? AT_PWD : f_get(dirfd);
        if (fd == NULL) {
            err = _EBADF;
            goto out_free_args;
        }
        err = generic_getpath(fd, resolved);
        if (err < 0)
            goto out_free_args;
    } else if (filename[0] == '/') {
        strcpy(resolved, filename);
    } else {
        struct fd *at = (dirfd == AT_FDCWD_) ? AT_PWD : f_get(dirfd);
        if (at == NULL) {
            err = _EBADF;
            goto out_free_args;
        }
        err = path_normalize(at, filename, resolved,
                (flags & AT_SYMLINK_NOFOLLOW_) ? N_SYMLINK_NOFOLLOW : N_SYMLINK_FOLLOW);
        if (err < 0)
            goto out_free_args;
    }

    STRACE("execveat(%d, \"%.1000s\", {", dirfd, resolved);
    const char *args = argv;
    while (*args != '\0') {
        STRACE("\"%.1000s\", ", args);
        args += strlen(args) + 1;
    }
    STRACE("}, {");
    args = envp;
    while (*args != '\0') {
        STRACE("\"%.1000s\", ", args);
        args += strlen(args) + 1;
    }
    STRACE("}, %d)", flags);

    amd64_trace_exec_attempt(resolved, argv);
    err = do_execve(resolved, argc, argv, envp);

out_free_args:
    free(envp);
    free(argv);
    return err;
}

static ssize_t read_execve_user_args(guest_addr_t argv_addr, guest_addr_t envp_addr, ssize_t *argc_out,
        char **argv_out, char **envp_out) {
    char *argv = malloc(ARGV_MAX);
    if (argv == NULL)
        return _ENOMEM;
    ssize_t argc = user_read_string_array(argv_addr, argv, ARGV_MAX);
    if (argc < 0) {
        free(argv);
        return argc;
    }

    char *envp = malloc(ARGV_MAX);
    if (envp == NULL) {
        free(argv);
        return _ENOMEM;
    }
    if (envp_addr != 0) {
        ssize_t err = user_read_string_array(envp_addr, envp, ARGV_MAX);
        if (err < 0) {
            free(envp);
            free(argv);
            return err;
        }
    } else {
        // Do not take advantage of this nonstandard and nonportable misfeature!
        // - Michael Kerrisk, execve(2)
        envp[0] = envp[1] = '\0';
    }

    *argc_out = argc;
    *argv_out = argv;
    *envp_out = envp;
    return 0;
}
