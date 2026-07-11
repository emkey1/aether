// memfd_create + mmap: anonymous memory-backed files.
//
// History: kernel/memfd.c backed memfds with a malloc'd buffer and its fd_ops
// had no .mmap op, so the canonical memfd_create + ftruncate + mmap sequence
// returned ENODEV from do_mmap — the same bug class as the tmpfs ENODEV fixed
// just before this (see tmpfs_mmap.c). Fixed by backing every memfd with an
// unlinked host temp file eagerly at creation (a memfd exists specifically to
// be mmapped) and routing read/write/resize through pread/pwrite/ftruncate on
// that fd, so guest mappings are host mmaps of the same file and the host
// kernel provides MAP_SHARED write-back and mmap<->read/write coherence.
//
// Verifies (all behaviors ground-truthed against real Linux): fd basics
// (F_GETFL O_RDWR, mode 0777, size 0, nlink 0, MFD_CLOEXEC sets FD_CLOEXEC,
// bad flags EINVAL); mmap of a zero-size memfd succeeds; MAP_SHARED coherence
// in both directions (map write -> fd read, fd write -> map read); mmap-writes
// past EOF don't extend the file; MAP_PRIVATE isolation; EINVAL for an
// unaligned offset; nonzero-offset mapping content; ftruncate grow under a
// live mapping; write/lseek/O_APPEND file I/O; read past EOF returns 0;
// MAP_SHARED propagation across fork. Arch-neutral.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include "test_common.h"

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif

#ifndef SYS_memfd_create
#if defined(__i386__)
#define SYS_memfd_create 356
#elif defined(__x86_64__)
#define SYS_memfd_create 319
#elif defined(__aarch64__)
#define SYS_memfd_create 279
#endif
#endif

static int memfd(const char *name, unsigned flags) {
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

// Creation-time properties of the fd itself.
static void test_fd_basics(void) {
    int fd = memfd("basics", MFD_CLOEXEC);
    check(fd >= 0, "memfd_create");
    if (fd < 0) return;

    int fl = fcntl(fd, F_GETFL);
    check((fl & O_ACCMODE) == O_RDWR, "F_GETFL reports O_RDWR");
    check(fcntl(fd, F_GETFD) & FD_CLOEXEC, "MFD_CLOEXEC sets FD_CLOEXEC");

    struct stat st;
    check(fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && (st.st_mode & 07777) == 0777,
          "fstat: regular file, mode 0777");
    check(st.st_size == 0, "initial size 0");
    check(st.st_nlink == 0, "nlink 0");
    close(fd);

    fd = memfd("noclo", 0);
    check(fd >= 0 && !(fcntl(fd, F_GETFD) & FD_CLOEXEC),
          "no MFD_CLOEXEC -> no FD_CLOEXEC");
    if (fd >= 0) close(fd);

    check(memfd("bad", 0x8000) == -1 && errno == EINVAL,
          "unknown flags -> EINVAL");
}

// The canonical usage: create, ftruncate, mmap MAP_SHARED, and coherence with
// fd I/O in both directions.
static void test_shared_coherence(void) {
    int fd = memfd("shared", 0);
    check(fd >= 0, "memfd_create for coherence test");
    if (fd < 0) return;

    // Linux (verified): mmap succeeds even at size 0 (access would fault).
    char *p0 = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    check(p0 != MAP_FAILED, "mmap MAP_SHARED RW of zero-size memfd succeeds");
    if (p0 != MAP_FAILED) munmap(p0, 4096);

    check(ftruncate(fd, 8192) == 0, "ftruncate to 2 pages");
    char *p = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    check(p != MAP_FAILED, "mmap MAP_SHARED RW after ftruncate");
    if (p == MAP_FAILED) { close(fd); return; }

    check(p[0] == 0 && p[8191] == 0, "ftruncate growth reads back zero");

    strcpy(p, "hello memfd");
    char buf[32] = {0};
    check(pread(fd, buf, sizeof(buf), 0) > 0 && strcmp(buf, "hello memfd") == 0,
          "map write visible through fd read");
    check(pwrite(fd, "H", 1, 0) == 1 && strcmp(p, "Hello memfd") == 0,
          "fd write visible through mapping");

    munmap(p, 8192);
    close(fd);
}

// Writes past EOF through a mapping must not extend the file.
static void test_map_write_no_extend(void) {
    int fd = memfd("noextend", 0);
    if (fd < 0) { check(0, "memfd_create for no-extend test"); return; }
    check(ftruncate(fd, 100) == 0, "ftruncate to 100 bytes");
    char *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    check(p != MAP_FAILED, "mmap 1 page of 100-byte memfd");
    if (p != MAP_FAILED) {
        p[200] = 'Z';
        struct stat st;
        check(fstat(fd, &st) == 0 && st.st_size == 100,
              "map write past EOF does not extend the file");
        munmap(p, 4096);
    }
    close(fd);
}

// MAP_PRIVATE: writes stay private to the mapping.
static void test_private_isolation(void) {
    int fd = memfd("private", 0);
    if (fd < 0) { check(0, "memfd_create for private test"); return; }
    check(write(fd, "hello", 5) == 5, "setup write");
    char *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    check(p != MAP_FAILED, "mmap MAP_PRIVATE RW on memfd");
    if (p != MAP_FAILED) {
        p[0] = 'Q';
        char c = 0;
        check(pread(fd, &c, 1, 0) == 1 && c == 'h',
              "MAP_PRIVATE write not visible through fd");
        munmap(p, 4096);
    }
    close(fd);
}

// Unaligned offset -> EINVAL; page-aligned nonzero offset maps the right page.
static void test_offset(void) {
    int fd = memfd("offset", 0);
    if (fd < 0) { check(0, "memfd_create for offset test"); return; }
    check(ftruncate(fd, 3 * 4096) == 0, "ftruncate to 3 pages");

    char *p = mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, 123);
    check(p == MAP_FAILED && errno == EINVAL, "unaligned offset -> EINVAL");
    if (p != MAP_FAILED) munmap(p, 4096);

    check(pwrite(fd, "PAGE2", 5, 4096) == 5, "write marker at page 1");
    p = mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, 4096);
    check(p != MAP_FAILED && memcmp(p, "PAGE2", 5) == 0,
          "offset=4096 mapping shows page 1 content");
    if (p != MAP_FAILED) munmap(p, 4096);
    close(fd);
}

