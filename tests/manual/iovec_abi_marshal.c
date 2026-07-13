// iovec ABI marshaling: readv/writev/preadv/pwritev/process_vm_readv all
// funnel through kernel/user.c's user_read_iovecs_abi() to decode the
// guest's struct iovec array. That function used to special-case only
// `abi == GUEST_ABI_AMD64` for the 64-bit {ptr, size_t} layout and fell back
// to the 32-bit i386 layout for everything else -- including arm64 and
// riscv64, which are 64-bit ABIs with the SAME iovec layout as amd64. On
// those two guests every multi-word read of the iovec array was done at
// half the real stride (8 bytes instead of 16), so iov_len ended up holding
// the upper 32 bits of iov_base (usually 0 for small addresses) and, for
// iovcnt > 1, every entry past the first was parsed from the wrong offset
// entirely. The visible symptom: a real `strace` tracing an arm64/riscv64
// process showed every path/struct/array syscall argument as empty or
// zeroed (still readable via ptrace(2) PEEKDATA, which doesn't go through
// this path) while numeric return values stayed correct. Fixed by checking
// guest_abi_is_64bit(abi) instead of GUEST_ABI_AMD64 specifically.
//
// Verifies: multi-entry (2-iovec) readv/writev round-trip real data, and
// process_vm_readv reads real data from a 2-iovec remote array (self-to-self,
// since guest-only tests have no separate tracee to attach to). Arch-neutral;
// on i386/amd64 this exercises the already-correct paths as a regression
// guard. process_vm_readv is currently only wired for i386/amd64/arm64/riscv64
// (kernel/calls.c's per-ABI syscall tables), so that half is skipped, not
// failed, if the syscall isn't available.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>
#include "test_common.h"

#ifndef SYS_process_vm_readv
# ifdef __NR_process_vm_readv
#  define SYS_process_vm_readv __NR_process_vm_readv
# endif
#endif

static void test_readv_writev(void) {
    char a[] = "HELLO";
    char b[] = "WORLD!";
    struct iovec iov[2] = {
        { .iov_base = a, .iov_len = 5 },
        { .iov_base = b, .iov_len = 6 },
    };
    int fd = open("/tmp/iovec_abi_marshal.dat", O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) { printf("FAIL: open(w): %s\n", strerror(errno)); failures_total++; return; }
    ssize_t n = writev(fd, iov, 2);
    close(fd);
    if (n != 11) {
        printf("FAIL: writev returned %zd, want 11\n", n);
        failures_total++;
        return;
    }
    test_logf("writev ok: %zd bytes\n", n);

    char buf1[8] = {0}, buf2[8] = {0};
    struct iovec riov[2] = {
        { .iov_base = buf1, .iov_len = 5 },
        { .iov_base = buf2, .iov_len = 6 },
    };
    fd = open("/tmp/iovec_abi_marshal.dat", O_RDONLY);
    if (fd < 0) { printf("FAIL: open(r): %s\n", strerror(errno)); failures_total++; return; }
    n = readv(fd, riov, 2);
    close(fd);
    if (n != 11 || strcmp(buf1, "HELLO") != 0 || strcmp(buf2, "WORLD!") != 0) {
        printf("FAIL: readv n=%zd buf1=\"%s\" buf2=\"%s\", want 11/HELLO/WORLD!\n", n, buf1, buf2);
        failures_total++;
        return;
    }
    test_logf("readv ok: buf1=%s buf2=%s\n", buf1, buf2);
}

static void test_process_vm_readv(void) {
#ifdef SYS_process_vm_readv
    char src1[] = "ALPHA";
    char src2[] = "BETA1";
    char dst1[8] = {0}, dst2[8] = {0};
    struct iovec local[2] = {
        { .iov_base = dst1, .iov_len = 5 },
        { .iov_base = dst2, .iov_len = 5 },
    };
    struct iovec remote[2] = {
        { .iov_base = src1, .iov_len = 5 },
        { .iov_base = src2, .iov_len = 5 },
    };
    errno = 0;
    ssize_t r = syscall(SYS_process_vm_readv, getpid(), local, 2, remote, 2, 0);
    if (r < 0 && errno == ENOSYS) {
        test_logf("process_vm_readv: ENOSYS on this ABI, skipping\n");
        return;
    }
    if (r != 10 || strcmp(dst1, "ALPHA") != 0 || strcmp(dst2, "BETA1") != 0) {
        printf("FAIL: process_vm_readv r=%zd (%s) dst1=\"%s\" dst2=\"%s\", want 10/ALPHA/BETA1\n",
               r, r < 0 ? strerror(errno) : "", dst1, dst2);
        failures_total++;
        return;
    }
    test_logf("process_vm_readv ok: dst1=%s dst2=%s\n", dst1, dst2);
#else
    test_logf("process_vm_readv: SYS_process_vm_readv not defined, skipping\n");
#endif
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    test_readv_writev();
    test_process_vm_readv();
    return finish_suite("iovec_abi_marshal");
}
