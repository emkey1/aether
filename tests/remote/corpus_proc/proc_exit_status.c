/*
 * proc_exit_status.c — wait(2) status-word encoding for the full range of child
 * terminations: normal exit (incl. >8-bit truncation), termination by signal,
 * and the raw encoded word itself.
 *
 * The encoding is spec-defined and arch-independent:
 *   normal exit(n)     -> (n & 0xff) << 8        WIFEXITED, WEXITSTATUS = n & 0xff
 *   killed by signal s -> s (& 0x7f), +0x80 core WIFSIGNALED, WTERMSIG = s
 * We force RLIMIT_CORE=0 so WCOREDUMP is a deterministic 0 on every cell (the
 * core-dump bit is otherwise config-dependent — masked, per the cmpxchg lesson).
 */
#include "proc_common.h"
#include <sys/resource.h>

/* fork a child that calls _exit(code); decode the reaped status */
static void exit_case(const char *key, int code) {
    pid_t pid = fork();
    if (pid == 0)
        _exit(code);
    int st = 0;
    proc_wait_blocking(pid, &st, 0);
    emit_wstatus(key, st);
}

/* fork a child that dies from signal `sig` (default disposition) */
static void signal_case(const char *key, int sig) {
    pid_t pid = fork();
    if (pid == 0) {
        signal(sig, SIG_DFL);
        raise(sig);
        _exit(0);          /* unreachable for a fatal signal */
    }
    int st = 0;
    proc_wait_blocking(pid, &st, 0);
    emit_wstatus(key, st);
}

int main(int argc, char **argv) {
    proc_init(argc, argv);

    /* deterministic, host-independent core bit */
    struct rlimit rl = { .rlim_cur = 0, .rlim_max = 0 };
    setrlimit(RLIMIT_CORE, &rl);

    /* normal exits, including >8-bit values (only the low 8 bits survive) */
    exit_case("exit.0", 0);
    exit_case("exit.1", 1);
    exit_case("exit.42", 42);
    exit_case("exit.255", 255);
    exit_case("exit.256", 256);     /* -> exit:0 */
    exit_case("exit.257", 257);     /* -> exit:1 */
    exit_case("exit.0x1ff", 0x1ff); /* -> exit:255 */

    /* the raw status word for a known exit is spec-exact and arch-independent */
    {
        pid_t pid = fork();
        if (pid == 0)
            _exit(42);
        int st = 0;
        proc_wait_blocking(pid, &st, 0);
        emit_i("exit.42.rawword", st);          /* 42 << 8 == 0x2a00 */
        emit_i("exit.42.WIFEXITED", WIFEXITED(st) ? 1 : 0);
        emit_i("exit.42.WIFSIGNALED", WIFSIGNALED(st) ? 1 : 0);
        emit_i("exit.42.WIFSTOPPED", WIFSTOPPED(st) ? 1 : 0);
    }

    /* termination by signal: terminating, core-generating, and SIGKILL */
    signal_case("sig.SIGTERM", SIGTERM);
    signal_case("sig.SIGINT", SIGINT);
    signal_case("sig.SIGKILL", SIGKILL);
    signal_case("sig.SIGABRT", SIGABRT);
    signal_case("sig.SIGQUIT", SIGQUIT);   /* core sig, but RLIMIT_CORE=0 -> core=0 */
    signal_case("sig.SIGSEGV", SIGSEGV);
    signal_case("sig.SIGUSR1", SIGUSR1);
    signal_case("sig.SIGPIPE", SIGPIPE);

    /* raw word for a signal death: low 7 bits == signal number, no 0x80 */
    {
        pid_t pid = fork();
        if (pid == 0) { signal(SIGTERM, SIG_DFL); raise(SIGTERM); _exit(0); }
        int st = 0;
        proc_wait_blocking(pid, &st, 0);
        emit_i("sig.SIGTERM.rawword", st);              /* 15 */
        emit_i("sig.SIGTERM.WIFSIGNALED", WIFSIGNALED(st) ? 1 : 0);
        emit_i("sig.SIGTERM.WIFEXITED", WIFEXITED(st) ? 1 : 0);
        emit_i("sig.SIGTERM.WTERMSIG", WIFSIGNALED(st) ? WTERMSIG(st) : -1);
    }

    return 0;
}
