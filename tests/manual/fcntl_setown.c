// fcntl(F_SETOWN)/fcntl(F_GETOWN) were entirely unimplemented -- every call
// fell through sys_fcntl_common's default case to EINVAL. stress-ng's
// --sigurg stressor does fcntl(sockfd, F_SETOWN, getpid()) to register itself
// for SIGURG delivery and got errno=22 instead of success.
//
// Real Linux semantics (verified against a real kernel): F_SETOWN/F_GETOWN
// work on any fd type, not just sockets; who > 0 sets the pid owner and must
// name a live process (ESRCH otherwise); who < 0 sets the -who process group
// owner and must name a live process group (ESRCH otherwise); who == 0 always
// succeeds and clears the owner. Regression for fs/fd.c sys_fcntl_common.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include "test_common.h"

static void check(const char *label, int got, int want) {
    if (got != want) {
        printf("FAIL %s: got=%d want=%d\n", label, got, want);
        failures_total++;
    } else {
        test_logf("%s: %d ok\n", label, got);
    }
}

static void check_errno(const char *label, int ret, int want_ret, int want_errno) {
    int err = errno;
    if (ret != want_ret || (want_ret < 0 && err != want_errno)) {
        printf("FAIL %s: ret=%d errno=%d(%s) want ret=%d errno=%d\n",
               label, ret, err, strerror(err), want_ret, want_errno);
        failures_total++;
    } else {
        test_logf("%s: ret=%d errno=%d ok\n", label, ret, err);
    }
}

int main(int argc, char **argv) {
    test_init(argc, argv);

    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) {
        printf("FAIL: socket: %s\n", strerror(errno));
        return finish_suite("fcntl_setown");
    }

    pid_t pid = getpid();
    errno = 0;
    check_errno("F_SETOWN(getpid)", fcntl(s, F_SETOWN, pid), 0, 0);
    check("F_GETOWN after pid", fcntl(s, F_GETOWN), (int) pid);

    errno = 0;
    check_errno("F_SETOWN(0)", fcntl(s, F_SETOWN, 0), 0, 0);
    check("F_GETOWN after 0", fcntl(s, F_GETOWN), 0);

    pid_t pgrp = getpgrp();
    errno = 0;
    check_errno("F_SETOWN(-pgrp)", fcntl(s, F_SETOWN, -pgrp), 0, 0);
    check("F_GETOWN after -pgrp", fcntl(s, F_GETOWN), -(int) pgrp);

    errno = 0;
    check_errno("F_SETOWN(nonexistent pid)", fcntl(s, F_SETOWN, 999999), -1, ESRCH);

    errno = 0;
    check_errno("F_SETOWN(nonexistent pgrp)", fcntl(s, F_SETOWN, -999999), -1, ESRCH);

    // F_SETOWN/F_GETOWN are valid on any fd type on Linux, not just sockets.
    int fd = open("/tmp", O_RDONLY);
    if (fd < 0) {
        printf("FAIL: open /tmp: %s\n", strerror(errno));
        failures_total++;
    } else {
        errno = 0;
        check_errno("F_SETOWN on plain fd", fcntl(fd, F_SETOWN, pid), 0, 0);
        check("F_GETOWN on plain fd", fcntl(fd, F_GETOWN), (int) pid);
        close(fd);
    }

    close(s);
    return finish_suite("fcntl_setown");
}
