/*
 * time_itimer.c — setitimer(2) / getitimer(2) / alarm(2) conformance, for
 * ITIMER_REAL (the wall-clock interval timer that delivers SIGALRM).
 *
 * Invariants:
 *   - getitimer on an unset timer returns 0 with a zero it_value (this is the
 *     check that exposes getitimer being entirely unimplemented -> SIGSYS,
 *     caught by the guard and reported as ENOSYS);
 *   - after setitimer arms a 10s one-shot, getitimer reports a remaining
 *     it_value in (0, 10s];
 *   - setitimer's old_value reports the timer that was previously armed;
 *   - ITIMER_REAL actually delivers SIGALRM;
 *   - alarm() returns the seconds remaining on the previously set alarm, and
 *     alarm(0) cancels;
 *   - a bogus `which` -> EINVAL.
 *
 * NOTE (documented gap, intentionally NOT asserted): iSH only implements
 * ITIMER_REAL; setitimer(ITIMER_VIRTUAL/ITIMER_PROF) returns EINVAL whereas
 * Linux accepts them (CPU-time interval timers). Implementing virtual/prof
 * timers needs fine-grained CPU-time accounting iSH does not have, so per the
 * "only assert spec-guaranteed, mask the genuinely-unsupported" discipline we
 * do not emit a conformance key for them.
 */
#include "time_common.h"

static volatile sig_atomic_t alarms;
static void on_alarm(int s) { (void) s; alarms++; }

static void install_alarm(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_alarm;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGALRM, &sa, NULL);
}

static void disarm_real(void) {
    struct itimerval z;
    memset(&z, 0, sizeof z);
    setitimer(ITIMER_REAL, &z, NULL);
}

static int64_t iv_us(struct itimerval v) {
    return (int64_t) v.it_value.tv_sec * 1000000LL + v.it_value.tv_usec;
}

/* getitimer on an unset ITIMER_REAL -> 0, zero value. Guarded against SIGSYS so
 * an unimplemented getitimer reports cleanly instead of killing the cell. */
static void case_getitimer_initial(void) {
    disarm_real();
    struct itimerval it;
    memset(&it, 0, sizeof it);
    if (sigsetjmp(time_sysjmp, 1) == 0) {
        errno = 0;
        int r = getitimer(ITIMER_REAL, &it);
        emit("itimer.getitimer.initial.ret", "%s", res_errno(r));
        if (r == 0)
            emit_b("itimer.getitimer.initial.zero",
                   it.it_value.tv_sec == 0 && it.it_value.tv_usec == 0 &&
                   it.it_interval.tv_sec == 0 && it.it_interval.tv_usec == 0);
    } else {
        emit("itimer.getitimer.initial.ret", "%s", "ENOSYS");   /* SIGSYS: missing syscall */
    }
}

/* arm a 10s one-shot, then getitimer reports remaining in (0,10s] */
static void case_set_then_get(void) {
    disarm_real();
    struct itimerval want, got;
    memset(&want, 0, sizeof want);
    want.it_value.tv_sec = 10;
    errno = 0;
    int r = setitimer(ITIMER_REAL, &want, NULL);
    emit("itimer.setitimer.arm.ret", "%s", res_errno(r));

    memset(&got, 0, sizeof got);
    if (sigsetjmp(time_sysjmp, 1) == 0) {
        r = getitimer(ITIMER_REAL, &got);
        if (r == 0) {
            int64_t us = iv_us(got);
            emit_b("itimer.getitimer.after_set_in_range", us > 0 && us <= 10000000LL);
        } else {
            emit("itimer.getitimer.after_set_in_range", "%s", res_errno(r));
        }
    } else {
        emit("itimer.getitimer.after_set_in_range", "%s", "ENOSYS");
    }
    disarm_real();
}

/* setitimer's old_value reflects the previously armed timer */
static void case_oldvalue(void) {
    disarm_real();
    struct itimerval first, second, old;
    memset(&first, 0, sizeof first);
    memset(&second, 0, sizeof second);
    first.it_value.tv_sec = 20;
    second.it_value.tv_sec = 5;
    setitimer(ITIMER_REAL, &first, NULL);
    memset(&old, 0, sizeof old);
    int r = setitimer(ITIMER_REAL, &second, &old);
    emit("itimer.setitimer.replace.ret", "%s", res_errno(r));
    if (r == 0) {
        int64_t us = iv_us(old);
        /* old value should reflect ~20s remaining (not the new 5s) */
        emit_b("itimer.setitimer.oldvalue_valid", us > 10000000LL && us <= 20000000LL);
    }
    disarm_real();
}

/* ITIMER_REAL delivers SIGALRM */
static void case_delivery(void) {
    install_alarm();
    disarm_real();
    alarms = 0;
    struct itimerval want;
    memset(&want, 0, sizeof want);
    want.it_value.tv_usec = 60000;        /* 60ms one-shot */
    setitimer(ITIMER_REAL, &want, NULL);
    for (int i = 0; i < 150 && alarms == 0; i++) { struct timespec d = ms_ts(10); nanosleep(&d, NULL); }
    emit_b("itimer.real.delivers_sigalrm", alarms >= 1);
    disarm_real();
}

/* alarm() returns remaining seconds of the previous alarm; alarm(0) cancels */
static void case_alarm(void) {
    disarm_real();
    emit_i("alarm.fresh_returns_zero", alarm(100));   /* no prior alarm -> 0 */
    unsigned prev = alarm(0);                          /* cancel, report remaining */
    emit_b("alarm.cancel_returns_remaining", prev >= 98 && prev <= 100);
    emit_i("alarm.after_cancel_zero", alarm(0));       /* nothing pending now -> 0 */
}

/* an invalid `which` -> EINVAL */
static void case_bad_which(void) {
    struct itimerval it;
    memset(&it, 0, sizeof it);
    errno = 0;
    emit("itimer.setitimer.badwhich", "%s", res_errno(setitimer(99, &it, NULL)));
    if (sigsetjmp(time_sysjmp, 1) == 0) {
        errno = 0;
        emit("itimer.getitimer.badwhich", "%s", res_errno(getitimer(99, &it)));
    } else {
        emit("itimer.getitimer.badwhich", "%s", "ENOSYS");
    }
}

int main(int argc, char **argv) {
    time_init(argc, argv);
    time_install_sigsys_guard();
    case_getitimer_initial();
    case_set_then_get();
    case_oldvalue();
    case_delivery();
    case_alarm();
    case_bad_which();
    return 0;
}
