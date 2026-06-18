/*
 * fs_dupfd_range.c — F_DUPFD / F_DUPFD_CLOEXEC argument bounds.
 *
 * ISOLATED in its own test: a negative arg tripped iSH's assert(start >= 0) in
 * f_install_start (abort, or -EPERM/-EMFILE with asserts off) instead of Linux's
 * EINVAL. Keeping it separate means a pre-fix abort only crashes this one cell's
 * run, leaving the other fs tests comparable.
 *
 * Linux: fcntl(fd, F_DUPFD, arg) returns EINVAL when arg < 0 or
 * arg >= RLIMIT_NOFILE.
 */
#include "fs_common.h"
#include <sys/resource.h>

int main(int argc, char **argv) {
    fs_init(argc, argv, "dupfd_range");
    setvbuf(stdout, NULL, _IONBF, 0);   /* survive a pre-fix abort */

    int fd = open("f", O_RDWR | O_CREAT, 0644);
    if (fd < 0) { perror("open"); return 2; }

    struct rlimit rl;
    getrlimit(RLIMIT_NOFILE, &rl);
    long at_limit = (rl.rlim_cur < (rlim_t) 0x40000000) ? (long) rl.rlim_cur : 0x40000000;

    /* arg == RLIMIT_NOFILE (first invalid) -> EINVAL */
    emit("dupfd.at_limit", "%s", res_errno(fcntl(fd, F_DUPFD, at_limit)));
    /* arg way above the limit -> EINVAL */
    emit("dupfd.above", "%s", res_errno(fcntl(fd, F_DUPFD, at_limit + 1000)));
    /* F_DUPFD_CLOEXEC bounds are the same */
    emit("dupfd_cloexec.at_limit", "%s", res_errno(fcntl(fd, F_DUPFD_CLOEXEC, at_limit)));
    /* negative arg -> EINVAL (must NOT abort) */
    emit("dupfd.neg", "%s", res_errno(fcntl(fd, F_DUPFD, -1)));

    return 0;
}
