// pidfd_open (and the pidfd/io_uring stub family) must not SIGSYS on amd64.
//
// pidfd_open (#434) is an ENOSYS stub (login/PAM fall back to waitpid; io_uring
// and pidfd_send_signal likewise degrade to ordinary syscalls). But on amd64 it
// was in the syscall table yet absent from amd64_syscall_legacy_arg_count, so it
// fell to the default 6-arg classification: the marshaller validated unused
// upper arg registers and, when one held a value with high bits set (not a
// sign-extended -1), raised SIGSYS ("needs full-width args") -- killing login
// (exit 159). Classifying these stubs as 0-arg makes them return their ENOSYS
// cleanly so callers fall back.
//
// This test passes garbage with high bits in the unused arg registers to force
// the old failure; it must return cleanly (ENOSYS, or a success if ever
// implemented), never be killed by SIGSYS. Arch-neutral.
#define _GNU_SOURCE
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <sys/syscall.h>
#include "test_common.h"

#ifndef SYS_pidfd_open
# ifdef __NR_pidfd_open
#  define SYS_pidfd_open __NR_pidfd_open
# else
#  define SYS_pidfd_open 434 // "common" number on i386 and amd64
# endif
#endif

#define GARBAGE 0x1ffffffffULL // high bits set, not a sign-extended -1

static void check(const char *name, long r, int e) {
    if (r >= 0)
        test_logf("%s -> %ld (implemented)\n", name, r);
    else if (e == ENOSYS || e == EOPNOTSUPP)
        test_logf("%s -> -1 %s (callers fall back) ok\n", name, strerror(e));
    else {
        printf("FAIL: %s -> -1 %s (want ENOSYS or success)\n", name, strerror(e));
        failures_total++;
    }
}

int main(int argc, char **argv) {
    test_init(argc, argv);

    // pidfd_open(getpid(), 0), but stuff garbage into the 4 unused arg registers
    // so the marshaller would reject them under the old default-6 classification.
    errno = 0;
    long r = syscall(SYS_pidfd_open, (long) getpid(), 0L, GARBAGE, GARBAGE, GARBAGE, GARBAGE);
    check("pidfd_open", r, errno);

    if (r >= 0)
        close((int) r);

    return finish_suite("pidfd_open");
}
