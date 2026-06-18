/*
 * proc_wait.c — wait4(2)/waitid(2) semantics: WNOHANG, the ECHILD corners,
 * WNOWAIT, P_PGID group waits, the siginfo (CLD_* code + bare status), option
 * validation, and the WUNTRACED/WCONTINUED stop/continue notifications.
 *
 * Blocking waits that might not be supported on a given cell are guarded by an
 * alarm() so an unimplemented feature surfaces as a clean divergence (EINTR)
 * rather than hanging the whole cell.
 */
#include "proc_common.h"

/* waitid CLD_* codes (arch-independent constants) */
#ifndef CLD_EXITED
#define CLD_EXITED 1
#define CLD_KILLED 2
#define CLD_DUMPED 3
#define CLD_STOPPED 5
#define CLD_CONTINUED 6
#endif

static volatile sig_atomic_t alarm_fired;
static void on_alarm(int sig) { (void) sig; alarm_fired = 1; }

static void install_alarm_handler(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_alarm;       /* deliberately no SA_RESTART */
    sigemptyset(&sa.sa_mask);
    sigaction(SIGALRM, &sa, NULL);
}

#define NOPID 0x3ffffff0

/* WNOHANG returns 0 while the child is alive, then the pid once it exits */
static void case_wnohang(void) {
    pid_t pid = fork();
    if (pid == 0) { proc_sleep_ms(120); _exit(5); }
    int st = 0;
    int rc0 = waitpid(pid, &st, WNOHANG);
    emit_i("wnohang.alive.ret", rc0);                 /* 0: alive, nothing to report */
    int rc1 = proc_wait_blocking(pid, &st, 0);
    emit_i("wnohang.then.ret_is_pid", rc1 == pid);
    emit_wstatus("wnohang.then.status", st);
}

/* ECHILD when there are no children at all (run in a child with none) */
static void case_echild_none(void) {
    pid_t pid = fork();
    if (pid == 0) {
        int st = 0;
        errno = 0;
        int rc = waitpid(-1, &st, 0);
        emit("echild.none", "%s", rc < 0 ? errno_name(errno) : "OK");
        _exit(0);
    }
    proc_wait_blocking(pid, NULL, 0);
}

/* a second wait on an already-reaped child -> ECHILD */
static void case_double_reap(void) {
    pid_t pid = fork();
    if (pid == 0) _exit(3);
    int st = 0;
    proc_wait_blocking(pid, &st, 0);
    errno = 0;
    int rc = waitpid(pid, &st, 0);
    emit("echild.doublereap", "%s", rc < 0 ? errno_name(errno) : "OK");
}

/* waiting for a pid that is not our child -> ECHILD */
static void case_wait_notchild(void) {
    int st = 0;
    errno = 0;
    int rc = waitpid(NOPID, &st, 0);
    emit("echild.notchild", "%s", rc < 0 ? errno_name(errno) : "OK");
}

/* waitid + WNOWAIT leaves the zombie reapable; the next non-WNOWAIT wait still
 * succeeds, and only then is the child gone (ECHILD afterwards). */
static void case_waitid_wnowait(void) {
    pid_t pid = fork();
    if (pid == 0) _exit(9);
    siginfo_t info;
    memset(&info, 0, sizeof info);
    int rc1 = waitid(P_PID, pid, &info, WEXITED | WNOWAIT);
    emit_i("wnowait.peek.rc", rc1);
    emit_i("wnowait.peek.code", info.si_code);
    emit_i("wnowait.peek.status", info.si_status);
    emit_i("wnowait.peek.pid_is_child", info.si_pid == pid);
    /* still reapable */
    memset(&info, 0, sizeof info);
    int rc2 = waitid(P_PID, pid, &info, WEXITED);
    emit_i("wnowait.reap.rc", rc2);
    emit_i("wnowait.reap.code", info.si_code);
    /* now gone */
    errno = 0;
    int rc3 = waitid(P_PID, pid, &info, WEXITED);
    emit("wnowait.after", "%s", rc3 < 0 ? errno_name(errno) : "OK");
}

