/*
 * time_timer.c — POSIX per-process timers: timer_create / timer_settime /
 * timer_gettime / timer_getoverrun / timer_delete.
 *
 * Invariants:
 *   - create succeeds for the usual clocks; a bogus clock id -> EINVAL;
 *   - after arming a one-shot for 100ms, timer_gettime reports a remaining
 *     it_value in (0, 100ms] and it_interval == 0. (This is the key check that
 *     trips an ABI-width marshalling bug: if the kernel reads/writes the wrong
 *     itimerspec layout the remaining value is garbage / zero.)
 *   - a SIGEV_SIGNAL timer actually delivers its signal, with si_code SI_TIMER;
 *   - an interval timer keeps the it_interval it was given;
 *   - SIGEV_NONE arms and counts down but delivers nothing;
 *   - operating on a deleted / never-created timer id -> EINVAL.
 *
 * Uses the libc timer_* wrappers so the arch-correct struct itimerspec is
 * marshalled (i386: 32-bit fields; amd64: 64-bit fields).
 */
#include "time_common.h"

#ifndef SI_TIMER
#define SI_TIMER -2
#endif

static volatile sig_atomic_t timer_fires;
static volatile int timer_si_code;
static void on_timer_sig(int sig, siginfo_t *si, void *uc) {
    (void) sig; (void) uc;
    timer_fires++;
    timer_si_code = si ? si->si_code : 0;
}

static void install_rt_handler(int signo) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_timer_sig;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(signo, &sa, NULL);
}

static struct itimerspec mk_spec(long val_ms, long ival_ms) {
    struct itimerspec s;
    memset(&s, 0, sizeof s);
    s.it_value.tv_sec = val_ms / 1000;
    s.it_value.tv_nsec = (val_ms % 1000) * 1000000L;
    s.it_interval.tv_sec = ival_ms / 1000;
    s.it_interval.tv_nsec = (ival_ms % 1000) * 1000000L;
    return s;
}

static int64_t itv_ns(struct itimerspec s) { return (int64_t) s.it_value.tv_sec * 1000000000LL + s.it_value.tv_nsec; }
static int64_t iti_ns(struct itimerspec s) { return (int64_t) s.it_interval.tv_sec * 1000000000LL + s.it_interval.tv_nsec; }

/* create -> arm one-shot 100ms -> gettime remaining must be in (0,100ms],
 * interval 0. This is the amd64 itimerspec-width canary. */
static void case_arm_gettime(void) {
    struct sigevent sev;
    memset(&sev, 0, sizeof sev);
    sev.sigev_notify = SIGEV_SIGNAL;
    sev.sigev_signo = SIGUSR1;
    timer_t t;
    errno = 0;
    int r = timer_create(CLK_MONOTONIC, &sev, &t);
    emit("timer.create.ret", "%s", res_errno(r));
    if (r != 0)
        return;

    struct itimerspec want = mk_spec(100, 0), got;
    r = timer_settime(t, 0, &want, NULL);
    emit("timer.settime.ret", "%s", res_errno(r));

    memset(&got, 0, sizeof got);
    r = timer_gettime(t, &got);
    emit("timer.gettime.ret", "%s", res_errno(r));
    if (r == 0) {
        int64_t rem = itv_ns(got);
        emit_b("timer.gettime.remaining_in_range", rem > 0 && rem <= 100000000LL);
        emit_b("timer.gettime.interval_zero", iti_ns(got) == 0);
    }
    timer_delete(t);
}

/* an interval timer reports back the interval it was set with */
static void case_interval_roundtrip(void) {
    struct sigevent sev;
    memset(&sev, 0, sizeof sev);
    sev.sigev_notify = SIGEV_SIGNAL;
    sev.sigev_signo = SIGUSR1;
    timer_t t;
    if (timer_create(CLK_MONOTONIC, &sev, &t) != 0) { emit("timer.interval", "%s", "CREATEFAIL"); return; }
    struct itimerspec want = mk_spec(500, 250), got;
    timer_settime(t, 0, &want, NULL);
    memset(&got, 0, sizeof got);
    timer_gettime(t, &got);
    emit_b("timer.interval.preserved", iti_ns(got) == 250000000LL);
    timer_delete(t);
}

