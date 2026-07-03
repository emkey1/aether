#ifndef KERNEL_ABI_ARM64_H
#define KERNEL_ABI_ARM64_H

#include "misc.h"

typedef qword_t arm64_guest_addr_t;
typedef qword_t arm64_guest_word_t;
typedef sqword_t arm64_guest_sword_t;
typedef qword_t arm64_guest_reg_t;
typedef qword_t arm64_guest_ulong_t;
typedef sqword_t arm64_guest_long_t;
typedef qword_t arm64_guest_size_t;
typedef sqword_t arm64_guest_ssize_t;

struct arm64_iovec_ {
    arm64_guest_addr_t base;
    arm64_guest_size_t len;
};

static_assert(sizeof(struct arm64_iovec_) == 16, "arm64_iovec size");

#endif
