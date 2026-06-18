/*
 * time_timerfd.c — timerfd_create / timerfd_settime / timerfd_gettime + the
 * read(2)/poll(2) semantics of a timer fd.
 *
 * Invariants:
 *   - create succeeds for CLOCK_MONOTONIC/REALTIME;
 *   - an invalid clock id and a clock that is not allowed for timerfd
 *     (CLOCK_PROCESS_CPUTIME_ID) -> EINVAL;
 *   - unknown create flags -> EINVAL;
 *   - after arming a 80ms one-shot a blocking read returns 1 expiration and
 *     waited >= 80ms; poll reports the fd readable;
 *   - timerfd_gettime reports a remaining it_value in (0, value];
 *   - O_NONBLOCK read before expiry -> EAGAIN;
 *   - unknown settime flags -> EINVAL.
 */
#include "time_common.h"

#include <sys/timerfd.h>

#ifndef TFD_NONBLOCK
#define TFD_NONBLOCK O_NONBLOCK
#endif
#ifndef TFD_CLOEXEC
#define TFD_CLOEXEC O_CLOEXEC
#endif

static volatile sig_atomic_t alarm_fired;
static void on_alrm(int s) { (void) s; alarm_fired = 1; }
static void arm_guard(int sec) {
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_alrm; sigemptyset(&sa.sa_mask);
    sigaction(SIGALRM, &sa, NULL);
    alarm_fired = 0; alarm(sec);
}

static struct itimerspec mk(long val_ms, long ival_ms) {
    struct itimerspec s; memset(&s, 0, sizeof s);
    s.it_value.tv_sec = val_ms / 1000;
    s.it_value.tv_nsec = (val_ms % 1000) * 1000000L;
    s.it_interval.tv_sec = ival_ms / 1000;
    s.it_interval.tv_nsec = (ival_ms % 1000) * 1000000L;
    return s;
}
static int64_t v_ns(struct itimerspec s) { return (int64_t) s.it_value.tv_sec * 1000000000LL + s.it_value.tv_nsec; }

/* create validity: good clocks OK, bad/ disallowed clock -> EINVAL, bad flags -> EINVAL */
static void case_create(void) {
    int fd;
    errno = 0; fd = timerfd_create(CLK_MONOTONIC, 0);
    emit("timerfd.create.monotonic", "%s", fd < 0 ? errno_name(errno) : "OK");
    if (fd >= 0) close(fd);
    errno = 0; fd = timerfd_create(CLK_REALTIME, 0);
    emit("timerfd.create.realtime", "%s", fd < 0 ? errno_name(errno) : "OK");
    if (fd >= 0) close(fd);

    errno = 0; fd = timerfd_create(99, 0);
    emit("timerfd.create.badclock", "%s", fd < 0 ? errno_name(errno) : "OK");
    if (fd >= 0) close(fd);

    /* CLOCK_PROCESS_CPUTIME_ID is a valid clock id but not allowed for timerfd */
    errno = 0; fd = timerfd_create(CLK_PROCESS_CPUTIME, 0);
    emit("timerfd.create.cputime_clock", "%s", fd < 0 ? errno_name(errno) : "OK");
    if (fd >= 0) close(fd);

    /* an unknown flag bit -> EINVAL */
    errno = 0; fd = timerfd_create(CLK_MONOTONIC, 0x40000);
    emit("timerfd.create.badflags", "%s", fd < 0 ? errno_name(errno) : "OK");
    if (fd >= 0) close(fd);
}

/* arm 80ms one-shot; blocking read returns 1 and waited >= 80ms; poll readable */
static void case_read_poll(void) {
    int fd = timerfd_create(CLK_MONOTONIC, 0);
    if (fd < 0) { emit("timerfd.read", "%s", "CREATEFAIL"); return; }
    struct itimerspec s = mk(80, 0);
    int sr = timerfd_settime(fd, 0, &s, NULL);
    emit("timerfd.settime.ret", "%s", res_errno(sr));

    struct timespec a, b;
    clk_now(CLK_MONOTONIC, &a);
    uint64_t ticks = 0;
    arm_guard(3);
    ssize_t n = read(fd, &ticks, sizeof ticks);
    alarm(0);
    clk_now(CLK_MONOTONIC, &b);
    if (n == (ssize_t) sizeof ticks) {
        emit_i("timerfd.read.expirations", (long) ticks);
        emit_b("timerfd.read.waited_enough", elapsed_ns(a, b) >= 80000000LL);
    } else {
        emit("timerfd.read.expirations", "%s", alarm_fired ? "TIMEOUT" : (n < 0 ? errno_name(errno) : "SHORT"));
    }

    /* re-arm and check poll readability after expiry */
    s = mk(60, 0);
    timerfd_settime(fd, 0, &s, NULL);
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    int pr = poll(&pfd, 1, 3000);
    emit_b("timerfd.poll.readable", pr == 1 && (pfd.revents & POLLIN));
    /* drain */
    if (pr == 1) { ssize_t r2 = read(fd, &ticks, sizeof ticks); (void) r2; }
    close(fd);
}

/* gettime reports a remaining value in (0, armed] right after arming */
static void case_gettime(void) {
    int fd = timerfd_create(CLK_MONOTONIC, 0);
    if (fd < 0) { emit("timerfd.gettime", "%s", "CREATEFAIL"); return; }
    struct itimerspec s = mk(500, 0), got;
    timerfd_settime(fd, 0, &s, NULL);
    memset(&got, 0, sizeof got);
    int r = timerfd_gettime(fd, &got);
    emit("timerfd.gettime.ret", "%s", res_errno(r));
    if (r == 0) {
        int64_t rem = v_ns(got);
        emit_b("timerfd.gettime.remaining_in_range", rem > 0 && rem <= 500000000LL);
    }
    close(fd);
}

/* O_NONBLOCK read before any expiry -> EAGAIN; unknown settime flags -> EINVAL */
static void case_nonblock_and_flags(void) {
    int fd = timerfd_create(CLK_MONOTONIC, TFD_NONBLOCK);
    if (fd < 0) { emit("timerfd.nonblock", "%s", "CREATEFAIL"); return; }
    uint64_t ticks = 0;
    errno = 0;
    ssize_t n = read(fd, &ticks, sizeof ticks);          /* not armed -> EAGAIN */
    emit("timerfd.nonblock.unarmed_read", "%s", n < 0 ? errno_name(errno) : "OK");

    struct itimerspec s = mk(50, 0);
    errno = 0;
    int r = timerfd_settime(fd, 0x40, &s, NULL);          /* bogus flag */
    emit("timerfd.settime.badflags", "%s", r < 0 ? errno_name(errno) : "OK");
    close(fd);
}

int main(int argc, char **argv) {
    time_init(argc, argv);
    case_create();
    case_read_poll();
    case_gettime();
    case_nonblock_and_flags();
    return 0;
}