/* waitid siginfo for a normal exit and a signal death */
static void case_waitid_siginfo(void) {
    pid_t pid = fork();
    if (pid == 0) _exit(12);
    siginfo_t info; memset(&info, 0, sizeof info);
    int rc = waitid(P_PID, pid, &info, WEXITED);
    emit_i("waitid.exit.rc", rc);
    emit_i("waitid.exit.signo_is_SIGCHLD", info.si_signo == SIGCHLD);
    emit_i("waitid.exit.code", info.si_code);             /* CLD_EXITED */
    emit_i("waitid.exit.status", info.si_status);         /* 12 (bare) */

    pid = fork();
    if (pid == 0) { signal(SIGKILL, SIG_DFL); raise(SIGKILL); _exit(0); }
    memset(&info, 0, sizeof info);
    rc = waitid(P_PID, pid, &info, WEXITED);
    emit_i("waitid.kill.rc", rc);
    emit_i("waitid.kill.code", info.si_code);             /* CLD_KILLED */
    emit_i("waitid.kill.status", info.si_status);         /* 9 (bare signo) */
}

/* a group wait: waitpid(-pgid) and waitid(P_PGID) reap a child placed in a new
 * process group */
static void case_group_wait(void) {
    pid_t pid = fork();
    if (pid == 0) {
        setpgid(0, 0);            /* new group == my pid */
        proc_sleep_ms(40);
        _exit(21);
    }
    setpgid(pid, pid);            /* race-free: set it from the parent too */
    int st = 0;
    int rc = waitpid(-pid, &st, 0);   /* wait for any child in group `pid` */
    emit_i("groupwait.ret_is_pid", rc == pid);
    emit_wstatus("groupwait.status", st);
}

/* invalid idtype / options */
static void case_bad_args(void) {
    siginfo_t info;
    errno = 0;
    int rc = waitid(99, 0, &info, WEXITED);
    emit("waitid.badidtype", "%s", rc < 0 ? errno_name(errno) : "OK");

    /* a wholly invalid option bit -> EINVAL on both waitid and wait4 */
    errno = 0;
    rc = waitid(P_ALL, 0, &info, 0x40);
    emit("waitid.badopt", "%s", rc < 0 ? errno_name(errno) : "OK");

    pid_t pid = fork();
    if (pid == 0) _exit(0);
    int st = 0;
    errno = 0;
    /* WEXITED (0x4) is a waitid-only flag: wait4/waitpid must reject it */
    rc = waitpid(pid, &st, WEXITED);
    emit("waitpid.wexited", "%s", rc < 0 ? errno_name(errno) : "OK");
    proc_wait_blocking(pid, &st, 0);    /* clean up regardless */
}

/* WUNTRACED reports a job-control stop; WCONTINUED reports the resume. Both
 * blocking waits are alarm-guarded (3s) so a cell lacking support diverges
 * cleanly instead of hanging. */
static void case_stop_continue(void) {
    int sync[2];
    if (pipe(sync) != 0) { emit("stopcont", "%s", "PIPEFAIL"); return; }
    pid_t pid = fork();
    if (pid == 0) {
        close(sync[1]);
        raise(SIGSTOP);                 /* stop; parent sees it via WUNTRACED */
        char b;                         /* resumed by SIGCONT; block until told */
        ssize_t r; do { r = read(sync[0], &b, 1); } while (r < 0 && errno == EINTR);
        _exit(33);
    }
    close(sync[0]);
    int st = 0;

    alarm_fired = 0; alarm(3);
    int rc = waitpid(pid, &st, WUNTRACED);
    alarm(0);
    if (rc == pid) emit_wstatus("stop.untraced", st);
    else emit("stop.untraced", "%s", alarm_fired ? "TIMEOUT" : (rc < 0 ? errno_name(errno) : "ZERO"));

    kill(pid, SIGCONT);
    alarm_fired = 0; alarm(3);
    rc = waitpid(pid, &st, WCONTINUED);
    alarm(0);
    if (rc == pid) emit_wstatus("cont.continued", st);
    else emit("cont.continued", "%s", alarm_fired ? "TIMEOUT" : (rc < 0 ? errno_name(errno) : "ZERO"));

    char go = 'g';
    ssize_t w = write(sync[1], &go, 1); (void) w;
    close(sync[1]);
    proc_wait_blocking(pid, &st, 0);
    emit_wstatus("stopcont.finalexit", st);
}

int main(int argc, char **argv) {
    proc_init(argc, argv);
    install_alarm_handler();

    case_wnohang();
    case_echild_none();
    case_double_reap();
    case_wait_notchild();
    case_waitid_wnowait();
    case_waitid_siginfo();
    case_group_wait();
    case_bad_args();
    case_stop_continue();
    return 0;
}
