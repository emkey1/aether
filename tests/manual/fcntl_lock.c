// fcntl POSIX-lock (F_SETLK/F_SETLKW/F_GETLK) marshalling + release test.
//
// Regression for the amd64 fcntl path, which marshalled the guest struct flock
// with the i386 layout (l_start at offset 4) instead of the native amd64 layout
// (l_start at offset 8, after 4 bytes of padding). That shifted l_start/l_len by
// 32 bits, recording garbage lock ranges -- which on real workloads (dpkg)
// produced permanent F_SETLKW deadlocks. Also exercises that releasing a lock by
// process exit wakes an F_SETLKW waiter (file_lock_remove_owned_by must notify).
//
// Arch-neutral: must produce identical results on i386 and amd64.
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>

#define LSTART 40000LL
#define LLEN   10000LL   /* locked bytes: [40000 .. 49999] */

static int verbose;

static int probe(int fd, long long start, long long len,
                 int *out_pid, long long *out_start, long long *out_len) {
    struct flock fl;
    memset(&fl, 0, sizeof fl);
    fl.l_type = F_WRLCK;          /* "would a write lock here conflict?" */
    fl.l_whence = SEEK_SET;
    fl.l_start = start;
    fl.l_len = len;
    if (fcntl(fd, F_GETLK, &fl) < 0) { perror("F_GETLK"); exit(2); }
    if (out_pid)   *out_pid   = fl.l_pid;
    if (out_start) *out_start = (long long) fl.l_start;
    if (out_len)   *out_len   = (long long) fl.l_len;
    return fl.l_type;
}

// Part 1: child holds [40000,49999]; parent verifies range marshalling.
static int test_range_marshalling(void) {
    const char *path = "/tmp/fcntl_lock.dat";
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { perror("open"); return 2; }
    if (ftruncate(fd, 1 << 20) < 0) { perror("ftruncate"); return 2; }
    unlink("/tmp/fcntl_lock.ready");
    unlink("/tmp/fcntl_lock.done");

    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return 2; }
    if (pid == 0) {
        struct flock fl;
        memset(&fl, 0, sizeof fl);
        fl.l_type = F_WRLCK;
        fl.l_whence = SEEK_SET;
        fl.l_start = LSTART;
        fl.l_len = LLEN;
        if (fcntl(fd, F_SETLKW, &fl) < 0) { perror("child F_SETLKW"); _exit(3); }
        int m = open("/tmp/fcntl_lock.ready", O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (m >= 0) close(m);
        for (int i = 0; i < 500; i++) {
            if (access("/tmp/fcntl_lock.done", F_OK) == 0) break;
            usleep(10000);
        }
        _exit(0);
    }

    for (int i = 0; i < 1000; i++) {
        if (access("/tmp/fcntl_lock.ready", F_OK) == 0) break;
        usleep(5000);
    }

    int fails = 0, t, lpid = 0;
    long long s = 0, l = 0;

    t = probe(fd, 45000, 1, &lpid, &s, &l);      /* inside [40000,49999] */
    if (verbose)
        printf("probe@45000  -> type=%d holder_pid=%d range=[start=%lld len=%lld]\n", t, lpid, s, l);
    if (t != F_WRLCK) { printf("FAIL: expected a conflict inside the locked range\n"); fails++; }
    else if (s != LSTART || l != LLEN) {
        printf("FAIL: holder range wrong, got start=%lld len=%lld (want %lld/%lld)\n", s, l, LSTART, LLEN);
        fails++;
    }

    t = probe(fd, 100, 1, NULL, NULL, NULL);     /* before the range */
    if (verbose) printf("probe@100    -> type=%d\n", t);
    if (t != F_UNLCK) { printf("FAIL: byte 100 must be unlocked\n"); fails++; }

    t = probe(fd, 60000, 1, NULL, NULL, NULL);   /* after the range */
    if (verbose) printf("probe@60000  -> type=%d\n", t);
    if (t != F_UNLCK) { printf("FAIL: byte 60000 must be unlocked\n"); fails++; }

    int m = open("/tmp/fcntl_lock.done", O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (m >= 0) close(m);
    waitpid(pid, NULL, 0);
    close(fd);
    return fails;
}

// Part 2: a blocked F_SETLKW must be released when the holder *exits* (not just
// on explicit F_UNLCK). Child A holds the whole file then exits; child B blocks
// in F_SETLKW and must then acquire it. If the exit-release path fails to wake
// the waiter, B hangs and the test times out.
static int test_release_on_exit(void) {
    const char *path = "/tmp/fcntl_lock2.dat";
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { perror("open2"); return 2; }
    if (ftruncate(fd, 4096) < 0) { perror("ftruncate2"); return 2; }
    unlink("/tmp/fcntl_lock2.held");
    unlink("/tmp/fcntl_lock2.got");

    pid_t a = fork();
    if (a < 0) { perror("fork a"); return 2; }
    if (a == 0) {
        struct flock fl;
        memset(&fl, 0, sizeof fl);
        fl.l_type = F_WRLCK; fl.l_whence = SEEK_SET; fl.l_start = 0; fl.l_len = 0;
        if (fcntl(fd, F_SETLKW, &fl) < 0) { perror("A F_SETLKW"); _exit(3); }
        int m = open("/tmp/fcntl_lock2.held", O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (m >= 0) close(m);
        usleep(200000);   /* hold briefly, then EXIT (releases via fd teardown) */
        _exit(0);
    }
    for (int i = 0; i < 1000; i++) {
        if (access("/tmp/fcntl_lock2.held", F_OK) == 0) break;
        usleep(5000);
    }

    pid_t b = fork();
    if (b < 0) { perror("fork b"); return 2; }
    if (b == 0) {
        struct flock fl;
        memset(&fl, 0, sizeof fl);
        fl.l_type = F_WRLCK; fl.l_whence = SEEK_SET; fl.l_start = 0; fl.l_len = 0;
        /* Blocks until A exits and the lock is released + waiter woken. */
        if (fcntl(fd, F_SETLKW, &fl) < 0) { perror("B F_SETLKW"); _exit(3); }
        int m = open("/tmp/fcntl_lock2.got", O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (m >= 0) close(m);
        _exit(0);
    }

    int sa = 0, sb = 0;
    waitpid(a, &sa, 0);
    waitpid(b, &sb, 0);
    close(fd);
    int ok = (access("/tmp/fcntl_lock2.got", F_OK) == 0) &&
             WIFEXITED(sb) && WEXITSTATUS(sb) == 0;
    if (!ok) { printf("FAIL: F_SETLKW waiter not woken by holder exit\n"); return 1; }
    return 0;
}

int main(int argc, char **argv) {
    verbose = (argc > 1 && strcmp(argv[1], "-v") == 0);
    int fails = 0;
    fails += test_range_marshalling();
    fails += test_release_on_exit();
    printf("%s (%d failure%s)\n",
           fails ? "FCNTL LOCK TEST FAILED" : "FCNTL LOCK TEST PASSED",
           fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
