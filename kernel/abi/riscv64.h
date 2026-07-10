#ifndef KERNEL_ABI_RISCV64_H
#define KERNEL_ABI_RISCV64_H

#include "misc.h"

// riscv64 (RV64, LP64D) is an asm-generic Linux ABI like arm64; the types
// are deliberately identical to kernel/abi/arm64.h.
typedef qword_t riscv64_guest_addr_t;
typedef qword_t riscv64_guest_word_t;
typedef sqword_t riscv64_guest_sword_t;
typedef qword_t riscv64_guest_reg_t;
typedef qword_t riscv64_guest_ulong_t;
typedef sqword_t riscv64_guest_long_t;
typedef qword_t riscv64_guest_size_t;
typedef sqword_t riscv64_guest_ssize_t;

struct riscv64_iovec_ {
    riscv64_guest_addr_t base;
    riscv64_guest_size_t len;
};

static_assert(sizeof(struct riscv64_iovec_) == 16, "riscv64_iovec size");

#endif
