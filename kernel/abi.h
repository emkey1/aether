#ifndef KERNEL_ABI_H
#define KERNEL_ABI_H

#include "misc.h"
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

static inline const char *guest_abi_name(enum guest_abi abi) {
    return guest_abi_desc(abi).name;
}

static inline bool guest_abi_is_64bit(enum guest_abi abi) {
    return guest_abi_desc(abi).pointer_size == sizeof(amd64_guest_addr_t);
}

#endif
