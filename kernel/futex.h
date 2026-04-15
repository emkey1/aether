#ifndef KERNEL_FUTEX_H
#define KERNEL_FUTEX_H

int futex_wake(guest_addr_t uaddr, dword_t val);

#endif
