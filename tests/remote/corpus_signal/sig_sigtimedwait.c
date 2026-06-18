/*
 * sig_sigtimedwait.c — sigtimedwait/sigwaitinfo accept semantics.
 *
 *   - a signal already pending (blocked) is returned immediately, and is
 *     CONSUMED without running its handler;
 *   - an empty wait times out with EAGAIN (not EINTR/0);
 *   - an out-of-range timeout is rejected with EINVAL;
 *   - the returned siginfo carries the right signo/si_code/si_value.
 */
#include "sig_common.h"

static volatile sig_atomic_t handler_ran;
static void note(int s) { (void) s; handler_ran++; }

int main(int argc, char **argv) {
    sig_init(argc, argv);

    /* install a handler so we can prove sigtimedwait consumes WITHOUT running it */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = note;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, NULL);

    sigset_t set, old;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    sigaddset(&set, SIGRTMIN);
    sigprocmask(SIG_BLOCK, &set, &old);

    /* immediate accept of an already-pending standard signal */
    handler_ran = 0;
    raise(SIGUSR1);
    struct timespec to = { .tv_sec = 5, .tv_nsec = 0 };
    siginfo_t info; memset(&info, 0, sizeof info);
    int s = sigtimedwait(&set, &info, &to);
    emit_i("stw.immediate.signo", s);
    emit_i("stw.immediate.is_usr1", s == SIGUSR1);
    emit_i("stw.immediate.handler_ran", handler_ran);    /* 0: consumed, not run */

    /* immediate accept of a queued RT signal carries si_value + SI_QUEUE */
    {
        union sigval v; v.sival_int = 0x77;
        sigqueue(getpid(), SIGRTMIN, v);
        memset(&info, 0, sizeof info);
        int r = sigtimedwait(&set, &info, &to);
        emit_i("stw.rt.signo_off", r < 0 ? -1 : r - SIGRTMIN);
        emit_i("stw.rt.code", info.si_code);             /* SI_QUEUE = -1 */
        emit_i("stw.rt.val", info.si_value.sival_int);   /* 0x77 */
        emit_i("stw.rt.pid_is_self", info.si_pid == getpid());
    }

    /* timeout with nothing pending -> EAGAIN */
    {
        struct timespec t = { .tv_sec = 0, .tv_nsec = 30 * 1000 * 1000 };
        memset(&info, 0, sizeof info);
        errno = 0;
        int r = sigtimedwait(&set, &info, &t);
        emit("stw.timeout.errno", "%s", r < 0 ? errno_name(errno) : "GOT");
    }

    /* zero timeout, nothing pending -> immediate EAGAIN (poll semantics) */
    {
        struct timespec t = { 0, 0 };
        memset(&info, 0, sizeof info);
        errno = 0;
        int r = sigtimedwait(&set, &info, &t);
        emit("stw.poll.errno", "%s", r < 0 ? errno_name(errno) : "GOT");
    }

    /* out-of-range timeout -> EINVAL */
    {
        struct timespec bad = { .tv_sec = 0, .tv_nsec = 2000000000 };
        memset(&info, 0, sizeof info);
        errno = 0;
        int r = sigtimedwait(&set, &info, &bad);
        emit("stw.badtimeout.errno", "%s", r < 0 ? errno_name(errno) : "GOT");
    }

    sigprocmask(SIG_SETMASK, &old, NULL);
    return 0;
}