// ftruncate grow while a mapping is live: the mapping keeps tracking the file.
static void test_truncate_interaction(void) {
    int fd = memfd("trunc", 0);
    if (fd < 0) { check(0, "memfd_create for trunc test"); return; }
    check(ftruncate(fd, 4096) == 0, "ftruncate to 1 page");

    char *p = mmap(NULL, 2 * 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    check(p != MAP_FAILED, "mmap 2 pages of 1-page memfd");
    if (p == MAP_FAILED) { close(fd); return; }

    p[10] = 'A';
    check(ftruncate(fd, 2 * 4096) == 0, "grow memfd under live mapping");
    check(p[4096] == 0, "grown page reads back zero through mapping");
    p[4096 + 10] = 'B';
    char c = 0;
    check(pread(fd, &c, 1, 4096 + 10) == 1 && c == 'B',
          "write to grown page visible through fd");

    munmap(p, 2 * 4096);
    close(fd);
}

// Plain file I/O: write advances offset and size, lseek END, O_APPEND via
// fcntl, read past EOF returns 0.
static void test_file_io(void) {
    int fd = memfd("io", 0);
    if (fd < 0) { check(0, "memfd_create for io test"); return; }

    check(write(fd, "0123456789", 10) == 10, "write 10 bytes");
    check(lseek(fd, 0, SEEK_END) == 10, "lseek END == 10");
    struct stat st;
    check(fstat(fd, &st) == 0 && st.st_size == 10, "size 10 after write");

    char buf[32] = {0};
    check(pread(fd, buf, sizeof(buf), 100) == 0, "pread past EOF returns 0");
    check(pread(fd, buf, sizeof(buf), 0) == 10 && memcmp(buf, "0123456789", 10) == 0,
          "read back contents");

    check(fcntl(fd, F_SETFL, O_APPEND) == 0, "set O_APPEND");
    check(lseek(fd, 0, SEEK_SET) == 0, "rewind");
    check(write(fd, "abc", 3) == 3, "O_APPEND write");
    check(fstat(fd, &st) == 0 && st.st_size == 13, "O_APPEND write appended at end");
    check(pread(fd, buf, sizeof(buf), 10) == 3 && memcmp(buf, "abc", 3) == 0,
          "appended bytes at offset 10");

    check(ftruncate(fd, 5) == 0 && fstat(fd, &st) == 0 && st.st_size == 5,
          "shrink to 5");
    check(pread(fd, buf, sizeof(buf), 0) == 5, "read clamped to shrunk size");
    close(fd);
}

// MAP_SHARED across fork: child's write must be visible in the parent. This is
// the pattern real software uses memfd for (shared memory without a filesystem).
static void test_shared_fork(void) {
    int fd = memfd("fork", 0);
    if (fd < 0) { check(0, "memfd_create for fork test"); return; }
    check(ftruncate(fd, 4096) == 0, "ftruncate for fork test");

    char *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    check(p != MAP_FAILED, "mmap MAP_SHARED before fork");
    if (p == MAP_FAILED) { close(fd); return; }

    pid_t pid = fork();
    if (pid == 0) {
        memcpy(p, "CHILD!", 6);
        _exit(0);
    }
    int status;
    check(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "forked child exited cleanly");
    check(memcmp(p, "CHILD!", 6) == 0, "child's MAP_SHARED write visible in parent");

    munmap(p, 4096);
    close(fd);
}

int main(int argc, char **argv) {
    test_init(argc, argv);

    test_fd_basics();
    test_shared_coherence();
    test_map_write_no_extend();
    test_private_isolation();
    test_offset();
    test_truncate_interaction();
    test_file_io();
    test_shared_fork();

    return finish_suite("memfd_mmap");
}
