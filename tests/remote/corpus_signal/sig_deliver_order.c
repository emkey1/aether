/*
 * sig_deliver_order.c — order in which pending signal HANDLERS actually RUN
 * when several become deliverable at once (the asynchronous receive_signals
 * path, as opposed to the synchronous sigtimedwait path of sig_rt_order.c).
 *
 * signal(7): low-numbered signals have highest priority; real-time signals are
 * delivered lowest-numbered first, same-number FIFO. We block a set, queue them
 * scrambled, then unblock all at once; each handler appends its own offset to a
 * sequence buffer, so the buffer is the actual RUN order.
 *
 * Only real-time signals are scrambled/asserted (standard-signal cross-number
 * order is POSIX-unspecified). The same static musl binary runs on iSH and on
 * mint's real Linux, so a divergent sequence is an iSH delivery-order bug.
 */
#include "sig_common.h"

#define N 4
static volatile sig_atomic_t seq[16];
static volatile sig_atomic_t seq_len;
static int g_base;

static void rec(int sig, siginfo_t *info, void *uc) {
    (void) info; (void) uc;
    if (seq_len < (int)(sizeof seq / sizeof seq[0]))
        seq[seq_len++] = sig - g_base;
}

static void install(int sig) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = rec;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(sig, &sa, NULL);
}

static void emit_seq(const char *key) {
    char buf[64]; int n = 0;
    for (int i = 0; i < seq_len; i++)
        n += snprintf(buf + n, sizeof buf - n, "%s%d", i ? "," : "", (int) seq[i]);
    if (seq_len == 0) snprintf(buf, sizeof buf, "none");
    emit(key, "%s", buf);
}

int main(int argc, char **argv) {
    sig_init(argc, argv);
    g_base = SIGRTMIN;

    sigset_t set, old;
    sigemptyset(&set);
    for (int i = 0; i < N; i++) {
        sigaddset(&set, g_base + i);
        install(g_base + i);
    }

    /* block, queue scrambled, then unblock all at once */
    sigprocmask(SIG_BLOCK, &set, &old);
    seq_len = 0;
    int order[N] = {3, 1, 2, 0};
    for (int i = 0; i < N; i++) {
        union sigval v; v.sival_int = order[i];
        sigqueue(getpid(), g_base + order[i], v);
    }
    /* atomically unblock the whole set -> all become deliverable together */
    sigprocmask(SIG_SETMASK, &old, NULL);
    /* handlers have run synchronously on the unblocking syscall's return */
    emit_seq("deliver.rt.order");
    emit_i("deliver.rt.count", seq_len);

    return 0;
}
