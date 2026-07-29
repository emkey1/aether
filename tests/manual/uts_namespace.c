// uts_namespace.c — regression for UTS namespace support (issue #527).
//
// systemd creates a UTS namespace for every transient unit, so `systemd-run`
// died with 226/NAMESPACE ("the kernel does not support UTS namespaces") back
// when unshare(CLONE_NEWUTS) returned ENOSYS. The property that matters is
// containment: a task that unshared its UTS namespace must not be able to
// change anyone else's hostname, while a plain fork child -- which shares the
// namespace -- must.
//
// Each case runs in a child so a failed containment check can't leak a bogus
// hostname back into the parent's namespace.
#define _GNU_SOURCE
#include <sched.h>
#include <errno.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include "test_common.h"

#ifndef CLONE_NEWUTS
#define CLONE_NEWUTS 0x04000000
#endif

static int hostname_is(const char *want) {
    char buf[256];
    if (gethostname(buf, sizeof(buf)) < 0) {
        printf("FAIL: gethostname: %s\n", strerror(errno));
        return 0;
    }
    buf[sizeof(buf) - 1] = '\0';
    return strcmp(buf, want) == 0;
}

// Returns the child's exit status, or -1 if it didn't exit normally.
static int run_child(int (*fn)(void)) {
    pid_t pid = fork();
    if (pid < 0) {
        printf("FAIL: fork: %s\n", strerror(errno));
        return -1;
    }
    if (pid == 0)
        _exit(fn());
    int status;
    if (waitpid(pid, &status, 0) != pid)
        return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static int child_unshare_is_contained(void) {
    if (unshare(CLONE_NEWUTS) < 0) {
        printf("FAIL: unshare(CLONE_NEWUTS): %s\n", strerror(errno));
        return 1;
    }
    // The new namespace starts as a copy, not empty.
    if (!hostname_is("uts-outer")) {
        printf("FAIL: new namespace did not inherit the hostname\n");
        return 2;
    }
    if (sethostname("uts-inner", strlen("uts-inner")) < 0) {
        printf("FAIL: sethostname in new namespace: %s\n", strerror(errno));
        return 3;
    }
    if (!hostname_is("uts-inner"))
        return 4;
    return 0;
}

// clone(CLONE_NEWUTS) has to work too, not just unshare -- systemd's executor
// takes that path. Uses a fork-like clone (no CLONE_VM) so the child can run
// on its own stack without us handing one over.
static int child_clone_is_contained(void) {
    long rc = syscall(SYS_clone, (long) (CLONE_NEWUTS | SIGCHLD), 0L, 0L, 0L, 0L);
    if (rc == 0) {
        if (!hostname_is("uts-outer"))
            _exit(1);
        if (sethostname("uts-cloned", strlen("uts-cloned")) < 0)
            _exit(2);
        _exit(hostname_is("uts-cloned") ? 0 : 3);
    }
    if (rc < 0) {
        printf("FAIL: clone(CLONE_NEWUTS): %s\n", strerror(errno));
        return 1;
    }
    int status;
    if (waitpid((pid_t) rc, &status, 0) != (pid_t) rc)
        return 2;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        printf("FAIL: cloned child failed (status %d)\n", status);
        return 3;
    }
    return 0;
}

// The negative control: without CLONE_NEWUTS the child shares our namespace,
// so its sethostname must be visible here. This is what proves the test above
// is measuring namespace containment and not just fork's address-space copy.
static int child_shared_ns_writes_through(void) {
    if (sethostname("uts-shared", strlen("uts-shared")) < 0) {
        printf("FAIL: sethostname in shared namespace: %s\n", strerror(errno));
        return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(10));

    if (geteuid() != 0) {
        printf("uts_namespace: SKIP (needs root)\n");
        return 0;
    }

    char saved[256];
    if (gethostname(saved, sizeof(saved)) < 0) {
        printf("uts_namespace: SKIP (gethostname: %s)\n", strerror(errno));
        return 0;
    }
    saved[sizeof(saved) - 1] = '\0';

    if (sethostname("uts-outer", strlen("uts-outer")) < 0) {
        printf("uts_namespace: SKIP (sethostname unsupported: %s)\n", strerror(errno));
        return 0;
    }

    int rc = run_child(child_unshare_is_contained);
    if (rc != 0)
        failf("unshare_contained", (uint64_t) rc, 0, 0, 0, 0, 0);
    else if (!hostname_is("uts-outer"))
        failf("unshare_leaked_to_parent", 1, 0, 0, 0, 0, 0);

    rc = run_child(child_clone_is_contained);
    if (rc != 0)
        failf("clone_contained", (uint64_t) rc, 0, 0, 0, 0, 0);
    else if (!hostname_is("uts-outer"))
        failf("clone_leaked_to_parent", 1, 0, 0, 0, 0, 0);

    rc = run_child(child_shared_ns_writes_through);
    if (rc != 0)
        failf("shared_ns_child", (uint64_t) rc, 0, 0, 0, 0, 0);
    else if (!hostname_is("uts-shared"))
        failf("shared_ns_did_not_write_through", 1, 0, 0, 0, 0, 0);

    sethostname(saved, strlen(saved));
    return finish_suite("uts_namespace");
}
