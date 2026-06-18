/*
 * proc_session.c — sessions and process groups: setsid/getsid, setpgid/getpgid,
 * and the error-code corners (ESRCH/EPERM/EINVAL/EACCES) that emulators tend to
 * get wrong.
 *
 * Most cases run inside a forked child so the test process's own session is not
 * disturbed and each case starts from a clean, known state. Children emit their
 * own observation lines (stdout is unbuffered — see proc_init).
 */
#include "proc_common.h"

/* a re-exec'd instance that just blocks, used to test "setpgid a child that has
 * already exec'd -> EACCES". It signals readiness by writing one byte to the fd
 * named in argv[2], then sleeps until killed. */
static int exec_sleeper(int argc, char **argv) {
    if (argc >= 3) {
        int fd = atoi(argv[2]);
        char b = 'x';
        ssize_t w = write(fd, &b, 1);
        (void) w;
    }
    for (;;)
        pause();
    return 0;
}

static void case_setsid_basic(void) {
    pid_t pid = fork();
    if (pid == 0) {
        pid_t rc = setsid();
        emit_i("setsid.rc_eq_pid", rc == getpid());
        emit_i("setsid.sid_eq_pid", getsid(0) == getpid());
        emit_i("setsid.pgrp_eq_pid", getpgrp() == getpid());
        _exit(0);
    }
    proc_wait_blocking(pid, NULL, 0);
}

static void case_setsid_already_leader(void) {
    pid_t pid = fork();
    if (pid == 0) {
        setsid();                 /* now a session+group leader */
        errno = 0;
        pid_t rc = setsid();      /* leader can't create a new session */
        emit("setsid.again", "%s", res_errno(rc));
        _exit(0);
    }
    proc_wait_blocking(pid, NULL, 0);
}

static void case_setpgid_self_new_group(void) {
    pid_t pid = fork();
    if (pid == 0) {
        errno = 0;
        int rc = setpgid(0, 0);   /* create a new group == our pid */
        emit("setpgid.self.rc", "%s", res_errno(rc));
        emit_i("setpgid.self.pgrp_eq_pid", getpgrp() == getpid());
        _exit(0);
    }
    proc_wait_blocking(pid, NULL, 0);
}

static void case_setpgid_negative_pgid(void) {
    pid_t pid = fork();
    if (pid == 0) {
        errno = 0;
        int rc = setpgid(0, -3);  /* negative pgid -> EINVAL on Linux */
        emit("setpgid.negpgid", "%s", res_errno(rc));
        _exit(0);
    }
    proc_wait_blocking(pid, NULL, 0);
}

static void case_setpgid_session_leader(void) {
    pid_t pid = fork();
    if (pid == 0) {
        setsid();                 /* become a session leader */
        errno = 0;
        int rc = setpgid(0, 0);   /* a session leader cannot leave its group */
        emit("setpgid.sessleader", "%s", res_errno(rc));
        _exit(0);
    }
    proc_wait_blocking(pid, NULL, 0);
}

/* a stable pid that is essentially never live (above the default pid_max) */
#define NOPID 0x3ffffff0

static void case_getpgid_getsid_errors(void) {
    errno = 0;
    emit("getpgid.nopid", "%s", res_errno(getpgid(NOPID)));
    errno = 0;
    emit("getsid.nopid", "%s", res_errno(getsid(NOPID)));
    /* getpgid(0)/getsid(0) are consistent with the no-arg forms */
    emit_i("getpgid0_eq_getpgrp", getpgid(0) == getpgrp());
    emit_i("getsid0_eq_getsid_self", getsid(0) == getsid(getpid()));
}

static void case_setpgid_errors(void) {
    /* setpgid of a non-existent pid -> ESRCH */
    errno = 0;
    emit("setpgid.nopid", "%s", res_errno(setpgid(NOPID, 0)));
}

/* setpgid a child that has already execve()'d -> EACCES (man setpgid) */
static void case_setpgid_after_exec(char **argv) {
    int p[2];
    if (pipe(p) != 0) { emit("setpgid.afterexec", "%s", "PIPEFAIL"); return; }
    pid_t pid = fork();
    if (pid == 0) {
        close(p[0]);
        char fdbuf[16];
        snprintf(fdbuf, sizeof fdbuf, "%d", p[1]);
        char *const cargv[] = { argv[0], (char *) "--exec-sleeper", fdbuf, NULL };
        execv(argv[0], cargv);
        _exit(127);
    }
    close(p[1]);
    char b = 0;
    /* wait until the child has actually exec'd (it writes one byte post-exec) */
    ssize_t r;
    do { r = read(p[0], &b, 1); } while (r < 0 && errno == EINTR);
    close(p[0]);
    errno = 0;
    int rc = setpgid(pid, pid);   /* child is in our session; only the exec blocks it */
    emit("setpgid.afterexec", "%s", res_errno(rc));
    kill(pid, SIGKILL);
    proc_wait_blocking(pid, NULL, 0);
}

int main(int argc, char **argv) {
    if (argc >= 2 && strcmp(argv[1], "--exec-sleeper") == 0)
        return exec_sleeper(argc, argv);

    proc_init(argc, argv);

    case_setsid_basic();
    case_setsid_already_leader();
    case_setpgid_self_new_group();
    case_setpgid_negative_pgid();
    case_setpgid_session_leader();
    case_getpgid_getsid_errors();
    case_setpgid_errors();
    case_setpgid_after_exec(argv);
    return 0;
}