/* a SIGEV_SIGNAL one-shot actually delivers, with si_code SI_TIMER */
static void case_delivery(void) {
    install_rt_handler(SIGUSR1);
    struct sigevent sev;
    memset(&sev, 0, sizeof sev);
    sev.sigev_notify = SIGEV_SIGNAL;
    sev.sigev_signo = SIGUSR1;
    timer_t t;
    if (timer_create(CLK_MONOTONIC, &sev, &t) != 0) { emit("timer.delivery", "%s", "CREATEFAIL"); return; }
    timer_fires = 0; timer_si_code = 0;
    struct itimerspec want = mk_spec(50, 0);
    timer_settime(t, 0, &want, NULL);
    /* wait up to ~1.5s for the one fire */
    for (int i = 0; i < 150 && timer_fires == 0; i++) { struct timespec d = ms_ts(10); nanosleep(&d, NULL); }
    emit_b("timer.delivery.fired", timer_fires >= 1);
    if (timer_fires >= 1)
        emit_i("timer.delivery.si_code", timer_si_code);   /* SI_TIMER == -2 */
    timer_delete(t);
}

/* SIGEV_NONE: counts down (gettime shrinks) but delivers nothing */
static void case_sigev_none(void) {
    struct sigevent sev;
    memset(&sev, 0, sizeof sev);
    sev.sigev_notify = SIGEV_NONE;
    timer_t t;
    errno = 0;
    int r = timer_create(CLK_MONOTONIC, &sev, &t);
    emit("timer.sigevnone.create", "%s", res_errno(r));
    if (r != 0)
        return;
    struct itimerspec want = mk_spec(400, 0), g1, g2;
    timer_settime(t, 0, &want, NULL);
    memset(&g1, 0, sizeof g1); timer_gettime(t, &g1);
    struct timespec d = ms_ts(80); nanosleep(&d, NULL);
    memset(&g2, 0, sizeof g2); timer_gettime(t, &g2);
    emit_b("timer.sigevnone.counts_down", itv_ns(g2) < itv_ns(g1) && itv_ns(g2) > 0);
    timer_delete(t);
}

/* bad clock at create; operations on a never-created / deleted timer id */
static void case_errors(void) {
    struct sigevent sev;
    memset(&sev, 0, sizeof sev);
    sev.sigev_notify = SIGEV_SIGNAL;
    sev.sigev_signo = SIGUSR1;
    timer_t t;
    errno = 0;
    int r = timer_create(99, &sev, &t);
    emit("timer.create.badclock", "%s", res_errno(r));

    /* an invalid sigev_notify method -> EINVAL */
    struct sigevent bad;
    memset(&bad, 0, sizeof bad);
    bad.sigev_notify = 0x7f;
    errno = 0;
    r = timer_create(CLK_MONOTONIC, &bad, &t);
    emit("timer.create.badnotify", "%s", res_errno(r));

    /* create then delete, then settime/gettime/getoverrun on the dead id.
     * (timer_t is the kernel timer id cast to a pointer on musl, so reusing it
     * after delete exercises the kernel's "unknown timer id" path -> EINVAL.) */
    if (timer_create(CLK_MONOTONIC, &sev, &t) == 0) {
        timer_delete(t);
        struct itimerspec s = mk_spec(10, 0), got;
        errno = 0; emit("timer.settime.deleted", "%s", res_errno(timer_settime(t, 0, &s, NULL)));
        errno = 0; emit("timer.gettime.deleted", "%s", res_errno(timer_gettime(t, &got)));
        errno = 0; emit("timer.getoverrun.deleted", "%s", res_errno(timer_getoverrun(t)));
    }
}

int main(int argc, char **argv) {
    time_init(argc, argv);
    time_install_sigsys_guard();
    case_arm_gettime();
    case_interval_roundtrip();
    case_delivery();
    case_sigev_none();
    case_errors();
    return 0;
}
