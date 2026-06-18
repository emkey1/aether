/*
 * fs_dup.c — dup / dup2 / dup3 / F_DUPFD(_CLOEXEC) and FD_CLOEXEC semantics.
 *
 * Invariants asserted (all POSIX/Linux-guaranteed):
 *   - duped fds share one open file description (one offset).
 *   - dup/dup2/F_DUPFD clear FD_CLOEXEC on the new fd; dup3(O_CLOEXEC) /
 *     F_DUPFD_CLOEXEC set it.
 *   - dup2(fd,fd) returns fd (no close) iff fd valid, else EBADF.
 *   - dup3(fd,fd) is always EINVAL; bad flags EINVAL.
 */
#include "fs_common.h"

static int getfd(int fd) { return fcntl(fd, F_GETFD); }

int main(int argc, char **argv) {
    fs_init(argc, argv, "dup");

    int fd = open("f", O_RDWR | O_CREAT, 0644);
    if (fd < 0) { perror("open"); return 2; }
    ssize_t w = write(fd, "0123456789", 10); (void) w;

    /* ---- dup shares the offset ---- */
    int d = dup(fd);
    emit_i("dup.ok", d >= 0);
    lseek(fd, 3, SEEK_SET);
    emit_i("dup.shared_off", (long) lseek(d, 0, SEEK_CUR));      /* 3 */
    emit_i("dup.cloexec", getfd(d));                             /* 0 */

    emit("dup.badfd", "%s", res_errno(dup(999)));               /* EBADF */

    /* ---- dup2 ---- */
    int t = dup2(fd, 20);
    emit_i("dup2.target", t == 20);
    emit_i("dup2.shared_off", (long) lseek(20, 0, SEEK_CUR));    /* 3 */
    emit_i("dup2.cloexec", getfd(20));                           /* 0 */
    /* dup2(fd,fd): returns fd, no close, when fd valid */
    emit_i("dup2.same_valid", dup2(fd, fd) == fd);
    /* dup2(fd,fd) with invalid fd -> EBADF */
    emit("dup2.same_invalid", "%s", res_errno(dup2(999, 999)));  /* EBADF */
    emit("dup2.badsrc", "%s", res_errno(dup2(999, 21)));         /* EBADF */

    /* ---- dup3 ---- */
    emit("dup3.same",     "%s", res_errno(dup3(fd, fd, 0)));         /* EINVAL */
    emit("dup3.badflags", "%s", res_errno(dup3(fd, 22, 0x4)));      /* EINVAL */
    int d3 = dup3(fd, 23, O_CLOEXEC);
    emit_i("dup3.cloexec", d3 == 23 ? getfd(23) : -1);              /* 1 */

    /* ---- F_DUPFD picks lowest >= arg; clears cloexec ---- */
    int fdd = fcntl(fd, F_DUPFD, 30);
    emit_i("dupfd.atleast", fdd >= 30);
    emit_i("dupfd.cloexec", fdd >= 0 ? getfd(fdd) : -1);            /* 0 */
    emit_i("dupfd.shared_off", fdd >= 0 ? (long) lseek(fdd, 0, SEEK_CUR) : -1); /* 3 */

    int fdc = fcntl(fd, F_DUPFD_CLOEXEC, 40);
    emit_i("dupfd_cloexec.atleast", fdc >= 40);
    emit_i("dupfd_cloexec.cloexec", fdc >= 0 ? getfd(fdc) : -1);    /* 1 */

    /* ---- O_CLOEXEC at open, and dup of a cloexec fd clears it ---- */
    int ce = open("f", O_RDONLY | O_CLOEXEC);
    emit_i("open.cloexec", ce >= 0 ? getfd(ce) : -1);              /* 1 */
    int ce2 = dup(ce);
    emit_i("dup_of_cloexec.clears", ce2 >= 0 ? getfd(ce2) : -1);  /* 0 */

    /* F_SETFD toggles the bit */
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    emit_i("setfd.on", getfd(fd));     /* 1 */
    fcntl(fd, F_SETFD, 0);
    emit_i("setfd.off", getfd(fd));    /* 0 */

    return 0;
}
