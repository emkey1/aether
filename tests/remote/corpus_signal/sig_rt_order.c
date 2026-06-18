/*
 * sig_rt_order.c — real-time signal queueing + delivery ORDER + siginfo.
 *
 * POSIX / signal(7) guarantee for real-time signals (the spec-guaranteed
 * invariant we assert, per the cmpxchg lesson):
 *   - multiple *different* RT signals pending: delivered LOWEST-NUMBERED FIRST.
 *   - multiple instances of the *same* RT signal: delivered FIFO (oldest first),
 *     each carrying its own queued si_value.
 *
 * iSH kept a single FIFO insertion-ordered queue and dequeued the first match
 * (signal_take_next_locked / receive_signals), so it delivered in send order,
 * not lowest-numbered-first. We dequeue via sigtimedwait (drains the pending
 * queue without running handlers) and emit the (signal, value) sequence.
 *
 * Standard-signal cross-number order is left UNSPECIFIED by POSIX, so this test
 * only scrambles/【asserts】real-time signals, never standard ones.
 */
#include "sig_common.h"

int main(int argc, char **argv) {
    sig_init(argc, argv);

    const int base = SIGRTMIN;
    sigset_t set, old;
    sigemptyset(&set);
    for (int i = 0; i < 4; i++)
        sigaddset(&set, base + i);
    sigprocmask(SIG_BLOCK, &set, &old);

    struct timespec to = { .tv_sec = 5, .tv_nsec = 0 };

    /* --- phase 1: cross-number priority ---
     * queue four distinct RT signals in a scrambled order; lowest number must
     * come out first regardless of send order. */
    int send_off[4] = {2, 0, 3, 1};
    for (int i = 0; i < 4; i++) {
        union sigval v; v.sival_int = 0x100 + send_off[i];
        sigqueue(getpid(), base + send_off[i], v);
    }
    for (int step = 0; step < 4; step++) {
        siginfo_t info; memset(&info, 0, sizeof info);
        int s = sigtimedwait(&set, &info, &to);
        char k[64];
        snprintf(k, sizeof k, "rtorder.cross.step%d.off", step);
        emit_i(k, s < 0 ? -errno : s - base);
        snprintf(k, sizeof k, "rtorder.cross.step%d.val", step);
        emit_i(k, s < 0 ? -1 : info.si_value.sival_int);
        snprintf(k, sizeof k, "rtorder.cross.step%d.code", step);
        emit_i(k, s < 0 ? 0 : info.si_code);
    }

    /* --- phase 2: same-number FIFO interleaved with a higher number ---
     * three instances of base, then two of base+1. Correct drain order is
     * base(a), base(b), base(c) [FIFO], then base+1(d), base+1(e). */
    int vals_lo[3] = {0xA, 0xB, 0xC};
    int vals_hi[2] = {0xD, 0xE};
    for (int i = 0; i < 3; i++) {
        union sigval v; v.sival_int = vals_lo[i];
        sigqueue(getpid(), base, v);
    }
    for (int i = 0; i < 2; i++) {
        union sigval v; v.sival_int = vals_hi[i];
        sigqueue(getpid(), base + 1, v);
    }
    for (int step = 0; step < 5; step++) {
        siginfo_t info; memset(&info, 0, sizeof info);
        int s = sigtimedwait(&set, &info, &to);
        char k[64];
        snprintf(k, sizeof k, "rtorder.fifo.step%d.off", step);
        emit_i(k, s < 0 ? -errno : s - base);
        snprintf(k, sizeof k, "rtorder.fifo.step%d.val", step);
        emit_i(k, s < 0 ? -1 : info.si_value.sival_int);
    }

    /* queue must now be empty -> immediate EAGAIN with a zero timeout */
    {
        struct timespec zero = {0, 0};
        siginfo_t info;
        errno = 0;
        int s = sigtimedwait(&set, &info, &zero);
        emit("rtorder.drain.empty", "%s", s < 0 ? errno_name(errno) : "GOT");
    }

    sigprocmask(SIG_SETMASK, &old, NULL);
    return 0;
}
