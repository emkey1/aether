// epoll_ctl(EPOLL_CTL_ADD) nesting-cycle detection.
//
// kernel/epoll.c had no cycle/depth check at all for indirect epoll nesting
// (only a direct fd == epfd self-add was rejected, with EINVAL). Adding an
// epoll fd that already (transitively) watches the target epoll instance --
// e.g. ep1 ADD ep2, then ep2 ADD ep1 -- silently succeeded instead of
// failing with ELOOP like real Linux. Because the call "succeeded" (returned
// 0), errno was left untouched from whatever unrelated syscall last set it,
// which is exactly how stress-ng's --epoll stressor surfaced this: it
// reported the stale errno (commonly EAGAIN, left over from a nonblocking
// accept4() a moment earlier) instead of the real defect. Regression for
// kernel/epoll.c's epoll_would_loop / sys_epoll_ctl_guest EPOLL_CTL_ADD path.
#define _GNU_SOURCE
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include "test_common.h"

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

    int ep1 = epoll_create1(0);
    int ep2 = epoll_create1(0);
    if (ep1 < 0 || ep2 < 0) {
        printf("FAIL: epoll_create1: %s\n", strerror(errno));
        return finish_suite("epoll_eloop");
    }

    struct epoll_event ev;
    memset(&ev, 0, sizeof ev);

    // Direct self-add: always EINVAL, distinct from the indirect-cycle case.
    ev.events = EPOLLIN;
    ev.data.fd = ep1;
    errno = 0;
    check_errno("ep1 ADD ep1 (self)", epoll_ctl(ep1, EPOLL_CTL_ADD, ep1, &ev), -1, EINVAL);

    // ep1 watches ep2: fine, ordinary one-level nesting.
    ev.events = EPOLLIN;
    ev.data.fd = ep2;
    errno = 0;
    check_errno("ep1 ADD ep2", epoll_ctl(ep1, EPOLL_CTL_ADD, ep2, &ev), 0, 0);

    // ep2 watching ep1 now would close a 2-instance cycle: ELOOP.
    ev.events = EPOLLIN;
    ev.data.fd = ep1;
    errno = 0;
    check_errno("ep2 ADD ep1 (cycle)", epoll_ctl(ep2, EPOLL_CTL_ADD, ep1, &ev), -1, ELOOP);

    // Tear the nesting back down, then a fresh (non-cyclic) add must succeed.
    check_errno("ep1 DEL ep2", epoll_ctl(ep1, EPOLL_CTL_DEL, ep2, NULL), 0, 0);
    ev.events = EPOLLIN;
    ev.data.fd = ep1;
    errno = 0;
    check_errno("ep2 ADD ep1 (after untangling)", epoll_ctl(ep2, EPOLL_CTL_ADD, ep1, &ev), 0, 0);

    // A 3-instance cycle (ep1 -> ep2 -> ep3 -> ep1) must also be rejected.
    check_errno("ep2 DEL ep1 (reset)", epoll_ctl(ep2, EPOLL_CTL_DEL, ep1, NULL), 0, 0);
    int ep3 = epoll_create1(0);
    if (ep3 < 0) {
        printf("FAIL: epoll_create1 ep3: %s\n", strerror(errno));
        failures_total++;
    } else {
        ev.events = EPOLLIN; ev.data.fd = ep2;
        errno = 0;
        check_errno("ep1 ADD ep2 (3-cycle)", epoll_ctl(ep1, EPOLL_CTL_ADD, ep2, &ev), 0, 0);
        ev.events = EPOLLIN; ev.data.fd = ep3;
        errno = 0;
        check_errno("ep2 ADD ep3 (3-cycle)", epoll_ctl(ep2, EPOLL_CTL_ADD, ep3, &ev), 0, 0);
        ev.events = EPOLLIN; ev.data.fd = ep1;
        errno = 0;
        check_errno("ep3 ADD ep1 (closes 3-cycle)", epoll_ctl(ep3, EPOLL_CTL_ADD, ep1, &ev), -1, ELOOP);
        close(ep3);
    }

    close(ep1);
    close(ep2);
    return finish_suite("epoll_eloop");
}
