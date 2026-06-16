// amd64 sendfile (64-bit count) and vhangup must not crash / be "missing".
//
// Two amd64 syscall-table gaps seen finishing the Devuan install:
//   - sendfile (#40): systemd's file copy falls back to sendfile after
//     copy_file_range returns ENOSYS, passing a 64-bit size_t count. sendfile
//     was classified as a 4-arg call, so the full-width arg marshaller rejected
//     the wide count -> SIGSYS ("Bad system call"). It is a stub returning
//     EINVAL (systemd then falls back to read/write); it just must not SIGSYS.
//   - vhangup (#153): login calls it to reset the controlling tty; amd64 had no
//     table entry ("missing syscall 153"). It is now a success (no-op) stub.
//
// Test requires only a clean return, never SIGSYS: sendfile -> EINVAL/ENOSYS
// (or success); vhangup -> 0 (or EPERM). Arch-neutral via the SYS_* numbers.
#define _GNU_SOURCE
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include "test_common.h"

#ifndef SYS_sendfile
# ifdef __NR_sendfile
#  define SYS_sendfile __NR_sendfile
# elif defined(__x86_64__)
#  define SYS_sendfile 40
# else
#  define SYS_sendfile 187
# endif
#endif
#ifndef SYS_vhangup
# ifdef __NR_vhangup
#  define SYS_vhangup __NR_vhangup
# elif defined(__x86_64__)
#  define SYS_vhangup 153
# else
#  define SYS_vhangup 111
# endif
#endif

int main(int argc, char **argv) {
    test_init(argc, argv);

    // sendfile with a 64-bit count -- the systemd copy fallback. Reaching the
    // line after the syscall at all proves no SIGSYS.
    const char *sp = "/tmp/sendfile_vhangup.src", *dp = "/tmp/sendfile_vhangup.dst";
    int s = open(sp, O_RDWR | O_CREAT | O_TRUNC, 0600);
    int d = open(dp, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (s >= 0) (void) write(s, "x", 1);
    errno = 0;
    long r = syscall(SYS_sendfile, d, s, (void *) 0, (size_t) 0x7fffffffffffffffULL);
    int e = errno;
    if (r >= 0)
        test_logf("sendfile returned %ld (implemented)\n", r);
    else if (e == EINVAL || e == ENOSYS || e == EOPNOTSUPP)
        test_logf("sendfile -> -1 %s (callers fall back) ok\n", strerror(e));
    else {
        printf("FAIL: sendfile -> -1 %s (want EINVAL/ENOSYS or success)\n", strerror(e));
        failures_total++;
    }
    if (s >= 0) close(s);
    if (d >= 0) close(d);
    unlink(sp);
    unlink(dp);

    // vhangup -- must be wired (not "missing syscall"); no-op stub succeeds.
    errno = 0;
    r = syscall(SYS_vhangup);
    e = errno;
    if (r == 0)
        test_logf("vhangup -> 0 ok\n");
    else if (e == EPERM)
        test_logf("vhangup -> -1 EPERM (acceptable)\n");
    else {
        printf("FAIL: vhangup -> %ld %s (want 0)\n", r, r < 0 ? strerror(e) : "");
        failures_total++;
    }

    return finish_suite("sendfile_vhangup");
}
