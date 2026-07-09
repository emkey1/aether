// st_dev semantics for virtual filesystem mounts (tmpfs, proc, binds).
//
// History: iSH's tmpfs/proc/devpts/sysfs inodes never set stat.dev, so every
// virtual mount reported st_dev=0. busybox df matches a path to a mount by
// comparing st_dev of the path against st_dev of each mountpoint in
// /proc/mounts; with everything at dev 0 the LAST 0-dev mount won, so
// `df /tmp` printed the devpts line (0 blocks) even though statfs("/tmp")
// itself was correct. Same-filesystem checks (find -xdev, du -x) also could
// not tell virtual mounts apart. Fixed by allocating a unique anonymous
// device (Linux 0:xx semantics) per mount at mount time and stamping it into
// stat results whenever the filesystem leaves stat.dev at 0; bind mounts
// share their origin's device, matching Linux's shared-superblock rule.
// Ground-truthed against a real kernel: each tmpfs/proc/devpts mount gets a
// distinct anon dev with major 0, and a bind of a tmpfs reports the origin's.
//
// Needs root to mount private tmpfs instances; mount-dependent checks are
// skipped when mounting fails. Also passes on real Linux.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include "test_common.h"

#ifndef SYS_memfd_create
#if defined(__i386__)
#define SYS_memfd_create 356
#elif defined(__x86_64__)
#define SYS_memfd_create 319
#else
#define SYS_memfd_create 279
#endif
#endif

static int my_memfd_create(const char *name, unsigned flags) {
    return syscall(SYS_memfd_create, name, flags);
}

static void check(int cond, const char *what) {
    if (cond) {
        test_logf("ok: %s\n", what);
    } else {
        printf("FAIL: %s (errno=%d %s)\n", what, errno, strerror(errno));
        failures_total++;
    }
}

// Two private tmpfs mounts: nonzero anon devs (major 0), distinct from each
// other, from /, and from /proc; files inherit the mount's dev; fstat agrees
// with stat; a bind mount shares the origin's dev.
static void test_tmpfs_devs(void) {
    char dir_a[128], dir_b[128], dir_bind[128];
    snprintf(dir_a, sizeof(dir_a), "/tmp/stdev-a.%d", (int) getpid());
    snprintf(dir_b, sizeof(dir_b), "/tmp/stdev-b.%d", (int) getpid());
    snprintf(dir_bind, sizeof(dir_bind), "/tmp/stdev-bind.%d", (int) getpid());

    if (mkdir(dir_a, 0700) != 0 || mount("tmpfs", dir_a, "tmpfs", 0, NULL) != 0) {
        test_logf("skip: cannot mount private tmpfs (errno=%d)\n", errno);
        rmdir(dir_a);
        return;
    }
    struct stat root_st, proc_st, a_st, b_st;
    check(stat("/", &root_st) == 0, "stat /");
    check(stat("/proc", &proc_st) == 0, "stat /proc");
    check(stat(dir_a, &a_st) == 0, "stat tmpfs A");
    check(a_st.st_dev != 0, "tmpfs A has nonzero st_dev");
    check(major(a_st.st_dev) == 0, "tmpfs A st_dev is anonymous (major 0)");
    check(a_st.st_dev != root_st.st_dev, "tmpfs A st_dev != root's");

    check(proc_st.st_dev != 0, "/proc has nonzero st_dev");
    check(proc_st.st_dev != a_st.st_dev, "/proc st_dev != tmpfs A's");

    if (mkdir(dir_b, 0700) == 0 && mount("tmpfs", dir_b, "tmpfs", 0, NULL) == 0) {
        check(stat(dir_b, &b_st) == 0, "stat tmpfs B");
        check(b_st.st_dev != 0, "tmpfs B has nonzero st_dev");
        check(b_st.st_dev != a_st.st_dev, "two tmpfs mounts get distinct st_dev");
        umount(dir_b);
    } else {
        test_logf("skip: second tmpfs mount failed (errno=%d)\n", errno);
    }
    rmdir(dir_b);

    // a file lives on its mount's device, and fstat matches stat
    char file_a[160];
    snprintf(file_a, sizeof(file_a), "%s/f", dir_a);
    int fd = open(file_a, O_CREAT | O_RDWR, 0600);
    check(fd >= 0, "create file on tmpfs A");
    if (fd >= 0) {
        struct stat f_st, ffd_st;
        check(stat(file_a, &f_st) == 0 && f_st.st_dev == a_st.st_dev,
              "file inherits the mount's st_dev");
        check(fstat(fd, &ffd_st) == 0 && ffd_st.st_dev == a_st.st_dev,
              "fstat st_dev matches the mount");
        close(fd);
    }

    // bind mount shares the origin's device (same superblock on Linux)
    if (mkdir(dir_bind, 0700) == 0 && mount(dir_a, dir_bind, NULL, MS_BIND, NULL) == 0) {
        struct stat bind_st, bind_f_st;
        char file_bind[192];
        snprintf(file_bind, sizeof(file_bind), "%s/f", dir_bind);
        check(stat(dir_bind, &bind_st) == 0 && bind_st.st_dev == a_st.st_dev,
              "bind mount shares the origin's st_dev");
        check(stat(file_bind, &bind_f_st) == 0 && bind_f_st.st_dev == a_st.st_dev,
              "file via bind path has the origin's st_dev");
        umount(dir_bind);
    } else {
        test_logf("skip: bind mount failed (errno=%d)\n", errno);
    }
    rmdir(dir_bind);

    unlink(file_a);
    umount(dir_a);
    rmdir(dir_a);
}

// fd types with no path also live on anonymous devices on Linux (pipefs,
// the internal shm tmpfs for memfds) — st_dev must not be 0.
static void test_anon_fd_devs(void) {
    int pipefd[2];
    if (pipe(pipefd) == 0) {
        struct stat st;
        check(fstat(pipefd[0], &st) == 0 && st.st_dev != 0,
              "pipe has nonzero st_dev");
        close(pipefd[0]);
        close(pipefd[1]);
    }

    int mfd = my_memfd_create("stdev-test", 0);
    if (mfd >= 0) {
        struct stat st;
        check(fstat(mfd, &st) == 0 && st.st_dev != 0,
              "memfd has nonzero st_dev");
        close(mfd);
    } else {
        test_logf("skip: memfd_create unavailable (errno=%d)\n", errno);
    }
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    test_tmpfs_devs();
    test_anon_fd_devs();
    return finish_suite("mount_stdev");
}
