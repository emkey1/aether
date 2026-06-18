/*
 * proc_fork.c — fork(2) inheritance invariants: parentage, the shared open file
 * description (offset) vs the copied fd table, signal disposition/mask
 * inheritance, the cleared pending set, and the copied (independent) fs context
 * (cwd, umask).
 *
 * All values are derived invariants so they hold identically under iSH (root)
 * and mint (a non-root VM user).
 */
#include "proc_common.h"

/* child getppid() is the parent, and fork() returns the child's real pid */
static void case_parentage(void) {
    pid_t me = getpid();
    int p[2];
    if (pipe(p) != 0) { emit("fork.parentage", "%s", "PIPEFAIL"); return; }
    pid_t pid = fork();
    if (pid == 0) {
        close(p[0]);
        pid_t ppid = getppid();
        ssize_t w = write(p[1], &ppid, sizeof ppid);
        (void) w;
        _exit(0);
    }
    close(p[1]);
    pid_t child_ppid = 0;
    ssize_t r = read(p[0], &child_ppid, sizeof child_ppid);
    (void) r;
    close(p[0]);
    proc_wait_blocking(pid, NULL, 0);
    emit_i("fork.child_ppid_is_parent", child_ppid == me);
    emit_i("fork.ret_is_positive", pid > 0);
}

/* fork shares the open file description: a child's read advances the offset the
 * parent then sees (single shared file position) */
static void case_shared_offset(void) {
    char path[] = "/tmp/procforkXXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) { emit("fork.shared_offset", "%s", "MKSTEMPFAIL"); return; }
    ssize_t w = write(fd, "AB", 2); (void) w;
    lseek(fd, 0, SEEK_SET);
    int rd = open(path, O_RDONLY);
    unlink(path);
    if (rd < 0) { close(fd); emit("fork.shared_offset", "%s", "OPENFAIL"); return; }

    pid_t pid = fork();
    if (pid == 0) {
        char c;
        ssize_t n = read(rd, &c, 1);   /* reads 'A', advances shared offset */
        _exit(n == 1 && c == 'A' ? 0 : 1);
    }
    int st = 0;
    proc_wait_blocking(pid, &st, 0);
    char c = '?';
    ssize_t n = read(rd, &c, 1);       /* should see 'B' if offset is shared */
    emit_i("fork.shared_offset.parent_sees_B", n == 1 && c == 'B');
    emit_i("fork.shared_offset.child_read_A", WIFEXITED(st) && WEXITSTATUS(st) == 0);
    close(fd); close(rd);
}

static volatile sig_atomic_t dummy_count;
static void dummy_handler(int s) { (void) s; dummy_count++; }

/* signal dispositions and the signal mask are inherited across fork; the pending
 * set is NOT */
static void case_signal_inheritance(void) {
    /* SIGUSR1 -> SIG_IGN, SIGINT -> a handler, both should be inherited */
    signal(SIGUSR1, SIG_IGN);
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_handler = dummy_handler; sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);

    /* block + raise SIGUSR2 so it is pending in the parent at fork time */
    sigset_t block; sigemptyset(&block); sigaddset(&block, SIGUSR2);
    sigprocmask(SIG_BLOCK, &block, NULL);
    raise(SIGUSR2);

    int p[2];
    if (pipe(p) != 0) { emit("fork.signals", "%s", "PIPEFAIL"); return; }
    pid_t pid = fork();
    if (pid == 0) {
        close(p[0]);
        int res[4];
        struct sigaction got;
        sigaction(SIGUSR1, NULL, &got);
        res[0] = (got.sa_handler == SIG_IGN);                  /* disposition inherited */
        sigaction(SIGINT, NULL, &got);
        res[1] = (got.sa_handler == dummy_handler);            /* handler inherited */
        sigset_t cur; sigprocmask(SIG_SETMASK, NULL, &cur);
        res[2] = sigismember(&cur, SIGUSR2);                   /* mask inherited (blocked) */
        sigset_t pend; sigpending(&pend);
        res[3] = !sigismember(&pend, SIGUSR2);                 /* pending NOT inherited */
        ssize_t w = write(p[1], res, sizeof res); (void) w;
        _exit(0);
    }
    close(p[1]);
    int res[4] = {0};
    ssize_t r = read(p[0], res, sizeof res); (void) r;
    close(p[0]);
    proc_wait_blocking(pid, NULL, 0);

    emit_i("fork.disp.ign_inherited", res[0]);
    emit_i("fork.disp.handler_inherited", res[1]);
    emit_i("fork.mask.inherited", res[2]);
    emit_i("fork.pending.not_inherited", res[3]);

    /* restore parent state. SIGUSR2 is still pending+blocked in the parent;
     * ignore it before unblocking so its delivery is discarded rather than
     * terminating us (default disposition). */
    signal(SIGUSR2, SIG_IGN);
    sigprocmask(SIG_UNBLOCK, &block, NULL);
    signal(SIGUSR2, SIG_DFL);
    signal(SIGUSR1, SIG_DFL);
    signal(SIGINT, SIG_DFL);
}

/* fork COPIES the fs context: a child's chdir/umask do not affect the parent */
static void case_fs_independent(void) {
    char dir[] = "/tmp/procfsXXXXXX";
    if (!mkdtemp(dir)) { emit("fork.fs", "%s", "MKDTEMPFAIL"); return; }
    if (chdir(dir) != 0) { emit("fork.fs", "%s", "CHDIRFAIL"); return; }
    mode_t set = 0123;
    umask(set);

    pid_t pid = fork();
    if (pid == 0) {
        chdir("/");
        umask(0);            /* change child's copy only */
        _exit(0);
    }
    proc_wait_blocking(pid, NULL, 0);

    char cwd[256] = {0};
    char *gc = getcwd(cwd, sizeof cwd);
    emit_i("fork.cwd.parent_unchanged", gc != NULL && strcmp(cwd, dir) == 0);
    mode_t now = umask(022);   /* read back parent's umask */
    emit_i("fork.umask.parent_unchanged", now == set);

    chdir("/tmp");
    rmdir(dir);
}

/* the inherited umask value itself is observable in the child */
static void case_umask_inherited(void) {
    mode_t set = 0157;
    umask(set);
    int p[2];
    if (pipe(p) != 0) { emit("fork.umask_value", "%s", "PIPEFAIL"); return; }
    pid_t pid = fork();
    if (pid == 0) {
        close(p[0]);
        mode_t old = umask(0);             /* returns the inherited mask */
        ssize_t w = write(p[1], &old, sizeof old); (void) w;
        _exit(0);
    }
    close(p[1]);
    mode_t child_old = 0;
    ssize_t r = read(p[0], &child_old, sizeof child_old); (void) r;
    close(p[0]);
    proc_wait_blocking(pid, NULL, 0);
    emit_i("fork.umask.inherited_value", child_old == set);
    umask(022);
}

int main(int argc, char **argv) {
    proc_init(argc, argv);
    case_parentage();
    case_shared_offset();
    case_signal_inheritance();
    case_fs_independent();
    case_umask_inherited();
    return 0;
}
