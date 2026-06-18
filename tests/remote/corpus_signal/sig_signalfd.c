/*
 * sig_signalfd.c — signalfd(2) draining order + siginfo fields + nonblock.
 *
 * signalfd consumes from the same pending queue as ordinary delivery, so it
 * must also hand back signals lowest-numbered-first (real-time spec). It also
 * reports SI_QUEUE/SI_USER, the sender pid and the queued value. Reads are of
 * blocked signals (the normal signalfd usage), so nothing runs a handler.
 */
#include "sig_common.h"
#include <sys/signalfd.h>

int main(int argc, char **argv) {
    sig_init(argc, argv);

    const int base = SIGRTMIN;
    sigset_t set, old;
    sigemptyset(&set);
    for (int i = 0; i < 3; i++)
        sigaddset(&set, base + i);
    sigaddset(&set, SIGUSR1);
    sigprocmask(SIG_BLOCK, &set, &old);

    int fd = signalfd(-1, &set, O_NONBLOCK);
    if (fd < 0) { emit("signalfd.create", "%s", "FAIL"); return 0; }

    /* empty -> EAGAIN */
    {
        struct signalfd_siginfo si;
        errno = 0;
        ssize_t n = read(fd, &si, sizeof si);
        emit("signalfd.empty.errno", "%s", n < 0 ? errno_name(errno) : "GOT");
    }

    /* queue three RT signals scrambled + give each a value; drain lowest-first */
    int order[3] = {2, 0, 1};
    for (int i = 0; i < 3; i++) {
        union sigval v; v.sival_int = 0x200 + order[i];
        sigqueue(getpid(), base + order[i], v);
    }
    for (int step = 0; step < 3; step++) {
        struct signalfd_siginfo si; memset(&si, 0, sizeof si);
        ssize_t n = read(fd, &si, sizeof si);
        char k[64];
        snprintf(k, sizeof k, "signalfd.rt.step%d.off", step);
        emit_i(k, n == (ssize_t) sizeof si ? (int) si.ssi_signo - base : -1);
        snprintf(k, sizeof k, "signalfd.rt.step%d.val", step);
        emit_i(k, n == (ssize_t) sizeof si ? si.ssi_int : -1);
        snprintf(k, sizeof k, "signalfd.rt.step%d.code", step);
        emit_i(k, n == (ssize_t) sizeof si ? si.ssi_code : 0);
    }

    /* a kill()'d standard signal -> SI_USER, sender pid */
    kill(getpid(), SIGUSR1);
    {
        struct signalfd_siginfo si; memset(&si, 0, sizeof si);
        ssize_t n = read(fd, &si, sizeof si);
        emit_i("signalfd.kill.signo", n > 0 ? (int) si.ssi_signo : -1);
        emit_i("signalfd.kill.code", n > 0 ? si.ssi_code : 0xbad);      /* SI_USER 0 */
        emit_i("signalfd.kill.pid_is_self", n > 0 ? (si.ssi_pid == (uint32_t) getpid()) : -1);
    }

    close(fd);
    sigprocmask(SIG_SETMASK, &old, NULL);
    return 0;
}
