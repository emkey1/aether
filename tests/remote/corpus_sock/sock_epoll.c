/*
 * sock_epoll.c — epoll level- vs edge-triggered semantics (EPOLLET),
 * EPOLLONESHOT disarm + EPOLL_CTL_MOD re-arm, the EPOLL_CTL error corners, and
 * the infinite-timeout path with an already-ready fd (regression for the
 * spurious-0 / libuv-assert bug).
 *
 * All readiness checks use timeout=0 so they are deterministic and never block.
 * The one infinite-timeout call is over an already-ready fd and alarm-guarded.
 */
#include "sock_common.h"
#include <sys/epoll.h>
#include <sys/eventfd.h>

static int ep_add(int ep, int fd, uint32_t events) {
    struct epoll_event ev = { .events = events, .data = { .fd = fd } };
    return epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev);
}
static int ep_mod(int ep, int fd, uint32_t events) {
    struct epoll_event ev = { .events = events, .data = { .fd = fd } };
    return epoll_ctl(ep, EPOLL_CTL_MOD, fd, &ev);
}
/* one non-blocking wait; returns the number of ready fds (0/1) */
static int ep_ready(int ep) {
    struct epoll_event evs[4];
    return epoll_wait(ep, evs, 4, 0);
}
static void drain_eventfd(int fd) { uint64_t v; ssize_t r = read(fd, &v, sizeof v); (void) r; }

/* LEVEL-triggered: a ready fd is reported on every wait until it is drained */
static void case_level(void) {
    int ep = epoll_create1(0);
    int efd = eventfd(0, EFD_NONBLOCK);
    ep_add(ep, efd, EPOLLIN);
    uint64_t one = 1; ssize_t w = write(efd, &one, sizeof one); (void) w;
    int r1 = ep_ready(ep);
    int r2 = ep_ready(ep);          /* not drained -> still reported (level) */
    drain_eventfd(efd);
    int r3 = ep_ready(ep);          /* drained -> no longer reported */
    emit_b("epoll.level_reports_until_drained", r1 == 1 && r2 == 1 && r3 == 0);
    close(efd); close(ep);
}

/* EDGE-triggered: reported once per ready transition, not again until a new one */
static void case_edge(void) {
    int ep = epoll_create1(0);
    int efd = eventfd(0, EFD_NONBLOCK);
    ep_add(ep, efd, EPOLLIN | EPOLLET);
    uint64_t one = 1; ssize_t w = write(efd, &one, sizeof one); (void) w;
    int r1 = ep_ready(ep);          /* the edge */
    int r2 = ep_ready(ep);          /* no new edge -> nothing */
    drain_eventfd(efd);             /* back to not-ready */
    ssize_t w2 = write(efd, &one, sizeof one); (void) w2;  /* not-ready -> ready: new edge */
    int r3 = ep_ready(ep);
    emit_b("epoll.edge_once_per_transition", r1 == 1 && r2 == 0 && r3 == 1);
    close(efd); close(ep);
}

/* EPOLLONESHOT: reported once, then disarmed until EPOLL_CTL_MOD re-arms */
static void case_oneshot(void) {
    int ep = epoll_create1(0);
    int efd = eventfd(0, EFD_NONBLOCK);
    ep_add(ep, efd, EPOLLIN | EPOLLONESHOT);
    uint64_t one = 1; ssize_t w = write(efd, &one, sizeof one); (void) w;
    int r1 = ep_ready(ep);          /* reported once */
    int r2 = ep_ready(ep);          /* disarmed (still readable, but oneshot) */
    int mr = ep_mod(ep, efd, EPOLLIN | EPOLLONESHOT);   /* re-arm */
    int r3 = ep_ready(ep);
    emit_b("epoll.oneshot_disarms", r1 == 1 && r2 == 0);
    emit_b("epoll.oneshot_mod_rearms", mr == 0 && r3 == 1);
    close(efd); close(ep);
}

/* EPOLL_CTL error corners */
static void case_ctl_errors(void) {
    int ep = epoll_create1(0);
    int efd = eventfd(0, EFD_NONBLOCK);

    emit("epoll.add_ok", "%s", res_errno(ep_add(ep, efd, EPOLLIN)));
    errno = 0;
    emit("epoll.add_twice_eexist", "%s", res_errno(ep_add(ep, efd, EPOLLIN)));

    int other = eventfd(0, EFD_NONBLOCK);
    errno = 0;
    emit("epoll.mod_missing_enoent", "%s", res_errno(ep_mod(ep, other, EPOLLIN)));
    errno = 0;
    emit("epoll.del_missing_enoent", "%s",
         res_errno(epoll_ctl(ep, EPOLL_CTL_DEL, other, NULL)));

    /* adding the epoll fd to itself is EINVAL */
    errno = 0;
    emit("epoll.add_self_einval", "%s", res_errno(ep_add(ep, ep, EPOLLIN)));

    /* a bogus op is EINVAL */
    struct epoll_event ev = { .events = EPOLLIN, .data = { .fd = efd } };
    errno = 0;
    emit("epoll.bad_op_einval", "%s", res_errno(epoll_ctl(ep, 9999, efd, &ev)));

    close(other); close(efd); close(ep);
}

/* infinite timeout over an already-ready fd returns it immediately (never a
 * spurious 0) */
static void case_infinite_ready(void) {
    int ep = epoll_create1(0);
    int efd = eventfd(0, EFD_NONBLOCK);
    ep_add(ep, efd, EPOLLIN);
    uint64_t one = 1; ssize_t w = write(efd, &one, sizeof one); (void) w;
    struct epoll_event evs[2];
    sock_arm_alarm(4);
    int r = epoll_wait(ep, evs, 2, -1);
    sock_disarm_alarm();
    emit_b("epoll.infinite_ready_returns",
           r == 1 && (evs[0].events & EPOLLIN) && evs[0].data.fd == efd);
    close(efd); close(ep);
}

int main(int argc, char **argv) {
    sock_init(argc, argv, "epoll");
    case_level();
    case_edge();
    case_oneshot();
    case_ctl_errors();
    case_infinite_ready();
    return 0;
}
