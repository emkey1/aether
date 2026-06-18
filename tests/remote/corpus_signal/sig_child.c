/*
 * sig_child.c — SIGCHLD siginfo (si_code/si_status) on child exit vs kill, and
 * the waitid(2) siginfo for the same events.
 *
 * Linux reports, for the parent's SA_SIGINFO SIGCHLD handler and for waitid:
 *   normal exit(n)        -> si_code = CLD_EXITED(1),  si_status = n      (bare)
 *   killed by signal s    -> si_code = CLD_KILLED(2),  si_status = s      (bare)
 * iSH delivered SIGCHLD with si_code = SI_KERNEL(128) and si_status = the
 * raw wait(2)-encoded status word (n<<8), which a CLD_-inspecting handler or a
 * waitid() caller reads wrong.
 */
#include "sig_common.h"
#include <sys/wait.h>

static volatile sig_atomic_t c_count;
static volatile sig_atomic_t c_code;
static volatile sig_atomic_t c_status;
static volatile sig_atomic_t c_signo;

static void chld(int sig, siginfo_t *info, void *uc) {
    (void) uc;
    c_count++;
    c_signo = sig;
    c_code = info ? info->si_code : 0xdead;
    c_status = info ? info->si_status : 0xdead;
}

static void install_chld(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = chld;
    sa.sa_flags = SA_SIGINFO;       /* deliberately NOT SA_RESTART */
    sigemptyset(&sa.sa_mask);
    sigaction(SIGCHLD, &sa, NULL);
}

/* run a child, let the async SIGCHLD handler observe it, then reap */
static void exit_case(void) {
    c_count = 0; c_code = 0xbad; c_status = 0xbad;
    pid_t pid = fork();
    if (pid == 0) { sig_sleep_ms(30); _exit(42); }
    for (int i = 0; i < 1000 && c_count == 0; i++) sig_sleep_ms(1);
    emit_i("sigchld.exit.code", c_code);
    emit_i("sigchld.exit.status", c_status);
    emit_i("sigchld.exit.signo", c_signo);
    emit_i("sigchld.exit.count", c_count);
    int ws = 0; waitpid(pid, &ws, 0);
}

static void killed_case(void) {
    c_count = 0; c_code = 0xbad; c_status = 0xbad;
    pid_t pid = fork();
    if (pid == 0) {
        signal(SIGTERM, SIG_DFL);
        sig_sleep_ms(30);
        raise(SIGTERM);          /* terminated by signal 15 */
        _exit(0);
    }
    for (int i = 0; i < 1000 && c_count == 0; i++) sig_sleep_ms(1);
    emit_i("sigchld.killed.code", c_code);
    emit_i("sigchld.killed.status", c_status);
    int ws = 0; waitpid(pid, &ws, 0);
}

/* waitid(2) populates a siginfo with the same CLD_ codes (no handler involved) */
static void waitid_case(void) {
    pid_t pid = fork();
    if (pid == 0) { _exit(7); }
    siginfo_t info; memset(&info, 0, sizeof info);
    int rc = waitid(P_PID, pid, &info, WEXITED);
    emit_i("waitid.exit.rc", rc);
    emit_i("waitid.exit.code", info.si_code);
    emit_i("waitid.exit.status", info.si_status);
    emit_i("waitid.exit.signo", info.si_signo);
    emit_i("waitid.exit.pid_is_child", info.si_pid == pid);
}

int main(int argc, char **argv) {
    sig_init(argc, argv);
    install_chld();
    exit_case();
    killed_case();
    /* default SIGCHLD disposition for the waitid case so no handler interferes */
    signal(SIGCHLD, SIG_DFL);
    waitid_case();
    return 0;
}
