// Syscalls that were implemented but not reachable, because the per-ABI table
// entry was missing. The implementation existing is not the same as the guest
// being able to call it, and a libc that reaches the same functionality by
// another route hides the gap completely -- which is why these go through the
// raw syscall rather than the libc wrapper.
//
//  * truncate: i386 [92] was absent while truncate64 [193] was present, so a
//    statically-linked glibc calling it got the "missing syscall" ENOSYS. That
//    one was easy to spot. The bigger bug underneath was that
//    sys_truncate64_guest passed a NULL dirfd to generic_setattrat, which since
//    the AT_PWD work means "bad/closed dirfd" and returns EBADF -- so every
//    truncate(2) on EVERY ABI was failing, not just the unwired i386 entry.
//
//  * adjtimex: implemented since the clock_adjtime work but only wired into the
//    asm-generic table (arm64/riscv64 [171]); i386 [124] and amd64 [159] were
//    both missing. Reported upstream as ish-app/ish#1322. A modern glibc
//    implements adjtimex(3) on top of clock_adjtime64, which we already had, so
//    the libc wrapper worked fine and only the raw syscall failed.
//
// Arch-neutral: SYS_* resolve to the right per-ABI numbers.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/timex.h>
#include <unistd.h>
#include "test_common.h"

static void check(const char *what, long got, long want) {
    if (got == want) {
        test_logf("  ok   %s = %ld\n", what, got);
        return;
    }
    printf("FAIL: %s = %ld (want %ld)\n", what, got, want);
    failures_total++;
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(60));

    const char *path = "/tmp/syscall_wiring_test";
    unlink(path);
    int fd = open(path, O_CREAT | O_RDWR, 0644);
    if (fd < 0) {
        printf("syscall_wiring: FAIL setup: %s\n", strerror(errno));
        return 1;
    }
    if (write(fd, "0123456789ABCDEF", 16) != 16) {
        printf("syscall_wiring: FAIL setup write\n");
        close(fd);
        return 1;
    }
    close(fd);

    struct stat st;

    // --- truncate via the raw syscall -------------------------------------
    errno = 0;
    long r = syscall(SYS_truncate, path, (long) 8);
    if (r < 0) {
        printf("FAIL: raw SYS_truncate -> %s\n", strerror(errno));
        failures_total++;
    } else {
        stat(path, &st);
        check("raw SYS_truncate shrinks to 8", (long) st.st_size, 8);
    }

    // ...and via the libc wrapper, which may take a different route.
    errno = 0;
    if (truncate(path, 4) < 0) {
        printf("FAIL: truncate(3) -> %s\n", strerror(errno));
        failures_total++;
    } else {
        stat(path, &st);
        check("truncate(3) shrinks to 4", (long) st.st_size, 4);
    }

    // Growing, and the error case.
    errno = 0;
    if (truncate(path, 32) < 0) {
        printf("FAIL: truncate grow -> %s\n", strerror(errno));
        failures_total++;
    } else {
        stat(path, &st);
        check("truncate grows to 32", (long) st.st_size, 32);
    }
    errno = 0;
    r = truncate("/tmp/syscall_wiring_absent", 0);
    check("truncate of a missing file is ENOENT", r < 0 && errno == ENOENT, 1);

    unlink(path);

    // --- adjtimex via the raw syscall -------------------------------------
#ifdef SYS_adjtimex
    {
        struct timex tx;
        memset(&tx, 0, sizeof tx);
        tx.modes = 0;                    // pure query
        errno = 0;
        r = syscall(SYS_adjtimex, &tx);
        if (r < 0) {
            printf("FAIL: raw SYS_adjtimex -> %s\n", strerror(errno));
            failures_total++;
        } else {
            test_logf("  ok   raw SYS_adjtimex -> %ld (tick=%ld)\n", r, tx.tick);
            // A query returns a clock state, not an error, and populates the
            // struct. Real Linux reports TIME_ERROR (5) when unsynchronised.
            check("adjtimex query returns a clock state", r >= 0, 1);
            check("adjtimex query fills in tick", tx.tick != 0, 1);
        }

        // Any modes != 0 needs privilege we deliberately do not grant.
        memset(&tx, 0, sizeof tx);
        tx.modes = ADJ_FREQUENCY;
        errno = 0;
        r = syscall(SYS_adjtimex, &tx);
        check("adjtimex with modes set is EPERM", r < 0 && errno == EPERM, 1);
    }
#else
    test_logf("  (SYS_adjtimex undefined for this ABI, skipping)\n");
#endif

    if (failures_total != 0) {
        printf("syscall_wiring: FAIL failures=%u\n", failures_total);
        return 1;
    }
    printf("syscall_wiring: PASS\n");
    return 0;
}
