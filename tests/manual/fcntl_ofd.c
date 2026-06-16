// Open file description lock (F_OFD_GETLK / F_OFD_SETLK / F_OFD_SETLKW) test.
//
// Regression for the missing OFD-lock commands. The emulator handled the
// process POSIX locks (F_SETLK/F_GETLK/F_SETLKW and their 64-bit variants) but
// returned EINVAL for the OFD commands (36/37/38). systemd-sysusers'
// take_etc_passwd_lock() uses fcntl(fd, F_OFD_SETLKW, ...), so installing a
// package whose user is created that way (e.g. openssh-server on Devuan amd64)
// failed with "Failed to take /etc/passwd lock: Invalid argument".
//
// OFD locks are byte-range record locks like POSIX locks, but owned by the
// open file description rather than the process. This test pins down the parts
// that distinguish the two:
//   - separate open()s in ONE process conflict (POSIX locks would not),
//   - a dup()'d fd shares the lock (same open file description),
//   - the lock survives while any dup of the description is open and is
//     released only on the last close,
//   - F_OFD_GETLK reports a conflicting OFD lock with l_pid == -1,
//   - F_OFD_SETLKW blocks and is woken when the holder releases.
//
// Arch-neutral: identical results on i386 and amd64. Also passes on real Linux.
#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include "test_common.h"

#ifndef F_OFD_GETLK
#define F_OFD_GETLK 36
#define F_OFD_SETLK 37
#define F_OFD_SETLKW 38
#endif

static void fail(const char *msg) {
    printf("FAIL: %s\n", msg);
    failures_total++;
}

// Place/query a whole-or-partial lock; returns fcntl()'s result (errno set on -1).
static int do_lock(int fd, int cmd, int type, long long start, long long len) {
    struct flock fl;
    memset(&fl, 0, sizeof fl);
    fl.l_type = type;
    fl.l_whence = SEEK_SET;
    fl.l_start = start;
    fl.l_len = len;
    return fcntl(fd, cmd, &fl);
}

static int conflict_errno(int rc) {
    // A non-blocking SETLK that cannot be granted fails with EAGAIN (or EACCES).
    return rc < 0 && (errno == EAGAIN || errno == EACCES);
}

// Probe whether some other lock would block a write lock on [start,len).
// Returns the reported l_type and, via out_pid, the holder pid field.
static int probe(int fd, long long start, long long len, int *out_pid) {
    struct flock fl;
    memset(&fl, 0, sizeof fl);
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = start;
    fl.l_len = len;
    if (fcntl(fd, F_OFD_GETLK, &fl) < 0)
        return -1;
    if (out_pid)
        *out_pid = fl.l_pid;
    return fl.l_type;
}

// Smoke test: take a lock and confirm the command is implemented at all. On an
// unfixed build F_OFD_SETLK returns EINVAL here, making the failure obvious.
static int test_supported(const char *path) {
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { perror("open"); fail("open"); return 0; }
    if (do_lock(fd, F_OFD_SETLK, F_WRLCK, 0, 0) < 0) {
        printf("FAIL: F_OFD_SETLK not supported: %s\n", strerror(errno));
        failures_total++;
        close(fd);
        return 0;
    }
    test_logf("F_OFD_SETLK accepted\n");
    close(fd);
    return 1;
}

// F_OFD_GETLK from a different open file description must see the conflict and
// report l_pid == -1 (OFD locks are not owned by a process).
static void test_getlk_reports_minus_one(const char *path) {
    int holder = open(path, O_RDWR | O_CREAT, 0644);
    if (do_lock(holder, F_OFD_SETLK, F_WRLCK, 4096, 1024) < 0) {
        fail("holder F_OFD_SETLK"); close(holder); return;
    }
    int querier = open(path, O_RDWR);
    int pid = 0;
    int t = probe(querier, 4096, 1, &pid);
    test_logf("F_OFD_GETLK inside range -> type=%d l_pid=%d\n", t, pid);
    if (t < 0) fail("F_OFD_GETLK failed");
    else if (t != F_WRLCK) fail("F_OFD_GETLK did not report the conflicting lock");
    else if (pid != -1) {
        printf("FAIL: F_OFD_GETLK l_pid=%d, want -1\n", pid);
        failures_total++;
    }
    int outside = probe(querier, 0, 1, NULL);
    if (outside != F_UNLCK) fail("byte outside the locked range reported locked");
    close(querier);
    close(holder);
}

