/*
 * sig_block_pending.c — delivery vs the blocked/pending masks.
 *
 * Spec-guaranteed facts asserted here:
 *   - a blocked signal raised is reported pending by sigpending(2);
 *   - standard signals COALESCE: raising one twice while blocked delivers once;
 *   - real-time signals QUEUE: queueing three while blocked delivers three;
 *   - unblocking a pending signal delivers it;
 *   - sigprocmask returns the prior mask.
 * Cross-number delivery ORDER for *standard* signals is POSIX-unspecified, so
 * this test never asserts it — only counts and membership.
 */
#include "sig_common.h"

static volatile sig_atomic_t usr1_count;
static volatile sig_atomic_t rt_count;

static void count_usr1(int s) { (void) s; usr1_count++; }
static void count_rt(int s) { (void) s; rt_count++; }

static void install(int sig, void (*fn)(int)) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = fn;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);
    sigaction(sig, &sa, NULL);
}

int main(int argc, char **argv) {
    sig_init(argc, argv);
    install(SIGUSR1, count_usr1);
    install(SIGRTMIN, count_rt);

    /* --- standard signal coalescing while blocked --- */
    sigset_t one, old;
    sigemptyset(&one);
    sigaddset(&one, SIGUSR1);
    sigprocmask(SIG_BLOCK, &one, &old);

    usr1_count = 0;
    raise(SIGUSR1);
    raise(SIGUSR1);                 /* second is coalesced (standard signal) */

    sigset_t pend;
    sigpending(&pend);
    emit_i("blockpend.std.pending_member", sigismember(&pend, SIGUSR1));
    emit_i("blockpend.std.delivered_while_blocked", usr1_count);   /* 0 */

    sigprocmask(SIG_SETMASK, &old, NULL);   /* unblock -> one delivery */
    emit_i("blockpend.std.delivered_after_unblock", usr1_count);   /* 1 */

    /* --- real-time signal queueing while blocked --- */
    sigset_t rtset, rtold;
    sigemptyset(&rtset);
    sigaddset(&rtset, SIGRTMIN);
    sigprocmask(SIG_BLOCK, &rtset, &rtold);

    rt_count = 0;
    for (int i = 0; i < 3; i++) {
        union sigval v; v.sival_int = i;
        sigqueue(getpid(), SIGRTMIN, v);
    }
    sigpending(&pend);
    emit_i("blockpend.rt.pending_member", sigismember(&pend, SIGRTMIN));
    emit_i("blockpend.rt.delivered_while_blocked", rt_count);      /* 0 */

    sigprocmask(SIG_SETMASK, &rtold, NULL);  /* unblock -> three deliveries */
    emit_i("blockpend.rt.delivered_after_unblock", rt_count);      /* 3 */

    /* --- sigprocmask returns the prior mask correctly --- */
    sigset_t cur;
    sigprocmask(SIG_BLOCK, &one, NULL);
    sigprocmask(SIG_BLOCK, NULL, &cur);      /* query current */
    emit_i("blockpend.mask.usr1_blocked", sigismember(&cur, SIGUSR1));
    sigprocmask(SIG_UNBLOCK, &one, &cur);
    emit_i("blockpend.mask.prior_had_usr1", sigismember(&cur, SIGUSR1));

    /* --- SIGKILL/SIGSTOP cannot be blocked --- */
    sigset_t kill_set, after;
    sigemptyset(&kill_set);
    sigaddset(&kill_set, SIGKILL);
    sigaddset(&kill_set, SIGSTOP);
    sigprocmask(SIG_BLOCK, &kill_set, NULL);
    sigprocmask(SIG_BLOCK, NULL, &after);
    emit_i("blockpend.unblockable.kill", sigismember(&after, SIGKILL));   /* 0 */
    emit_i("blockpend.unblockable.stop", sigismember(&after, SIGSTOP));   /* 0 */

    return 0;
}
