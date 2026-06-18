// fs_conformance.c — self-checking regression lock for the filesystem/VFS
// conformance fixes found by differential testing against real Linux (mint).
// Each check asserts the documented Linux behavior that iSH now matches:
//
//   - O_NOFOLLOW on a final symlink -> ELOOP
//   - rmdir of a symlink-to-dir -> ENOTDIR (final symlink not followed)
//   - mkdir over an existing/dangling symlink -> EEXIST
//   - unlink of a directory -> EISDIR (not the host's EPERM)
//   - "nonexistent/.." -> ENOENT (no lexical '..' collapse)
//   - open("name/", O_CREAT) -> EISDIR (no create through a trailing slash)
//   - getdents with a too-small buffer -> EINVAL (not a spurious EOF)
//   - renameat2(RENAME_NOREPLACE) onto an existing name -> EEXIST, else success
//   - fcntl(F_DUPFD) with a negative/over-limit arg -> EINVAL (no abort)
//   - readlink("/proc/self") is the bare numeric pid (no trailing slash)
//   - dup clears FD_CLOEXEC; dup3(O_CLOEXEC) sets it
//
// The same source is a Tier-0 functional gate (run under iSH, no oracle) and a
// portable check that also passes on a real Linux kernel.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/resource.h>
#include "test_common.h"

#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0400000
#endif
#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE (1 << 0)
#endif

static int eq_errno(const char *label, long r, int want) {
    int e = (r < 0) ? errno : 0;
    if (r < 0 && e == want) { test_logf("ok   %s (errno %d)\n", label, e); return 1; }
    printf("FAIL %s: ret=%ld errno=%d, wanted -1/errno=%d\n", label, r, e, want);
    failures_total++;
    return 0;
}
static int is_ok(const char *label, long r) {
    if (r >= 0) { test_logf("ok   %s\n", label); return 1; }
    printf("FAIL %s: ret=%ld errno=%d, wanted success\n", label, r, errno);
    failures_total++;
    return 0;
}
static int is_true(const char *label, int cond) {
    if (cond) { test_logf("ok   %s\n", label); return 1; }
    printf("FAIL %s\n", label);
    failures_total++;
    return 0;
}

// open + immediately close; return the open() result (errno preserved on failure)
static long open_close(const char *path, int flags, mode_t mode) {
    int fd = open(path, flags, mode);
    if (fd >= 0) close(fd);
    return fd;
}
static void mkfile(const char *path) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) close(fd);
}

int main(int argc, char **argv) {
    test_init(argc, argv);

    char base[200];
    snprintf(base, sizeof base, "/tmp/fsconf_XXXXXX");
    if (!mkdtemp(base) || chdir(base) != 0) { perror("setup"); return 2; }

    // ---- O_NOFOLLOW ----
    mkfile("target");
    symlink("target", "sl");
    mkdir("d", 0755);
    eq_errno("nofollow.symlink", open_close("sl", O_RDONLY | O_NOFOLLOW, 0), ELOOP);
    is_ok("nofollow.regfile", open_close("target", O_RDONLY | O_NOFOLLOW, 0));
    is_ok("nofollow.dir", open_close("d", O_RDONLY | O_DIRECTORY | O_NOFOLLOW, 0));

    // ---- '..'/'.' through a nonexistent component ----
    struct stat st;
    eq_errno("dotdot.nonexist.stat", stat("nope/..", &st), ENOENT);
    eq_errno("dotdot.nonexist.access", access("nope/..", F_OK), ENOENT);
    eq_errno("dotdot.nonexist.open", open_close("nope/../target", O_RDONLY, 0), ENOENT);

    // ---- trailing slash ----
    eq_errno("slash.creat", open_close("newf/", O_WRONLY | O_CREAT, 0644), EISDIR);
    eq_errno("slash.onfile", open_close("target/", O_RDONLY, 0), ENOTDIR);
    is_ok("slash.ondir", open_close("d/", O_RDONLY, 0));

    // ---- rmdir / mkdir / unlink on symlinks and dirs ----
    mkdir("rtgt", 0755);
    symlink("rtgt", "rsl");
    eq_errno("rmdir.symlink", rmdir("rsl"), ENOTDIR);
    is_true("rmdir.symtgt_kept", stat("rtgt", &st) == 0 && S_ISDIR(st.st_mode));

    symlink("d", "msl");
    eq_errno("mkdir.over_symlink", mkdir("msl", 0755), EEXIST);
    symlink("nowhere", "mdsl");
    eq_errno("mkdir.over_dangling", mkdir("mdsl", 0755), EEXIST);

    mkdir("ud", 0755);
    eq_errno("unlink.dir", unlink("ud"), EISDIR);

    // ---- getdents with a too-small buffer ----
    int dfd = open("d", O_RDONLY | O_DIRECTORY);
    if (dfd >= 0) {
        char tiny[1];
        eq_errno("getdents.tiny", syscall(SYS_getdents64, dfd, tiny, (unsigned) sizeof tiny), EINVAL);
        close(dfd);
    }

    // ---- renameat2(RENAME_NOREPLACE) ----
    mkfile("nr_a"); mkfile("nr_b");
    eq_errno("rename.noreplace.exist",
             syscall(SYS_renameat2, AT_FDCWD, "nr_a", AT_FDCWD, "nr_b", RENAME_NOREPLACE), EEXIST);
    is_ok("rename.noreplace.new",
          syscall(SYS_renameat2, AT_FDCWD, "nr_a", AT_FDCWD, "nr_new", RENAME_NOREPLACE));
    is_true("rename.noreplace.moved", stat("nr_new", &st) == 0);

    // ---- F_DUPFD bounds (must not abort) ----
    int fd = open("target", O_RDONLY);
    if (fd >= 0) {
        eq_errno("dupfd.neg", fcntl(fd, F_DUPFD, -1), EINVAL);
        struct rlimit rl; getrlimit(RLIMIT_NOFILE, &rl);
        long lim = (rl.rlim_cur < (rlim_t) 0x40000000) ? (long) rl.rlim_cur : 0x40000000;
        eq_errno("dupfd.at_limit", fcntl(fd, F_DUPFD, lim), EINVAL);
        // a valid F_DUPFD still works and clears cloexec
        int nf = fcntl(fd, F_DUPFD, 50);
        is_true("dupfd.valid", nf >= 50);
        if (nf >= 0) { is_true("dupfd.cloexec_clear", fcntl(nf, F_GETFD) == 0); close(nf); }
        close(fd);
    }

    // ---- dup cloexec semantics ----
    int ce = open("target", O_RDONLY | O_CLOEXEC);
    if (ce >= 0) {
        is_true("open.cloexec_set", fcntl(ce, F_GETFD) == FD_CLOEXEC);
        int d2 = dup(ce);
        if (d2 >= 0) { is_true("dup.cloexec_clear", fcntl(d2, F_GETFD) == 0); close(d2); }
        int d3 = dup3(ce, 60, O_CLOEXEC);
        if (d3 >= 0) { is_true("dup3.cloexec_set", fcntl(60, F_GETFD) == FD_CLOEXEC); close(60); }
        close(ce);
    }

    // ---- /proc/self is the bare numeric pid ----
    char link[64];
    ssize_t r = readlink("/proc/self", link, sizeof link - 1);
    if (r > 0) {
        link[r] = '\0';
        int numeric = 1;
        for (ssize_t i = 0; i < r; i++) if (link[i] < '0' || link[i] > '9') numeric = 0;
        is_true("proc.self.numeric", numeric);
    }

    return finish_suite("fs_conformance");
}
