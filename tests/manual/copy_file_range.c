// copy_file_range with a 64-bit length must not SIGSYS on amd64.
//
// Regression for the amd64 syscall arg marshaller. copy_file_range (amd64 #326)
// was classified as a 6-arg call and run through the generic full-width check,
// which rejects any of the first N args whose upper 32 bits are set and raises
// SIGSYS ("Bad system call"). systemd-sysusers calls copy_file_range with
// len = SIZE_MAX to copy a whole file; on amd64 size_t is 64-bit, so that len
// tripped the check and killed the process -- openssh-server/polkitd user
// creation died with exit 159 (128+SIGSYS). (On i386 size_t is 32-bit, so the
// existing handler never saw a wide len.)
//
// The handler is a stub; it now returns ENOSYS (not EPERM) so callers fall back
// to a read/write copy. This test only requires that the call RETURNS (no
// SIGSYS) -- ENOSYS, or a real success, are both acceptable.
#define _GNU_SOURCE
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <sys/syscall.h>
#include "test_common.h"

#ifndef SYS_copy_file_range
# ifdef __NR_copy_file_range
#  define SYS_copy_file_range __NR_copy_file_range
# elif defined(__x86_64__)
#  define SYS_copy_file_range 326
# else
#  define SYS_copy_file_range 377
# endif
#endif

int main(int argc, char **argv) {
    test_init(argc, argv);
    const char *sp = "/tmp/copy_file_range.src";
    const char *dp = "/tmp/copy_file_range.dst";

    int s = open(sp, O_RDWR | O_CREAT | O_TRUNC, 0600);
    int d = open(dp, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (s < 0 || d < 0) {
        printf("FAIL: open: %s\n", strerror(errno));
        failures_total++;
        return finish_suite("copy_file_range");
    }
    (void) write(s, "hello", 5);
    lseek(s, 0, SEEK_SET);

    // The systemd-sysusers pattern: a huge length to "copy everything". Use a
    // large *positive* value (~SSIZE_MAX), not (size_t)-1 -- the marshaller
    // treats all-ones as a valid sign-extended -1 and lets it through, whereas
    // a value with high bits set but != -1 is exactly what tripped the amd64
    // full-width check and raised SIGSYS. (On i386, size_t is 32-bit so this
    // truncates harmlessly.) Raw syscall so glibc doesn't reshape the args;
    // reaching the next line at all proves no SIGSYS.
    size_t big_len = (size_t) 0x7fffffffffffffffULL;
    errno = 0;
    long r = syscall(SYS_copy_file_range, s, (void *) 0, d, (void *) 0, big_len, 0u);
    int e = errno;

    if (r >= 0) {
        test_logf("copy_file_range returned %ld (implemented)\n", r);
    } else if (e == ENOSYS) {
        test_logf("copy_file_range -> -1 ENOSYS (callers fall back) ok\n");
    } else {
        printf("FAIL: copy_file_range -> -1 %s (want ENOSYS or success)\n", strerror(e));
        failures_total++;
    }

    close(s);
    close(d);
    unlink(sp);
    unlink(dp);
    return finish_suite("copy_file_range");
}
