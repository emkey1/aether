/*
 * sig_si_code.c — siginfo si_code/si_pid/si_value for each signal-send path.
 *
 * Linux distinguishes the sender mechanism in si_code:
 *   kill(2)            -> SI_USER   (0)
 *   tkill/tgkill(2)    -> SI_TKILL  (-6)
 *   sigqueue/rt_*(2)   -> SI_QUEUE  (-1)   and carries si_value
 *   raise(3)           -> whatever syscall musl uses (tkill/tgkill -> SI_TKILL)
 *
 * iSH routed kill/tkill/tgkill through one do_kill->kill_task that hardcoded
 * SI_USER, so tkill/tgkill handlers saw si_code=0 instead of SI_TKILL. The
 * same static musl binary runs under iSH and under mint's real Linux, so a
 * divergence here is purely in iSH's kernel si_code assignment.
 */
#include "sig_common.h"

static volatile sig_atomic_t h_count;
static volatile sig_atomic_t h_signo;
static volatile sig_atomic_t h_code;
static volatile sig_atomic_t h_pid_is_self;
static volatile sig_atomic_t h_uid_is_self;
static volatile sig_atomic_t h_val;

static void handler(int sig, siginfo_t *info, void *uc) {
    (void) uc;
    h_count++;
    h_signo = sig;
    h_code = info ? info->si_code : 0xdead;
    h_pid_is_self = info ? (info->si_pid == getpid()) : -1;
    h_uid_is_self = info ? (info->si_uid == getuid()) : -1;
    h_val = info ? info->si_value.sival_int : 0;
}

static void install(int sig) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(sig, &sa, NULL);
}

static void reset(void) {
    h_count = 0; h_signo = 0; h_code = 0xbad;
    h_pid_is_self = -1; h_uid_is_self = -1; h_val = 0;
}

int main(int argc, char **argv) {
    sig_init(argc, argv);

    install(SIGUSR1);
    install(SIGUSR2);
    install(SIGRTMIN);

    /* kill(self) -> SI_USER */
    reset();
    kill(getpid(), SIGUSR1);
    emit_i("sicode.kill.code", h_code);
    emit_i("sicode.kill.pid_is_self", h_pid_is_self);
    emit_i("sicode.kill.uid_is_self", h_uid_is_self);
    emit_i("sicode.kill.signo", h_signo);
    emit_i("sicode.kill.count", h_count);

    /* tgkill(self) -> SI_TKILL */
    reset();
    syscall(SYS_tgkill, getpid(), sig_gettid(), SIGUSR2);
    emit_i("sicode.tgkill.code", h_code);
    emit_i("sicode.tgkill.pid_is_self", h_pid_is_self);
    emit_i("sicode.tgkill.signo", h_signo);
    emit_i("sicode.tgkill.count", h_count);

    /* tkill(self) -> SI_TKILL */
    reset();
    syscall(SYS_tkill, sig_gettid(), SIGUSR1);
    emit_i("sicode.tkill.code", h_code);
    emit_i("sicode.tkill.pid_is_self", h_pid_is_self);
    emit_i("sicode.tkill.signo", h_signo);

    /* raise() -> whichever syscall musl uses; same binary on both oracles */
    reset();
    raise(SIGUSR1);
    emit_i("sicode.raise.code", h_code);
    emit_i("sicode.raise.signo", h_signo);

    /* sigqueue -> SI_QUEUE + si_value */
    reset();
    {
        union sigval v; v.sival_int = 0x5151;
        sigqueue(getpid(), SIGRTMIN, v);
    }
    emit_i("sicode.sigqueue.code", h_code);
    emit_i("sicode.sigqueue.pid_is_self", h_pid_is_self);
    emit_i("sicode.sigqueue.val", h_val);
    emit_i("sicode.sigqueue.signo_off", h_signo - SIGRTMIN);

    return 0;
}
