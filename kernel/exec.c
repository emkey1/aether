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
#include "fs/tty.h"
#include "fs/path.h"
#include "kernel/elf.h"
#include "kernel/vdso.h"
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

static inline addr_t align_stack(addr_t sp);
static inline ssize_t user_strlen(size_t p);
static inline int user_memset(addr_t start, byte_t val, dword_t len);
static inline addr_t copy_string(addr_t sp, const char *string);
static inline addr_t args_copy(addr_t sp, struct exec_args args);
static size_t args_size(struct exec_args args);
static ssize_t read_execve_user_args(addr_t argv_addr, addr_t envp_addr, ssize_t *argc_out,
        char **argv_out, char **envp_out);
static int read_header(struct fd *fd, enum guest_abi abi, struct elf_info *header);
static int read_prg_headers(struct fd *fd, struct elf_info header, struct elf_prg_info **ph_out);
static int load_entry(struct elf_prg_info ph, addr_t bias, struct fd *fd);
static addr_t find_hole_for_elf(struct elf_info *header, struct elf_prg_info *ph);

static bool trace_session_exec_name(const char *name) {
    return strcmp(name, "login") == 0 ||
        strcmp(name, "sshd") == 0 ||
        strcmp(name, "sh") == 0 ||
        strcmp(name, "bash") == 0 ||
        strcmp(name, "dash") == 0 ||
        strcmp(name, "getty") == 0 ||
        strcmp(name, "agetty") == 0;
}

static bool trace_session_exec_attempt(const char *current_name, const char *file) {
    if (trace_session_exec_name(current_name))
        return true;
    const char *basename = strrchr(file, '/');
    if (basename == NULL)
        basename = file;
    else
        basename++;
    return trace_session_exec_name(basename);
}

static void trace_exec_argv(const struct exec_args *argv, char *buf, size_t size) {
    if (size == 0)
        return;
    buf[0] = '\0';
    if (argv == NULL || argv->args == NULL || argv->count == 0)
        return;

    size_t used = 0;
    const char *arg = argv->args;
    size_t shown = 0;
    for (size_t i = 0; i < argv->count && shown < 4 && *arg != '\0'; i++) {
        const char *sep = shown == 0 ? "" : " ";
        int wrote = snprintf(buf + used, size - used, "%s\"%.48s\"", sep, arg);
        if (wrote < 0 || (size_t) wrote >= size - used) {
            used = size - 1;
            break;
        }
        used += wrote;
        shown++;
        arg += strlen(arg) + 1;
    }
    if (shown < argv->count && used + 4 < size)
        strcpy(buf + used, " ...");
}

static void trace_exec_tty(struct task *task, int *type_out, int *num_out) {
    int type = -1;
    int num = -1;
    lock(&task->group->lock, 0);
    struct tty *tty = task->group->tty;
    if (tty != NULL) {
        type = tty->type;
        num = tty->num;
    }
    unlock(&task->group->lock);
    if (type_out != NULL)
        *type_out = type;
    if (num_out != NULL)
        *num_out = num;
}

static bool elf_abi_matches(enum guest_abi abi, byte_t bitness, uint16_t machine) {
    switch (abi) {
    case GUEST_ABI_AMD64:
        return bitness == ELF_64BIT && machine == ELF_X86_64;
    case GUEST_ABI_I386:
    default:
        return bitness == ELF_32BIT && machine == ELF_X86;
    }
}

static bool elf_value_fits_addr(qword_t value) {
    return value <= (qword_t) (addr_t) -1;
}

