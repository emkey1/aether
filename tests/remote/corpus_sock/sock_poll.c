/*
 * sock_poll.c — poll(2)/select(2)/ppoll(2) readiness on sockets and pipes:
 * writable-when-empty, readable-after-write, POLLHUP after peer close, POLLNVAL
 * on a closed fd, negative-fd ignored, and the timeout=0 fast path.
 */
#include "sock_common.h"

/* a fresh connected stream pair is writable and not readable; after a write the
 * peer becomes readable */
static void case_basic(void) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) { emit("poll.basic", "%s", "SETUPFAIL"); return; }
    short rev = 0;
    sock_poll_one(sv[0], POLLOUT, 0, &rev);
    emit_b("poll.fresh_writable", (rev & POLLOUT) != 0);
    sock_poll_one(sv[1], POLLIN, 0, &rev);
    emit_b("poll.fresh_not_readable", !(rev & POLLIN));
    write(sv[0], "x", 1);
    sock_sleep_ms(10);
    sock_poll_one(sv[1], POLLIN, 0, &rev);
    emit_b("poll.readable_after_write", (rev & POLLIN) != 0);
    close(sv[0]); close(sv[1]);
}

/* POLLHUP is reported (regardless of the requested events) once the peer closes */
static void case_hup(void) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) { emit("poll.hup", "%s", "SETUPFAIL"); return; }
    close(sv[1]);
    sock_sleep_ms(10);
    short rev = 0;
    /* request only POLLOUT; POLLHUP comes back unsolicited */
    int r = sock_poll_one(sv[0], POLLOUT, 0, &rev);
    emit_b("poll.hup_after_peer_close", r == 1 && (rev & POLLHUP));
    close(sv[0]);
}

/* a closed fd reports POLLNVAL; poll counts it as ready */
static void case_nval(void) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) { emit("poll.nval", "%s", "SETUPFAIL"); return; }
    int dead = sv[0];
    close(dead);
    short rev = 0;
    int r = sock_poll_one(dead, POLLIN, 0, &rev);
    emit_b("poll.closed_fd_nval", r == 1 && (rev & POLLNVAL));
    close(sv[1]);
}

/* a negative fd in the pollfd array is skipped (revents 0), not an error */
static void case_negative_fd(void) {
    struct pollfd pfd = { .fd = -1, .events = POLLIN, .revents = 0xff };
    int r = poll(&pfd, 1, 0);
    emit_b("poll.negative_fd_ignored", r == 0 && pfd.revents == 0);
}

/* timeout=0 with nothing ready returns 0 immediately */
static void case_timeout_zero(void) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) { emit("poll.t0", "%s", "SETUPFAIL"); return; }
    struct pollfd pfd = { .fd = sv[1], .events = POLLIN, .revents = 0 };
    int r = poll(&pfd, 1, 0);
    emit_b("poll.timeout_zero_no_events", r == 0);
    close(sv[0]); close(sv[1]);
}

/* select() agrees that a written-to pipe read end is readable */
static void case_select(void) {
    int p[2];
    if (pipe(p) < 0) { emit("poll.select", "%s", "SETUPFAIL"); return; }
    write(p[1], "s", 1);
    sock_sleep_ms(10);
    fd_set rfds; FD_ZERO(&rfds); FD_SET(p[0], &rfds);
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    int r = select(p[0] + 1, &rfds, NULL, NULL, &tv);
    emit_b("poll.select_readable", r == 1 && FD_ISSET(p[0], &rfds));
    close(p[0]); close(p[1]);
}

/* ppoll with a zero timeout behaves like poll(timeout=0) */
static void case_ppoll(void) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) { emit("poll.ppoll", "%s", "SETUPFAIL"); return; }
    write(sv[0], "p", 1);
    sock_sleep_ms(10);
    struct pollfd pfd = { .fd = sv[1], .events = POLLIN, .revents = 0 };
    struct timespec ts = { .tv_sec = 1, .tv_nsec = 0 };
    int r = ppoll(&pfd, 1, &ts, NULL);
    emit_b("poll.ppoll_readable", r == 1 && (pfd.revents & POLLIN));
    close(sv[0]); close(sv[1]);
}

int main(int argc, char **argv) {
    sock_init(argc, argv, "poll");
    case_basic();
    case_hup();
    case_nval();
    case_negative_fd();
    case_timeout_zero();
    case_select();
    case_ppoll();
    return 0;
}