// The defining property: two separate open()s in the SAME process are distinct
// open file descriptions and therefore conflict. (Process POSIX locks from one
// process never conflict with each other -- so this catches a per-process owner.)
static void test_separate_opens_conflict(const char *path) {
    int fd1 = open(path, O_RDWR | O_CREAT, 0644);
    int fd2 = open(path, O_RDWR);
    if (do_lock(fd1, F_OFD_SETLK, F_WRLCK, 0, 0) < 0) {
        fail("fd1 F_OFD_SETLK"); close(fd1); close(fd2); return;
    }
    errno = 0;
    int rc = do_lock(fd2, F_OFD_SETLK, F_WRLCK, 0, 0);
    test_logf("second open F_OFD_SETLK -> rc=%d errno=%d (%s)\n", rc, errno, strerror(errno));
    if (rc == 0)
        fail("two separate open file descriptions did not conflict (owner is per-process, not per-fd)");
    else if (!conflict_errno(rc)) {
        printf("FAIL: second F_OFD_SETLK failed with %s, want EAGAIN/EACCES\n", strerror(errno));
        failures_total++;
    }
    close(fd1);
    close(fd2);
}

// A dup()'d fd refers to the same open file description, so locking through it
// is compatible with the existing lock rather than conflicting.
static void test_dup_shares(const char *path) {
    int fd = open(path, O_RDWR | O_CREAT, 0644);
    if (do_lock(fd, F_OFD_SETLK, F_WRLCK, 0, 0) < 0) {
        fail("fd F_OFD_SETLK (dup test)"); close(fd); return;
    }
    int dupfd = dup(fd);
    if (dupfd < 0) { perror("dup"); fail("dup"); close(fd); return; }
    if (do_lock(dupfd, F_OFD_SETLK, F_WRLCK, 0, 0) < 0) {
        printf("FAIL: re-locking through a dup conflicted with itself: %s\n", strerror(errno));
        failures_total++;
    }
    close(dupfd);
    close(fd);
}

// The lock is released only when the LAST reference to the open file
// description is closed, not on the first close of a dup.
static void test_release_on_last_close(const char *path) {
    int fd = open(path, O_RDWR | O_CREAT, 0644);
    int dupfd = dup(fd);
    if (do_lock(fd, F_OFD_SETLK, F_WRLCK, 0, 0) < 0) {
        fail("fd F_OFD_SETLK (close test)"); close(fd); close(dupfd); return;
    }
    close(fd); // dupfd still references the open file description -> lock persists

    int other = open(path, O_RDWR);
    errno = 0;
    int rc = do_lock(other, F_OFD_SETLK, F_WRLCK, 0, 0);
    if (rc == 0)
        fail("lock vanished while a dup of the open file description was still open");
    else if (!conflict_errno(rc)) {
        printf("FAIL: expected EAGAIN while a dup was open, got %s\n", strerror(errno));
        failures_total++;
    }
    close(other);

    close(dupfd); // last reference -> lock released by fd teardown

    int fresh = open(path, O_RDWR);
    if (do_lock(fresh, F_OFD_SETLK, F_WRLCK, 0, 0) < 0) {
        printf("FAIL: lock not released after last close: %s\n", strerror(errno));
        failures_total++;
    }
    close(fresh);
}

// F_OFD_SETLKW must block on a conflicting lock and wake when it is released.
// The child uses its own open() (a separate description) so it genuinely
// contends with the parent; the parent releases with an explicit F_UNLCK.
static void test_blocking_wake(const char *path) {
    const char *got = "/tmp/fcntl_ofd.got";
    unlink(got);
    int fd = open(path, O_RDWR | O_CREAT, 0644);
    if (do_lock(fd, F_OFD_SETLK, F_WRLCK, 0, 0) < 0) {
        fail("parent F_OFD_SETLK (blocking test)"); close(fd); return;
    }

    pid_t pid = fork();
    if (pid < 0) { perror("fork"); fail("fork"); close(fd); return; }
    if (pid == 0) {
        int cfd = open(path, O_RDWR); // child's own open file description
        if (do_lock(cfd, F_OFD_SETLKW, F_WRLCK, 0, 0) < 0)
            _exit(3);
        int m = open(got, O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (m >= 0) close(m);
        _exit(0);
    }

    // Give the child time to reach the blocking F_OFD_SETLKW.
    for (int i = 0; i < 40; i++) usleep(10000);
    if (access(got, F_OK) == 0)
        fail("child acquired the lock before it was released");

    // Release; the blocked F_OFD_SETLKW waiter must now be woken.
    if (do_lock(fd, F_OFD_SETLK, F_UNLCK, 0, 0) < 0)
        fail("parent F_UNLCK");

    int st = 0;
    waitpid(pid, &st, 0);
    if (!(WIFEXITED(st) && WEXITSTATUS(st) == 0))
        fail("child F_OFD_SETLKW was not woken after release");
    else if (access(got, F_OK) != 0)
        fail("child never recorded acquiring the lock");
    unlink(got);
    close(fd);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    const char *path = "/tmp/fcntl_ofd.dat";

    if (test_supported(path)) {
        test_getlk_reports_minus_one(path);
        test_separate_opens_conflict(path);
        test_dup_shares(path);
        test_release_on_last_close(path);
        test_blocking_wake(path);
    }

    unlink(path);
    return finish_suite("fcntl_ofd");
}
