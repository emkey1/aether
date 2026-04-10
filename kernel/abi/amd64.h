#ifndef KERNEL_ABI_AMD64_H
#define KERNEL_ABI_AMD64_H

#include "misc.h"

typedef qword_t amd64_guest_addr_t;
typedef qword_t amd64_guest_word_t;
typedef sqword_t amd64_guest_sword_t;
typedef qword_t amd64_guest_reg_t;
typedef qword_t amd64_guest_ulong_t;
typedef sqword_t amd64_guest_long_t;
typedef qword_t amd64_guest_size_t;
typedef sqword_t amd64_guest_ssize_t;

struct amd64_iovec_ {
    amd64_guest_addr_t base;
    amd64_guest_size_t len;
};

static_assert(sizeof(struct amd64_iovec_) == 16, "amd64_iovec size");

#endif
