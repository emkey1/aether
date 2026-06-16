// amd64 sendfile real copy (+ no SIGSYS on 64-bit count), and vhangup wired.
//
// sendfile (#40 amd64 / #187 i386) was a stub that first SIGSYS'd on amd64
// because its 64-bit size_t count tripped the legacy arg marshaller. It is now
// implemented natively (shared engine with copy_file_range) and actually copies
// bytes; this is systemd's copy fallback path. vhangup (#153 amd64 / #111 i386)
// had no table entry ("missing syscall"); it is now a success no-op stub that
// login uses to reset the controlling tty.
//
// Verifies sendfile with a NULL offset copies the data and advances positions
// (offset semantics are covered by copy_file_range.c's shared engine), and that
// vhangup returns cleanly. Arch-neutral.
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

static const char MSG[] = "SENDFILE MOVES THESE BYTES";

static void test_sendfile(void) {
    size_t mlen = strlen(MSG);
    char buf[128];
    int s = open("/tmp/sf.src", O_RDWR | O_CREAT | O_TRUNC, 0600);
    int d = open("/tmp/sf.dst", O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (s < 0 || d < 0) { printf("FAIL: open: %s\n", strerror(errno)); failures_total++; goto out; }
    if (write(s, MSG, mlen) != (ssize_t) mlen) { printf("FAIL: write src\n"); failures_total++; goto out; }
    lseek(s, 0, SEEK_SET);

    errno = 0;
    // NULL offset: read from src's current position, write to dst, advance both.
    long r = syscall(SYS_sendfile, d, s, (void *) 0, (size_t) mlen);
    if (r != (long) mlen) {
        printf("FAIL: sendfile: r=%ld (%s), want %zu\n", r, r < 0 ? strerror(errno) : "", mlen);
        failures_total++; goto out;
    }
    ssize_t got = pread(d, buf, sizeof buf - 1, 0);
    if (got >= 0) buf[got] = '\0';
    if (got != (ssize_t) mlen || strcmp(buf, MSG) != 0) {
        printf("FAIL: sendfile dest content wrong: \"%s\"\n", buf); failures_total++;
    }
    if (lseek(s, 0, SEEK_CUR) != (off_t) mlen) { printf("FAIL: sendfile src pos not advanced\n"); failures_total++; }
    test_logf("sendfile copy ok (%ld bytes)\n", r);
out:
    if (s >= 0) close(s);
    if (d >= 0) close(d);
    unlink("/tmp/sf.src"); unlink("/tmp/sf.dst");
}

static void test_vhangup(void) {
    errno = 0;
    long r = syscall(SYS_vhangup);
    if (r == 0)
        test_logf("vhangup -> 0 ok\n");
    else if (errno == EPERM)
        test_logf("vhangup -> -1 EPERM (acceptable)\n");
    else {
        printf("FAIL: vhangup -> %ld %s (want 0)\n", r, r < 0 ? strerror(errno) : "");
        failures_total++;
    }
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    test_sendfile();
    test_vhangup();
    return finish_suite("sendfile_vhangup");
}