static int read_header(struct fd *fd, enum guest_abi abi, struct elf_info *header) {
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
    if (memcmp(&ident->magic, ELF_MAGIC, sizeof(ident->magic)) != 0
            || (ident->type != ELF_EXECUTABLE && ident->type != ELF_DYNAMIC)
            || ident->endian != ELF_LITTLEENDIAN
            || ident->elfversion1 != 1
            || !elf_abi_matches(abi, ident->bitness, ident->machine))
        return _ENOEXEC;

    if (ident->bitness == ELF_32BIT) {
        *header = (struct elf_info) {
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

static int load_entry(struct elf_prg_info ph, addr_t bias, struct fd *fd) {
    int err;

    if (!elf_value_fits_addr(ph.vaddr) || !elf_value_fits_addr(ph.offset) ||
            !elf_value_fits_addr(ph.memsize) || !elf_value_fits_addr(ph.filesize))
        return _EOVERFLOW;
    if (ph.vaddr > (qword_t) ((addr_t) -1) - bias)
        return _EOVERFLOW;

    addr_t addr = (addr_t) ph.vaddr + bias;
    addr_t offset = (addr_t) ph.offset;
    addr_t memsize = (addr_t) ph.memsize;
    addr_t filesize = (addr_t) ph.filesize;

    int flags = P_READ;
    if (ph.flags & PH_W) flags |= P_WRITE;

    if ((err = fd->ops->mmap(fd, current->mem, PAGE(addr),
                    PAGE_ROUND_UP(filesize + PGOFFSET(addr)),
                    offset - PGOFFSET(addr), flags, MMAP_PRIVATE)) < 0)
        return err;
    // TODO find a better place for these to avoid code duplication
    mem_pt(current->mem, PAGE(addr))->data->fd = fd_retain(fd);
    mem_pt(current->mem, PAGE(addr))->data->file_offset = offset - PGOFFSET(addr);

    if (memsize > filesize) {
        // put zeroes between addr + filesize and addr + memsize, call that bss
        dword_t bss_size = memsize - filesize;

        // first zero the tail from the end of the file mapping to the end
        // of the load entry or the end of the page, whichever comes first
        addr_t file_end = addr + filesize;
        dword_t tail_size = PAGE_SIZE - PGOFFSET(file_end);
        
        if (tail_size == PAGE_SIZE)
            // if you can calculate tail_size better and not have to do this please let me know
            tail_size = 0;

        if (tail_size != 0) {
            // Unlock and lock the mem because the user functions must be
            // called without locking mem.
            write_unlock(&current->mem->lock);
            
            mem_ref_cnt_mod(current->mem, 1);
            user_memset(file_end, 0, tail_size);
            write_lock(&current->mem->lock);
            mem_ref_cnt_mod(current->mem, -1);
        }
        if (tail_size > bss_size)
            tail_size = bss_size;

        // then map the pages from after the file mapping up to and including the end of bss
        if (bss_size - tail_size != 0)
                
        if ((err = pt_map_nothing(current->mem, PAGE_ROUND_UP(addr + filesize),
            PAGE_ROUND_UP(bss_size - tail_size), flags)) < 0)
                
        return err;
    }
    
    return 0;
}

static addr_t find_hole_for_elf(struct elf_info *header, struct elf_prg_info *ph) {
    struct elf_prg_info *first = NULL, *last = NULL;
    for (int i = 0; i < header->phent_count; i++) {
        if (ph[i].type == PT_LOAD) {
            if (first == NULL)
                first = &ph[i];
            last = &ph[i];
        }
    }
    pages_t size = 0;
    if (first != NULL) {
        if (!elf_value_fits_addr(last->vaddr + last->memsize) || !elf_value_fits_addr(first->vaddr))
            return 0;
        pages_t a = PAGE_ROUND_UP((addr_t) (last->vaddr + last->memsize));
        pages_t b = PAGE((addr_t) first->vaddr);
        size = a - b;
    }
    return pt_find_hole(current->mem, size) << PAGE_BITS;
}

static intptr_t elf_exec(struct fd *fd, const char *file, struct exec_args argv, struct exec_args envp) {
    intptr_t err = 0;
    struct task *save = current;
    size_t guest_word_size = task_abi_desc(save).pointer_size;
    bool is_64bit = task_is_64bit(save);
    bool mem_locked = false;

    // read the headers
    struct elf_info header;
    if ((err = read_header(fd, save->abi, &header)) < 0)
        return err;
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
        if ((err = read_header(interp_fd, save->abi, &interp_header)) < 0) {
            if (err == _ENOEXEC)
                err = _ELIBBAD;
            goto out_free_interp;
        }
        if ((err = read_prg_headers(interp_fd, interp_header, &interp_ph)) < 0) {
            if (err == _ENOEXEC)
                err = _ELIBBAD;
            goto out_free_interp;
        }
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
    task_set_mm(save, mm_new(save->abi));
    unlock(&save->general_lock);
    write_lock(&save->mem->lock);
    mem_locked = true;

    save->mm->exefile = fd_retain(fd);

    addr_t load_addr = 0;
    bool load_addr_set = false;
    addr_t bias = 0;

    for (unsigned i = 0; i < header.phent_count; i++) {
        if (ph[i].type != PT_LOAD)
            continue;

        if (!load_addr_set && header.type == ELF_DYNAMIC) {
            if (interp_name)
                bias = 0x56555000;
            else
                bias = find_hole_for_elf(&header, ph);
        }

        if ((err = load_entry(ph[i], bias, fd)) < 0)
            goto beyond_hope;

        if (!load_addr_set) {
            qword_t mapped_load_addr = (qword_t) bias + ph[i].vaddr;
            if (ph[i].offset > mapped_load_addr) {
                err = _EOVERFLOW;
                goto beyond_hope;
            }
            mapped_load_addr -= ph[i].offset;
            if (!elf_value_fits_addr(mapped_load_addr)) {
                err = _EOVERFLOW;
                goto beyond_hope;
            }
            load_addr = (addr_t) mapped_load_addr;
            load_addr_set = true;
        }

        qword_t brk_q = (qword_t) bias + ph[i].vaddr + ph[i].memsize;
        if (!elf_value_fits_addr(brk_q)) {
            err = _EOVERFLOW;
            goto beyond_hope;
        }
        addr_t brk = (addr_t) brk_q;
        if (brk > save->mm->start_brk)
            save->mm->start_brk = save->mm->brk = BYTES_ROUND_UP(brk);
    }

    qword_t entry_q = (qword_t) bias + header.entry_point;
    if (!elf_value_fits_addr(entry_q)) {
        err = _EOVERFLOW;
        goto beyond_hope;
    }
    addr_t entry = (addr_t) entry_q;
    addr_t interp_base = 0;

    if (interp_name) {
        interp_base = find_hole_for_elf(&interp_header, interp_ph);
        for (int i = interp_header.phent_count - 1; i >= 0; i--) {
            if (interp_ph[i].type != PT_LOAD)
                continue;
            if ((err = load_entry(interp_ph[i], interp_base, interp_fd)) < 0)
                goto beyond_hope;
        }
        entry_q = (qword_t) interp_base + interp_header.entry_point;
        if (!elf_value_fits_addr(entry_q)) {
            err = _EOVERFLOW;
            goto beyond_hope;
        }
        entry = (addr_t) entry_q;
    }

    addr_t vdso_entry = 0;
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

    addr_t sp = vm_layout.stack_pointer;
    sp -= guest_word_size;

    err = _EFAULT;
    addr_t file_addr = sp = copy_string(sp, file);
    if (sp == 0)
        goto beyond_hope;
    addr_t envp_addr = sp = args_copy(sp, envp);
    if (sp == 0)
        goto beyond_hope;
    save->mm->env_start = sp;
    save->mm->env_end = sp + args_size(envp);
    addr_t argv_addr = sp = args_copy(sp, argv);
    if (sp == 0)
        goto beyond_hope;
    save->mm->argv_start = sp;
    save->mm->argv_end = sp + args_size(argv);
    sp = align_stack(sp);

    addr_t platform_addr = sp = copy_string(sp, task_abi_desc(save).elf_platform);
    if (sp == 0)
        goto beyond_hope;
    char random[16] = {};
    get_random(random, sizeof(random));
    addr_t random_addr = sp -= sizeof(random);
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

        addr_t p = sp;
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
            argv_addr += user_strlen(argv_addr) + 1;
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
            envp_addr += user_strlen(envp_addr) + 1;
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

        addr_t p = sp;
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
            argv_addr += user_strlen(argv_addr) + 1;
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
            envp_addr += user_strlen(envp_addr) + 1;
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
    save->cpu.esp = sp;
    save->cpu.eip = entry;
    save->cpu.fcw = 0x37f;

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

static inline addr_t align_stack(addr_t sp) {
    return sp &~ 0xf;
}

static inline addr_t copy_string(addr_t sp, const char *string) {
    sp -= strlen(string) + 1;
    if (user_write_string(sp, string))
        return 0;
    return sp;
}

static inline addr_t args_copy(addr_t sp, struct exec_args args) {
    size_t size = args_size(args);
    sp -= size;
    if (user_write(sp, args.args, size))
        return 0;
    return sp;
}

static inline ssize_t user_strlen(size_t p) {
    size_t i = 0;
    char c;
    do {
        if (user_get(p + i, c))
            return -1;
        i++;
    } while (c != '\0');
    return i - 1;
}

static inline int user_memset(addr_t start, byte_t val, dword_t len) {
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
    char current_comm[sizeof(current->comm)];
    lock(&current->general_lock, 0);
    strncpy(current_comm, current->comm, sizeof(current_comm));
    current_comm[sizeof(current_comm) - 1] = '\0';
    unlock(&current->general_lock);

    bool trace_attempt = trace_session_exec_attempt(current_comm, file);
    char argv_trace[256];
    int tty_type = -1;
    int tty_num = -1;
    if (trace_attempt) {
        trace_exec_argv(&argv, argv_trace, sizeof(argv_trace));
        trace_exec_tty(current, &tty_type, &tty_num);
    }

    struct fd *fd = generic_open(file, O_RDONLY, 0);
    if (IS_ERR(fd)) {
        if (trace_attempt) {
            printk("INFO: exec fail pid=%d tgid=%d comm=%s file=%s err=%d tty=%d:%d argv=%s\n",
                   current->pid, current->tgid, current_comm, file, (int) PTR_ERR(fd),
                   tty_type, tty_num, argv_trace);
        }
        return (int) PTR_ERR(fd);
    }

    struct statbuf stat;
    int err = fd->mount->fs->fstat(fd, &stat);
    if (err < 0) {
        fd_close(fd);
        if (trace_attempt) {
            printk("INFO: exec fail pid=%d tgid=%d comm=%s file=%s err=%d tty=%d:%d argv=%s\n",
                   current->pid, current->tgid, current_comm, file, err,
                   tty_type, tty_num, argv_trace);
        }
        return err;
    }

    // if nobody has permission to execute, it should be safe to not execute
    if (!(stat.mode & 0111)) {
        fd_close(fd);
        if (trace_attempt) {
            printk("INFO: exec fail pid=%d tgid=%d comm=%s file=%s err=%d tty=%d:%d argv=%s\n",
                   current->pid, current->tgid, current_comm, file, _EACCES,
                   tty_type, tty_num, argv_trace);
        }
        return _EACCES;
    }

    err = format_exec(fd, file, argv, envp);
    if (err == _ENOEXEC)
        err = shebang_exec(fd, file, argv, envp);
    fd_close(fd);
    if (err < 0) {
        if (trace_attempt) {
            printk("INFO: exec fail pid=%d tgid=%d comm=%s file=%s err=%d tty=%d:%d argv=%s\n",
                   current->pid, current->tgid, current_comm, file, err,
                   tty_type, tty_num, argv_trace);
        }
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

    char old_comm[sizeof(current->comm)];
    strncpy(old_comm, current_comm, sizeof(old_comm));
    old_comm[sizeof(old_comm) - 1] = '\0';

    // save current->comm
    lock(&current->general_lock, 0);
    const char *basename = strrchr(file, '/');
    if (basename == NULL)
        basename = file;
    else
        basename++;
    strncpy(current->comm, basename, sizeof(current->comm));
    current->comm[sizeof(current->comm) - 1] = '\0';
    unlock(&current->general_lock);

    if (trace_session_exec_name(old_comm) || trace_session_exec_name(basename)) {
        printk("INFO: exec session pid=%d tgid=%d old=%s new=%s file=%s tty=%d:%d argv=%s\n",
               current->pid, current->tgid, old_comm, basename, file,
               tty_type, tty_num, argv_trace);
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
        ptrace_event_stop(SIGTRAP_, &(struct siginfo_) {
            .code = SI_USER_,
            .kill.pid = current->pid,
            .kill.uid = current->uid,
        }, PTRACE_EVENT_EXEC_, current->pid);
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

static ssize_t user_read_string_array(addr_t addr, char *buf, size_t max) {
    size_t i = 0;
    size_t p = 0;
    for (;;) {
        addr_t str_addr;
        if (user_get(addr + i * sizeof(addr_t), str_addr))
            return _EFAULT;
        if (str_addr == 0)
            break;
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

    err = do_execve(filename, argc, argv, envp);

    free(envp);
    free(argv);
    return err;
}

ssize_t sys_execveat(fd_t dirfd, addr_t filename_addr, addr_t argv_addr, addr_t envp_addr, int_t flags) {
    if (flags & ~(AT_EMPTY_PATH_ | AT_SYMLINK_NOFOLLOW_))
        return _EINVAL;

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
    err = do_execve(resolved, argc, argv, envp);

out_free_args:
    free(envp);
    free(argv);
    return err;
}

static ssize_t read_execve_user_args(addr_t argv_addr, addr_t envp_addr, ssize_t *argc_out,
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
