/*
 * proc_exec.c — execve(2) process-image setup: argv/argc, the environment, the
 * auxiliary vector (getauxval), close-on-exec fd handling, and signal-
 * disposition reset across exec.
 *
 * The test re-execs itself in a "reporter" mode that inspects the new image and
 * emits the observations. auxv values that legitimately vary by arch or host
 * (AT_PHENT byte size, AT_HWCAP, AT_UID under a non-root VM) are emitted as
 * derived invariants so the keys compare equal across every cell.
 */
#include "proc_common.h"
#include <sys/auxv.h>
#include <elf.h>

#define CLOEXEC_SLOT 101
#define KEEP_SLOT    100

static volatile sig_atomic_t exec_dummy;
static void exec_handler(int s) { (void) s; exec_dummy++; }

/* reporter: inspect the freshly-exec'd image and emit observations */
static int exec_report(int argc, char **argv) {
    proc_init(argc, argv);   /* carries engine via persistent /proc/ish state */

    /* argv / argc */
    emit_i("exec.argc", argc);
    emit_i("exec.argv1_is_report", argc > 1 && strcmp(argv[1], "--exec-report") == 0);
    emit_i("exec.argv2", argc > 2 && strcmp(argv[2], "ARG1") == 0);
    emit_i("exec.argv3", argc > 3 && strcmp(argv[3], "ARG2") == 0);

    /* environment was replaced with exactly our envp */
    const char *e = getenv("PROC_EXEC_ENV");
    emit_i("exec.env_value", e != NULL && strcmp(e, "value") == 0);
    emit_i("exec.env_replaced", getenv("PROC_EXEC_ABSENT") == NULL);

    /* close-on-exec: the FD_CLOEXEC slot is gone, the plain slot survives */
    emit_i("exec.cloexec_closed", fcntl(CLOEXEC_SLOT, F_GETFD) == -1 && errno == EBADF);
    emit_i("exec.keepfd_survived", fcntl(KEEP_SLOT, F_GETFD) != -1);

    /* signal dispositions: a handler is reset to SIG_DFL, SIG_IGN persists */
    struct sigaction got;
    sigaction(SIGUSR1, NULL, &got);
    emit_i("exec.handler_reset_to_dfl", got.sa_handler == SIG_DFL);
    sigaction(SIGUSR2, NULL, &got);
    emit_i("exec.ignore_persists", got.sa_handler == SIG_IGN);

    /* auxv: only arch/host-independent invariants */
    emit_i("exec.aux.pagesz_4096", getauxval(AT_PAGESZ) == 4096);
    emit_i("exec.aux.secure_0", getauxval(AT_SECURE) == 0);
    emit_i("exec.aux.uid_matches", (uid_t) getauxval(AT_UID) == getuid());
    emit_i("exec.aux.euid_matches", (uid_t) getauxval(AT_EUID) == geteuid());
    emit_i("exec.aux.gid_matches", (gid_t) getauxval(AT_GID) == getgid());
    emit_i("exec.aux.egid_matches", (gid_t) getauxval(AT_EGID) == getegid());
    emit_i("exec.aux.clktck_100", getauxval(AT_CLKTCK) == 100);
    emit_i("exec.aux.phnum_positive", getauxval(AT_PHNUM) > 0);
    emit_i("exec.aux.phent_positive", getauxval(AT_PHENT) > 0);
    emit_i("exec.aux.entry_nonzero", getauxval(AT_ENTRY) != 0);
    {
        const char *fn = (const char *) getauxval(AT_EXECFN);
        emit_i("exec.aux.execfn_matches_argv0", fn != NULL && strcmp(fn, argv[0]) == 0);
    }
    return 0;
}

/* driver: set up fds + dispositions, then execve into the reporter */
static void case_exec(char **argv) {
    pid_t pid = fork();
    if (pid == 0) {
        int fd = open("/", O_RDONLY);
        if (fd < 0) _exit(120);
        if (dup2(fd, KEEP_SLOT) != KEEP_SLOT) _exit(121);
        if (dup2(fd, CLOEXEC_SLOT) != CLOEXEC_SLOT) _exit(122);
        /* dup2 clears FD_CLOEXEC on the target, so set it after */
        fcntl(CLOEXEC_SLOT, F_SETFD, FD_CLOEXEC);
        if (fd != KEEP_SLOT && fd != CLOEXEC_SLOT) close(fd);

        signal(SIGUSR1, exec_handler);   /* must reset to SIG_DFL across exec */
        signal(SIGUSR2, SIG_IGN);        /* must persist across exec */

        char *const cargv[] = { argv[0], (char *) "--exec-report",
                                (char *) "ARG1", (char *) "ARG2", NULL };
        char *const cenv[]  = { (char *) "PROC_EXEC_ENV=value", NULL };
        execve(argv[0], cargv, cenv);
        _exit(127);                      /* exec failed */
    }
    int st = 0;
    proc_wait_blocking(pid, &st, 0);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0)
        emit("exec.driver", "EXECFAIL:%d", WIFEXITED(st) ? WEXITSTATUS(st) : -WTERMSIG(st));
}

/* execve of a path that doesn't exist -> ENOENT (a child, so a failed exec in
 * one cell can't take down the suite) */
static void case_exec_enoent(void) {
    pid_t pid = fork();
    if (pid == 0) {
        char *const cargv[] = { (char *) "/nonexistent/proc_exec_xyz", NULL };
        errno = 0;
        execv(cargv[0], cargv);
        _exit(errno == ENOENT ? 0 : 1);
    }
    int st = 0;
    proc_wait_blocking(pid, &st, 0);
    emit_i("exec.enoent", WIFEXITED(st) && WEXITSTATUS(st) == 0);
}

int main(int argc, char **argv) {
    if (argc >= 2 && strcmp(argv[1], "--exec-report") == 0)
        return exec_report(argc, argv);

    proc_init(argc, argv);
    case_exec(argv);
    case_exec_enoent();
    return 0;
}
