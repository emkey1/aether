// pidfd functional behavior (issue #423 Tier 1b): CLONE_PIDFD, pidfd_open,
// and pidfd_send_signal (kernel/pidfd.c, kernel/fork.c). Complements
// pidfd_open.c, which only locks in the amd64 arg-count classification.
//
// Scope: pidfd here supports poll-for-exit and pidfd_send_signal. It does
// NOT support waitid(P_PIDFD, ...) reaping -- that would need waitid to
// recognize pidfd as an idtype, translating it back to a pid, which is a
// separate feature from "create/poll/signal a pidfd". Reaping still goes
// through plain waitpid(pid, ...) on the pid CLONE_PIDFD/pidfd_open return
// alongside the fd.
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include "test_common.h"

#ifndef CLONE_PIDFD
#define CLONE_PIDFD 0x00001000
#endif

static int raw_pidfd_send_signal(int pidfd, int sig, void *info, unsigned flags) {
    return syscall(SYS_pidfd_send_signal, pidfd, sig, info, flags);
}
static int raw_pidfd_open(pid_t pid, unsigned flags) {
    return syscall(SYS_pidfd_open, pid, flags);
}

static int child_sleep_forever(void *arg) {
    (void) arg;
    for (;;)
        pause();
    return 0;
}

// pidfd must not be readable (POLLIN) while the child is alive, must become
// readable once it exits, and the pid must still be reapable via plain
// waitpid (CLONE_PIDFD doesn't change reaping semantics).
static void test_clone_pidfd_poll(void) {
    char *stack = malloc(65536);
    int pidfd = -1;
    pid_t pid = clone(child_sleep_forever, stack + 65536, CLONE_PIDFD | SIGCHLD, NULL, &pidfd);
    if (pid == -1) {
        failf("clone CLONE_PIDFD succeeds", (uint64_t) errno, 0, 0, 0, 0, 0);
        free(stack);
        return;
    }
    if (pidfd < 0) {
        failf("clone CLONE_PIDFD yields a valid pidfd", (uint64_t) (unsigned) pidfd, 0, 0, 0, 0, 0);
        waitpid(pid, NULL, 0);
        free(stack);
        return;
    }

    struct pollfd pfd = {.fd = pidfd, .events = POLLIN};
    int rc = poll(&pfd, 1, 0);
    test_log_if(rc != 0 || (pfd.revents & POLLIN), "pidfd poll while alive: rc=%d revents=%d\n", rc, pfd.revents);
    if (rc != 0 || (pfd.revents & POLLIN))
        failf("pidfd not readable while child alive", (uint64_t) rc, (uint64_t) pfd.revents, 0, 0, 0, 0);

    kill(pid, SIGTERM);

    pfd.revents = 0;
    rc = poll(&pfd, 1, 2000);
    test_log_if(rc != 1 || !(pfd.revents & POLLIN), "pidfd poll after exit: rc=%d revents=%d\n", rc, pfd.revents);
    if (rc != 1 || !(pfd.revents & POLLIN))
        failf("pidfd readable after child exit", (uint64_t) rc, (uint64_t) pfd.revents, 0, 1, POLLIN, 0);

    int status;
    pid_t reaped = waitpid(pid, &status, 0);
    if (reaped != pid)
        failf("waitpid still reaps a CLONE_PIDFD child by pid", (uint64_t) reaped, 0, 0, (uint64_t) pid, 0, 0);

    close(pidfd);
    free(stack);
}

// pidfd_open(existing pid) + pidfd_send_signal must deliver a real signal.
static void test_pidfd_open_send_signal(void) {
    pid_t pid = fork();
    if (pid == 0) {
        for (;;)
            pause();
    }
    usleep(100000);

    int pidfd = raw_pidfd_open(pid, 0);
    if (pidfd < 0) {
        failf("pidfd_open on a live pid succeeds", (uint64_t) errno, 0, 0, 0, 0, 0);
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        return;
    }

    int err = raw_pidfd_send_signal(pidfd, SIGKILL, NULL, 0);
    test_log_if(err != 0, "pidfd_send_signal: rc=%d errno=%d\n", err, errno);
    if (err != 0) {
        failf("pidfd_send_signal succeeds", (uint64_t) errno, 0, 0, 0, 0, 0);
        close(pidfd);
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        return;
    }

    int status;
    waitpid(pid, &status, 0);
    close(pidfd);
    if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGKILL)
        failf("pidfd_send_signal(SIGKILL) actually kills the target",
              (uint64_t) status, 0, 0, 0, 0, 0);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    test_clone_pidfd_poll();
    test_pidfd_open_send_signal();
    return finish_suite("pidfd_clone");
}
