// /proc/<pid>/fdinfo/<fd> for a pidfd must include a "Pid:\t<pid>\n" line,
// matching real Linux's pidfd_show_fdinfo (fs/proc/fd.c). glibc's
// pidfd_get_pid() -- used by posix_spawn/pidfd_spawn (systemd's
// posix_spawn_wrapper, i.e. every clone3-based service start) -- reads
// exactly this line as its fallback when the newer PIDFD_GET_INFO ioctl
// isn't supported, and treats a missing line as "not a pidfd" (-ENOTTY).
// Before this fix, that ENOTTY propagated all the way up through
// pidfd_get_pid -> pidref_set_pidfd_take -> posix_spawn_wrapper as
// "Failed to spawn executor: Inappropriate ioctl for device", fatally
// blocking every clone3-spawned systemd service (tmp.mount,
// systemd-journald, dbus-broker, systemd-udevd, ...).
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include "test_common.h"

#ifndef SYS_pidfd_open
# ifdef __NR_pidfd_open
#  define SYS_pidfd_open __NR_pidfd_open
# else
#  define SYS_pidfd_open 434
# endif
#endif

static int raw_pidfd_open(pid_t pid, unsigned flags) {
    return syscall(SYS_pidfd_open, pid, flags);
}

// Mimics glibc's pidfd_get_pid_fdinfo(): parse "Pid:\t<n>\n" out of
// /proc/self/fdinfo/<fd>. Returns the parsed pid, or -1 if the field is
// missing (the exact condition glibc maps to -ENOTTY).
static int fdinfo_pid_field(int fd) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/self/fdinfo/%d", fd);
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        perror("fopen fdinfo");
        return -1;
    }
    char line[256];
    int pid = -1;
    while (fgets(line, sizeof(line), f) != NULL) {
        int val;
        if (sscanf(line, "Pid:\t%d", &val) == 1) {
            pid = val;
            break;
        }
    }
    fclose(f);
    return pid;
}

static int child_sleep_forever(void *arg) {
    (void) arg;
    for (;;)
        pause();
    return 0;
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(10));

    char *stack = malloc(65536);
    if (stack == NULL) {
        perror("malloc");
        return 1;
    }
    pid_t child = clone(child_sleep_forever, stack + 65536, SIGCHLD, NULL);
    if (child < 0) {
        perror("clone");
        return 1;
    }

    int pidfd = raw_pidfd_open(child, 0);
    if (pidfd < 0) {
        perror("pidfd_open");
        kill(child, SIGKILL);
        waitpid(child, NULL, 0);
        return 1;
    }

    int fdinfo_pid = fdinfo_pid_field(pidfd);
    test_logf("fdinfo Pid: field -> %d (want %d)\n", fdinfo_pid, (int) child);
    if (fdinfo_pid != (int) child) {
        printf("FAIL: /proc/self/fdinfo/%d missing or wrong \"Pid:\" line "
               "(got %d, want %d) -- this is exactly what makes glibc's "
               "pidfd_get_pid() fallback return ENOTTY\n",
               pidfd, fdinfo_pid, (int) child);
        failures_total++;
    }

    // A plain (non-pidfd) fd's fdinfo must NOT gain a spurious "Pid:" line.
    int plain_fd = open("/dev/null", O_RDONLY);
    if (plain_fd < 0) {
        perror("open /dev/null");
        failures_total++;
    } else {
        int spurious = fdinfo_pid_field(plain_fd);
        test_log_if(spurious == -1, "plain fd fdinfo has no Pid: field ok\n");
        if (spurious != -1) {
            printf("FAIL: plain fd %d's fdinfo has a spurious \"Pid:\" line (%d)\n",
                   plain_fd, spurious);
            failures_total++;
        }
        close(plain_fd);
    }

    close(pidfd);
    kill(child, SIGKILL);
    waitpid(child, NULL, 0);

    return finish_suite("pidfd_fdinfo_pid");
}
