// ptrace_group_stop: a SEIZE'd tracee that enters a job-control group-stop must
// report it to the tracer (as PTRACE_EVENT_STOP) and be resumable to completion.
// This is the regression for iSH-AOK's ptrace_group_stop() path -- before it,
// strace -f following a fork/clone child hung because a traced task's group-stop
// was invisible to the tracer's wait4 and PTRACE_CONT had nothing to resume.
//
// The flow also matches real Linux semantics, so this doubles as a conformance
// test (note: it requires a working ptrace, so it cannot run under qemu-user).
//
//   child : raise(SIGSTOP) -> then _exit(42)
//   tracer: SEIZE child; on the SIGSTOP signal-delivery-stop, deliver SIGSTOP to
//           force group-stop; expect a PTRACE_EVENT_STOP report; SIGCONT +
//           PTRACE_CONT to let it finish; verify exit code 42.

#define _GNU_SOURCE

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "test_common.h"

#ifndef PTRACE_SEIZE
#define PTRACE_SEIZE 0x4206
#endif
#ifndef PTRACE_EVENT_STOP
#define PTRACE_EVENT_STOP 128
#endif

static pid_t g_child;

static void on_alarm(int sig) {
    (void) sig;
    if (g_child > 0)
        kill(g_child, SIGKILL);
    static const char msg[] = "ptrace_group_stop: FAIL timeout (no group-stop report / never resumed)\n";
    write(2, msg, sizeof(msg) - 1);
    _exit(1);
}

static void test_group_stop_reported(void) {
    g_child = fork();
    if (g_child < 0) {
        perror("fork");
        failf("ptrace_group_stop fork", 0, 0, 0, 0, 0, 0);
        return;
    }
    if (g_child == 0) {
        // Give the parent a beat to PTRACE_SEIZE us before we stop.
        raise(SIGSTOP);
        _exit(42);
    }

    pid_t child = g_child;
    if (ptrace(PTRACE_SEIZE, child, 0, 0) != 0) {
        perror("PTRACE_SEIZE");
        kill(child, SIGKILL);
        failf("ptrace_group_stop seize", (uint64_t) errno, 0, 0, 0, 0, 0);
        return;
    }
    test_logf("seized %d\n", (int) child);

    int saw_group_stop = 0;
    for (int iterations = 0; iterations < 100; iterations++) {
        int status;
        pid_t w = waitpid(child, &status, 0);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            perror("waitpid");
            failf("ptrace_group_stop waitpid", (uint64_t) errno, 0, 0, 0, 0, 0);
            return;
        }

        if (WIFEXITED(status)) {
            int code = WEXITSTATUS(status);
            test_logf("child exited %d (saw_group_stop=%d)\n", code, saw_group_stop);
            if (!saw_group_stop)
                failf("ptrace_group_stop event", 0, 0, 0, 1, 0, 0);
            if (code != 42)
                failf("ptrace_group_stop exit", (uint64_t) code, 0, 0, 42, 0, 0);
            return;
        }
        if (WIFSIGNALED(status)) {
            failf("ptrace_group_stop killed", (uint64_t) WTERMSIG(status), 0, 0, 0, 0, 0);
            return;
        }
        if (!WIFSTOPPED(status)) {
            failf("ptrace_group_stop status", (uint64_t) status, 0, 0, 0, 0, 0);
            return;
        }

        int sig = WSTOPSIG(status);
        int event = (status >> 16) & 0xff;
        test_logf("stop: sig=%d event=%d status=%#x\n", sig, event, status);

        if (event == PTRACE_EVENT_STOP) {
            // The group-stop report. Lift job control (SIGCONT for real-Linux
            // listener semantics; harmless on iSH) and continue.
            saw_group_stop = 1;
            kill(child, SIGCONT);
            if (ptrace(PTRACE_CONT, child, 0, 0) != 0) {
                perror("PTRACE_CONT after event-stop");
                failf("ptrace_group_stop cont", (uint64_t) errno, 0, 0, 0, 0, 0);
                return;
            }
        } else if (sig == SIGSTOP) {
            // Signal-delivery-stop of SIGSTOP: deliver it so the child actually
            // group-stops (and thus reports PTRACE_EVENT_STOP next).
            if (ptrace(PTRACE_CONT, child, 0, SIGSTOP) != 0) {
                perror("PTRACE_CONT deliver SIGSTOP");
                failf("ptrace_group_stop deliver", (uint64_t) errno, 0, 0, 0, 0, 0);
                return;
            }
        } else {
            // Any other stop (SIGCONT delivery etc.): pass over it.
            if (ptrace(PTRACE_CONT, child, 0, 0) != 0) {
                perror("PTRACE_CONT pass");
                failf("ptrace_group_stop pass", (uint64_t) errno, 0, 0, 0, 0, 0);
                return;
            }
        }
    }

    kill(child, SIGKILL);
    failf("ptrace_group_stop loop", 0, 0, 0, 0, 0, 0);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    signal(SIGALRM, on_alarm);
    alarm(15);
    test_group_stop_reported();
    return finish_suite("ptrace_group_stop");
}
