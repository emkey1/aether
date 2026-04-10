#ifndef KERNEL_ABI_I386_H
#define KERNEL_ABI_I386_H

#include "misc.h"

typedef addr_t i386_guest_addr_t;
typedef uint_t i386_guest_word_t;
typedef int_t i386_guest_sword_t;
typedef dword_t i386_guest_reg_t;
typedef dword_t i386_guest_ulong_t;
typedef sdword_t i386_guest_long_t;
typedef dword_t i386_guest_size_t;
typedef sdword_t i386_guest_ssize_t;

struct i386_iovec_ {
    i386_guest_addr_t base;
    i386_guest_size_t len;
};

static_assert(sizeof(struct i386_iovec_) == 8, "i386_iovec size");

#endif
