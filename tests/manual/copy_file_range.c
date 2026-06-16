// copy_file_range(): real data-copy behavior, including offset semantics.
//
// History: copy_file_range was a stub. It first SIGSYS'd on amd64 because its
// 64-bit size_t len tripped the legacy arg marshaller; it is now implemented
// natively (amd64 #326 via the 64-bit native-syscall path; i386 #377), doing a
// real read/write copy with correct off_in/off_out handling. systemd-sysusers
// copies /etc/passwd-style files this way.
//
// Verifies: whole-file copy with NULL offsets (advances both positions); an
// explicit in-offset copies a sub-range, updates *off_in, and leaves the input
// file position unchanged. Arch-neutral.
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

static const char MSG[] = "HELLO COPY FILE RANGE WORLD"; // 27 bytes

static long cfr(int in, long long *in_off, int out, long long *out_off, size_t len) {
    return syscall(SYS_copy_file_range, in, in_off, out, out_off, len, 0u);
}

static int make_file(const char *path, const char *data, size_t n) {
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return -1;
    if (n && write(fd, data, n) != (ssize_t) n) { close(fd); return -1; }
    lseek(fd, 0, SEEK_SET);
    return fd;
}

// NULL offsets: copy the whole file and advance both file positions.
static void test_whole_copy(void) {
    size_t mlen = strlen(MSG);
    char buf[128];
    int s = make_file("/tmp/cfr.src", MSG, mlen);
    int d = open("/tmp/cfr.dst", O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (s < 0 || d < 0) { printf("FAIL: open: %s\n", strerror(errno)); failures_total++; goto out; }

    errno = 0;
    long r = cfr(s, NULL, d, NULL, mlen);
    if (r != (long) mlen) {
        printf("FAIL: copy_file_range whole: r=%ld (%s), want %zu\n", r, r < 0 ? strerror(errno) : "", mlen);
        failures_total++; goto out;
    }
    ssize_t got = pread(d, buf, sizeof buf - 1, 0);
    if (got >= 0) buf[got] = '\0';
    if (got != (ssize_t) mlen || strcmp(buf, MSG) != 0) {
        printf("FAIL: dest content wrong: \"%s\"\n", buf); failures_total++;
    }
    if (lseek(s, 0, SEEK_CUR) != (off_t) mlen) { printf("FAIL: src pos not advanced\n"); failures_total++; }
    if (lseek(d, 0, SEEK_CUR) != (off_t) mlen) { printf("FAIL: dst pos not advanced\n"); failures_total++; }
    test_logf("whole-file copy ok (%ld bytes)\n", r);
out:
    if (s >= 0) close(s);
    if (d >= 0) close(d);
    unlink("/tmp/cfr.src"); unlink("/tmp/cfr.dst");
}

// Explicit in-offset: copy a sub-range, update *off_in, leave the input file
// position unchanged.
static void test_offset_copy(void) {
    size_t mlen = strlen(MSG);
    char buf[128];
    int s = make_file("/tmp/cfr.src", MSG, mlen);
    int d = open("/tmp/cfr.dst2", O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (s < 0 || d < 0) { printf("FAIL: open2: %s\n", strerror(errno)); failures_total++; goto out; }

    lseek(s, 3, SEEK_SET);       // marker that must NOT move
    long long in_off = 6;        // copy from offset 6 to end
    size_t want = mlen - 6;
    errno = 0;
    long r = cfr(s, &in_off, d, NULL, want);
    if (r != (long) want) {
        printf("FAIL: copy_file_range sub-range: r=%ld (%s), want %zu\n", r, r < 0 ? strerror(errno) : "", want);
        failures_total++; goto out;
    }
    ssize_t got = pread(d, buf, sizeof buf - 1, 0);
    if (got >= 0) buf[got] = '\0';
    if (got != (ssize_t) want || strcmp(buf, MSG + 6) != 0) {
        printf("FAIL: sub-range content wrong: \"%s\"\n", buf); failures_total++;
    }
    if (in_off != (long long) mlen) { printf("FAIL: in_off=%lld, want %zu\n", in_off, mlen); failures_total++; }
    if (lseek(s, 0, SEEK_CUR) != 3) { printf("FAIL: src pos moved (should stay 3)\n"); failures_total++; }
    test_logf("offset copy ok (in_off->%lld, src pos kept at 3)\n", in_off);
out:
    if (s >= 0) close(s);
    if (d >= 0) close(d);
    unlink("/tmp/cfr.src"); unlink("/tmp/cfr.dst2");
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    test_whole_copy();
    test_offset_copy();
    return finish_suite("copy_file_range");
}
