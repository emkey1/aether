/*
 * time_wallclock.c — gettimeofday(2) / time(2) and the privileged setters
 * settimeofday/clock_settime.
 *
 * Invariants:
 *   - gettimeofday returns 0 with tv_usec in [0, 1e6) and an epoch tv_sec;
 *   - gettimeofday, time(), and clock_gettime(CLOCK_REALTIME) all agree to
 *     within a couple of seconds (they are the same underlying wall clock);
 *   - the wall clock does not jump backward across two close reads;
 *   - the privileged setters fail for an unprivileged caller. The mint oracle
 *     runs unprivileged, so settimeofday/clock_settime(CLOCK_REALTIME) ->
 *     EPERM there; clock_settime on a non-settable clock (CLOCK_MONOTONIC) ->
 *     EINVAL regardless of privilege. We pass the *current* time so that even
 *     a (wrongly) successful set is a harmless no-op.
 */
#include "time_common.h"

static void case_gettimeofday(void) {
    struct timeval tv;
    memset(&tv, 0, sizeof tv);
    errno = 0;
    int r = gettimeofday(&tv, NULL);
    emit("gtod.ret", "%s", res_errno(r));
    emit_b("gtod.usec_valid", tv.tv_usec >= 0 && tv.tv_usec < 1000000);
    emit_b("gtod.is_epoch", (int64_t) tv.tv_sec > 1500000000LL);
}

/* gettimeofday ~ time() ~ clock_gettime(REALTIME), all within 2s */
static void case_agreement(void) {
    struct timeval tv;
    struct timespec rt;
    gettimeofday(&tv, NULL);
    time_t t = time(NULL);
    clk_now(CLK_REALTIME, &rt);
    long d_tv_t = (long) (tv.tv_sec - t);
    long d_tv_rt = (long) (tv.tv_sec - rt.tv_sec);
    if (d_tv_t < 0) d_tv_t = -d_tv_t;
    if (d_tv_rt < 0) d_tv_rt = -d_tv_rt;
    emit_b("wall.gtod_eq_time", d_tv_t <= 2);
    emit_b("wall.gtod_eq_realtime", d_tv_rt <= 2);
}

/* the wall clock does not run backward across two reads */
static void case_nonbackward(void) {
    struct timeval a, b;
    gettimeofday(&a, NULL);
    for (volatile int i = 0; i < 100000; i++) { }
    gettimeofday(&b, NULL);
    int64_t ua = (int64_t) a.tv_sec * 1000000 + a.tv_usec;
    int64_t ub = (int64_t) b.tv_sec * 1000000 + b.tv_usec;
    emit_b("wall.gtod_nonbackward", ub >= ua);
}

/* time(NULL) and time(&t) agree */
static void case_time_arg(void) {
    time_t direct = 0;
    time_t ret = time(&direct);
    emit_b("time.arg_matches_ret", direct == ret);
    emit_b("time.is_epoch", (int64_t) ret > 1500000000LL);
}

/* privileged setters: unprivileged -> EPERM; non-settable clock -> EINVAL.
 * Pass the current time so a (wrong) success cannot disturb the host clock. */
static void case_setters(void) {
    struct timeval now;
    gettimeofday(&now, NULL);
    errno = 0;
    int r = settimeofday(&now, NULL);
    emit("settimeofday.unpriv", "%s", r < 0 ? errno_name(errno) : "OK");

    struct timespec rt;
    clk_now(CLK_REALTIME, &rt);
    errno = 0;
    r = clock_settime(CLK_REALTIME, &rt);
    emit("clock_settime.realtime_unpriv", "%s", r < 0 ? errno_name(errno) : "OK");

    /* CLOCK_MONOTONIC is never settable -> EINVAL on Linux regardless of uid */
    struct timespec mono;
    clk_now(CLK_MONOTONIC, &mono);
    errno = 0;
    r = clock_settime(CLK_MONOTONIC, &mono);
    emit("clock_settime.monotonic", "%s", r < 0 ? errno_name(errno) : "OK");
}

int main(int argc, char **argv) {
    time_init(argc, argv);
    case_gettimeofday();
    case_agreement();
    case_nonbackward();
    case_time_arg();
    case_setters();
    return 0;
}
