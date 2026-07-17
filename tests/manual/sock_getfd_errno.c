// fs/sock.c's sock_getfd() collapsed two distinct failure modes into one:
// a socket fd that doesn't exist, and an fd that exists and is open but
// isn't a socket at all (e.g. a plain file). Every socket syscall in the
// file (bind, connect, getsockname, getpeername, listen, accept, send,
// recv, setsockopt, getsockopt, shutdown, sendmsg, recvmsg) treated both
// cases identically as EBADF. Real Linux distinguishes them: EBADF only
// when the fd is closed/out of range, ENOTSOCK when the fd is valid but
// refers to a non-socket file. Verified against a real kernel (mint oracle):
// getpeername(open regular-file fd) -> ENOTSOCK(88), not EBADF(9).
//
// Fixed by giving sock_getfd() an int_t *err out-parameter so it can report
// EBADF vs ENOTSOCK distinctly, and updating every call site to propagate
// that specific error instead of a hardcoded EBADF.
#define _GNU_SOURCE
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "test_common.h"

static void check_errno(const char *label, int ret, int want_ret, int want_errno) {
    int err = errno;
    if (ret != want_ret || (want_ret < 0 && err != want_errno)) {
        printf("FAIL %s: ret=%d errno=%d(%s) want ret=%d errno=%d(%s)\n",
               label, ret, err, strerror(err), want_ret, want_errno, strerror(want_errno));
        failures_total++;
    } else {
        test_logf("%s: ret=%d errno=%d ok\n", label, ret, err);
    }
}

int main(int argc, char **argv) {
    test_init(argc, argv);

    int file_fd = open("/etc/hostname", O_RDONLY);
    if (file_fd < 0) {
        printf("FAIL: open /etc/hostname: %s\n", strerror(errno));
        return finish_suite("sock_getfd_errno");
    }
    // Well past any fd this process has open.
    int bad_fd = 987654;

    struct sockaddr_storage ss;
    socklen_t len = sizeof(ss);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    int one = 1;
    socklen_t olen = sizeof(one);

    errno = 0;
    check_errno("getpeername(regular file)", getpeername(file_fd, (struct sockaddr *) &ss, &len), -1, ENOTSOCK);
    errno = 0;
    check_errno("getpeername(bad fd)", getpeername(bad_fd, (struct sockaddr *) &ss, &len), -1, EBADF);

    len = sizeof(ss);
    errno = 0;
    check_errno("getsockname(regular file)", getsockname(file_fd, (struct sockaddr *) &ss, &len), -1, ENOTSOCK);
    errno = 0;
    check_errno("getsockname(bad fd)", getsockname(bad_fd, (struct sockaddr *) &ss, &len), -1, EBADF);

    errno = 0;
    check_errno("bind(regular file)", bind(file_fd, (struct sockaddr *) &a, sizeof(a)), -1, ENOTSOCK);
    errno = 0;
    check_errno("bind(bad fd)", bind(bad_fd, (struct sockaddr *) &a, sizeof(a)), -1, EBADF);

    errno = 0;
    check_errno("connect(regular file)", connect(file_fd, (struct sockaddr *) &a, sizeof(a)), -1, ENOTSOCK);
    errno = 0;
    check_errno("connect(bad fd)", connect(bad_fd, (struct sockaddr *) &a, sizeof(a)), -1, EBADF);

    errno = 0;
    check_errno("listen(regular file)", listen(file_fd, 1), -1, ENOTSOCK);
    errno = 0;
    check_errno("listen(bad fd)", listen(bad_fd, 1), -1, EBADF);

    errno = 0;
    check_errno("shutdown(regular file)", shutdown(file_fd, SHUT_RDWR), -1, ENOTSOCK);
    errno = 0;
    check_errno("shutdown(bad fd)", shutdown(bad_fd, SHUT_RDWR), -1, EBADF);

    errno = 0;
    check_errno("setsockopt(regular file)", setsockopt(file_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)), -1, ENOTSOCK);
    errno = 0;
    check_errno("setsockopt(bad fd)", setsockopt(bad_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)), -1, EBADF);

    errno = 0;
    check_errno("getsockopt(regular file)", getsockopt(file_fd, SOL_SOCKET, SO_REUSEADDR, &one, &olen), -1, ENOTSOCK);
    errno = 0;
    check_errno("getsockopt(bad fd)", getsockopt(bad_fd, SOL_SOCKET, SO_REUSEADDR, &one, &olen), -1, EBADF);

    close(file_fd);
    return finish_suite("sock_getfd_errno");
}
