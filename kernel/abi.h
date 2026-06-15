#ifndef KERNEL_ABI_H
#define KERNEL_ABI_H

#include "misc.h"
#include "emu/mmu.h"
#include "kernel/abi/i386.h"
#include "kernel/abi/amd64.h"

enum guest_abi {
    GUEST_ABI_I386 = 0,
    GUEST_ABI_AMD64 = 1,
};

struct guest_abi_desc {
    enum guest_abi abi;
    const char *name;
    const char *uname_machine;
    const char *elf_platform;
    size_t pointer_size;
    size_t word_size;
    size_t reg_size;
};

static inline struct guest_abi_desc guest_abi_desc(enum guest_abi abi) {
    switch (abi) {
    case GUEST_ABI_AMD64:
        return (struct guest_abi_desc) {
            .abi = abi,
            .name = "amd64",
            .uname_machine = "x86_64",
            .elf_platform = "x86_64",
            .pointer_size = sizeof(amd64_guest_addr_t),
            .word_size = sizeof(amd64_guest_word_t),
            .reg_size = sizeof(amd64_guest_reg_t),
        };
    case GUEST_ABI_I386:
    default:
        return (struct guest_abi_desc) {
            .abi = GUEST_ABI_I386,
            .name = "i386",
            .uname_machine = "i686",
            .elf_platform = "i686",
            .pointer_size = sizeof(i386_guest_addr_t),
            .word_size = sizeof(i386_guest_word_t),
            .reg_size = sizeof(i386_guest_reg_t),
        };
    }
}

struct guest_vm_layout {
    enum guest_abi abi;
    page_t page_limit;
    page_t mmap_floor;
    page_t mmap_ceiling;
    qword_t user_addr_max;
    page_t stack_page;
    guest_addr_t stack_pointer;
};

// Scalar accessor for the one field the hot address-validation path needs.
// guest_abi_vm_layout() below returns a ~40-byte struct by value, which the
// compiler will not fold away — materializing the whole thing just to read
// user_addr_max showed up as a large slice of the amd64 interpreter's time, since
// guest_abi_range_valid() runs on every instruction fetch and memory access.
// This is the single source of truth for the value (the struct uses it too).
static inline qword_t guest_abi_user_addr_max(enum guest_abi abi) {
    return abi == GUEST_ABI_AMD64 ? ((qword_t) 1 << 47) : ((qword_t) 1 << 32);
}

static inline struct guest_vm_layout guest_abi_vm_layout(enum guest_abi abi) {
    switch (abi) {
    case GUEST_ABI_AMD64:
        return (struct guest_vm_layout) {
            .abi = abi,
            // 128 TiB canonical user range for 4 KiB guest pages.
            .page_limit = (page_t) 1 << 35,
            // Let amd64 mappings use the full internal 47-bit guest window.
            // Keep the initial exec stack low for now because most syscall
            // pointer marshalling is still 32-bit.
            .mmap_floor = (page_t) 0x1000,
            .mmap_ceiling = ((page_t) 1 << 35) - 0x2000,
            .user_addr_max = guest_abi_user_addr_max(abi),
            .stack_page = (page_t) 0xffffe,
            .stack_pointer = 0xfffff000u,
        };
    case GUEST_ABI_I386:
    default:
        return (struct guest_vm_layout) {
            .abi = GUEST_ABI_I386,
            .page_limit = (page_t) 1 << 20,
            .mmap_floor = (page_t) 0x40000,
            .mmap_ceiling = (page_t) 0xf7ffe,
            .user_addr_max = guest_abi_user_addr_max(abi),
            .stack_page = (page_t) 0xffffd,
            .stack_pointer = 0xffffe000u,
        };
    }
}

static inline const char *guest_abi_name(enum guest_abi abi) {
    return guest_abi_desc(abi).name;
}

static inline bool guest_abi_is_64bit(enum guest_abi abi) {
    return guest_abi_desc(abi).pointer_size == sizeof(amd64_guest_addr_t);
}

static inline bool guest_abi_addr_valid(enum guest_abi abi, qword_t addr) {
    return addr < guest_abi_user_addr_max(abi);
}

static inline bool guest_abi_range_valid(enum guest_abi abi, qword_t addr, qword_t size) {
    qword_t max = guest_abi_user_addr_max(abi);
    if (addr >= max)
        return false;
    if (size == 0)
        return true;
    return size <= max - addr;
}

#endif
