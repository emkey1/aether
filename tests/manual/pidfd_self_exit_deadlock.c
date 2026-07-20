// A task that opens a pidfd on ITSELF and never closes it before exiting
// must not deadlock. Two related bugs found booting Arch Linux ARM's
// aarch64 systemd (>= 260) as PID 1, both in the self-pidfd exit path:
//
// 1. pidfd_open() (and clone(CLONE_PIDFD)) never set close-on-exec, so a
//    self-pidfd opened just before exec() survived into the exec'd program.
//    Linux unconditionally sets FD_CLOEXEC on every pidfd regardless of the
//    flags argument -- only PIDFD_NONBLOCK is a valid input bit.
// 2. Independent of (1): do_exit's exit_wait_needed() gates (waiting for a
//    task's reference count to drop before releasing its own resources,
//    including its fd table) ran BEFORE the fd-table teardown that would
//    close a still-open self-pidfd -- so a task holding one could never
//    finish exiting: the busy-wait can never see the refcount drop, because
//    the fd table that would drop it is only released once the wait
//    already returned false. Real trigger: systemd's exec-invoke helper
//    opens a self-pidfd (used to track/report its own PID) and, on one code
//    path, exits without ever closing it -- wedging every waitid() up its
//    parent chain forever (0% CPU, no error, looked like a plain hang).
//
// Reproduces the exit-without-close pattern directly: fork a child that
// opens a pidfd on itself, deliberately leaks it (no close, no exec), and
// exits. The parent must reap it via ordinary wait4 within the watchdog.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include "test_common.h"

#ifndef SYS_pidfd_open
#define SYS_pidfd_open 434
#endif

static void check(int cond, const char *what) {
    if (cond) {
        test_logf("ok: %s\n", what);
    } else {
        printf("FAIL: %s (errno=%d %s)\n", what, errno, strerror(errno));
        failures_total++;
    }
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(15));

    pid_t child = fork();
    check(child >= 0, "fork");
    if (child == 0) {
        // Self-pidfd, deliberately never closed, no exec -- the exact
        // pattern that wedged do_exit.
        int self_pidfd = (int) syscall(SYS_pidfd_open, getpid(), 0);
        if (self_pidfd < 0)
            _exit(2);
        _exit(0);
    }

    int status = 0;
    pid_t r = waitpid(child, &status, 0);
    check(r == child, "waitpid reaps the self-pidfd-holding child");
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "child exited cleanly (status)");

    // Also verify the CLOEXEC half directly: a self-pidfd must not survive
    // exec (checked via fork+exec of a tiny helper that inspects its own fd
    // table would need an extra binary; instead assert the fd-flags bit
    // directly here, which is the exact property do_exit's fd-table
    // teardown depends on if this task DID exec).
    int self_pidfd = (int) syscall(SYS_pidfd_open, getpid(), 0);
    check(self_pidfd >= 0, "pidfd_open(getpid())");
    if (self_pidfd >= 0) {
        int flags = fcntl(self_pidfd, F_GETFD);
        check(flags >= 0 && (flags & FD_CLOEXEC), "self-pidfd has FD_CLOEXEC set");
        close(self_pidfd);
    }

    return finish_suite("pidfd_self_exit_deadlock");
}
