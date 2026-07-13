// signalfd_epoll_deadlock: an exiting child delivering SIGCHLD to its parent
// (send_signal_with_sighand -> deliver_signal_unlocked_locked ->
// signalfd_wakeup_task -> poll_wakeup) took fd->poll_lock then poll->lock
// while the target's sighand->lock was already held. A sibling thread of the
// same process (sighand is shared per tgroup) concurrently inside
// epoll_wait, scanning the same signalfd (poll_wait -> poll_scan_ready_locked
// -> signalfd_poll), holds poll->lock and blocks taking sighand->lock. Those
// are opposite orders on the same two locks -- sighand->lock->poll->lock in
// the delivery path, poll->lock->sighand->lock in the scan path -- so the two
// threads can AB-BA deadlock. Regression for fs/poll.c poll_wakeup (see
// poll_wakeup_trylock) and kernel/signal.c signalfd_wakeup_task.
//
// Found via an on-device backtrace during a labwc Wayland session: an
// exiting "list-apps.sh" child wedged permanently against labwc's own
// epoll_wait thread this way, with a third thread piling up behind the same
// contended sighand->lock in a concurrent futex wait.
//
// One set of threads hammers epoll_wait on an epoll set containing a SIGCHLD
// signalfd (padded with extra never-ready fds to widen the time poll->lock is
// held per scan, raising the odds of catching the race); another set hammers
// fork()+_exit() -- each child's exit delivers SIGCHLD to this process, whose
// threads share one sighand. A correct kernel always finishes; a kernel with
// the bug wedges permanently and is caught by the alarm() watchdog below.

#define _GNU_SOURCE

#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "test_common.h"

#define RUN_SECONDS 6
#define NUM_WAITERS 2
#define NUM_FORKERS 4
#define NUM_PAD_FDS 96

static volatile sig_atomic_t g_stop;
static atomic_uint g_wakes;
static atomic_uint g_forks;
static int g_epfd = -1;
static int g_sigfd = -1;
static int g_pad_fds[NUM_PAD_FDS][2];

static void on_alarm(int sig) {
    (void) sig;
    static const char msg[] =
        "signalfd_epoll_deadlock: FAIL timeout (sighand/poll AB-BA deadlock)\n";
    write(2, msg, sizeof(msg) - 1);
    _exit(1);
}

static void *waiter_thread(void *arg) {
    (void) arg;
    while (!g_stop) {
        struct epoll_event ev[8];
        int n = epoll_wait(g_epfd, ev, 8, 1);
        if (n <= 0)
            continue;
        for (int i = 0; i < n; i++) {
            if (ev[i].data.fd != g_sigfd)
                continue;
            struct signalfd_siginfo si;
            ssize_t r;
            while ((r = read(g_sigfd, &si, sizeof si)) == (ssize_t) sizeof si)
                atomic_fetch_add(&g_wakes, 1);
        }
    }
    return NULL;
}

static void *forker_thread(void *arg) {
    (void) arg;
    while (!g_stop) {
        pid_t pid = fork();
        if (pid == 0)
            _exit(0);
        if (pid > 0) {
            atomic_fetch_add(&g_forks, 1);
            waitpid(pid, NULL, 0);
            // fork()'s child->parent is the calling task, not the process
            // leader, so this child's SIGCHLD lands in *this thread's* own
            // task->queue -- drain it here so at least one path proves the
            // full signalfd round-trip works, independent of the waiter
            // threads (below), whose only job is to keep signalfd_poll
            // contending for sighand->lock while holding poll->lock.
            struct epoll_event ev[4];
            int n = epoll_wait(g_epfd, ev, 4, 50);
            for (int i = 0; i < n; i++) {
                if (ev[i].data.fd != g_sigfd)
                    continue;
                struct signalfd_siginfo si;
                ssize_t r;
                while ((r = read(g_sigfd, &si, sizeof si)) == (ssize_t) sizeof si)
                    atomic_fetch_add(&g_wakes, 1);
            }
        }
    }
    return NULL;
}

static void test_deadlock(void) {
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
    if (sigprocmask(SIG_BLOCK, &mask, NULL) != 0) {
        failf("signalfd_epoll_deadlock sigprocmask", 0, 0, 0, 0, 0, 0);
        return;
    }

    g_sigfd = signalfd(-1, &mask, SFD_NONBLOCK);
    if (g_sigfd < 0) {
        failf("signalfd_epoll_deadlock signalfd", 0, 0, 0, 0, 0, 0);
        return;
    }

    g_epfd = epoll_create1(0);
    if (g_epfd < 0) {
        failf("signalfd_epoll_deadlock epoll_create1", 0, 0, 0, 0, 0, 0);
        return;
    }

    // Pad the epoll set with fds that never become ready, so each
    // poll_scan_ready_locked() pass holds poll->lock for longer, widening the
    // window for a concurrent signal delivery to collide with it.
    for (int i = 0; i < NUM_PAD_FDS; i++) {
        if (pipe(g_pad_fds[i]) != 0) {
            failf("signalfd_epoll_deadlock pipe", (uint64_t) i, 0, 0, 0, 0, 0);
            return;
        }
        struct epoll_event pev = {0};
        pev.events = EPOLLIN;
        pev.data.fd = g_pad_fds[i][0];
        epoll_ctl(g_epfd, EPOLL_CTL_ADD, g_pad_fds[i][0], &pev);
    }

    struct epoll_event ev = {0};
    ev.events = EPOLLIN;
    ev.data.fd = g_sigfd;
    if (epoll_ctl(g_epfd, EPOLL_CTL_ADD, g_sigfd, &ev) != 0) {
        failf("signalfd_epoll_deadlock epoll_ctl_add", 0, 0, 0, 0, 0, 0);
        return;
    }

    pthread_t waiters[NUM_WAITERS], forkers[NUM_FORKERS];
    for (int i = 0; i < NUM_WAITERS; i++)
        pthread_create(&waiters[i], NULL, waiter_thread, NULL);
    for (int i = 0; i < NUM_FORKERS; i++)
        pthread_create(&forkers[i], NULL, forker_thread, NULL);

    sleep(RUN_SECONDS);
    g_stop = 1;

    for (int i = 0; i < NUM_WAITERS; i++)
        pthread_join(waiters[i], NULL);
    for (int i = 0; i < NUM_FORKERS; i++)
        pthread_join(forkers[i], NULL);

    unsigned forks = atomic_load(&g_forks);
    unsigned wakes = atomic_load(&g_wakes);
    test_logf("forks=%u wakes=%u\n", forks, wakes);
    if (forks == 0)
        failf("signalfd_epoll_deadlock no_forks", 0, 0, 0, 1, 0, 0);
    if (wakes == 0)
        failf("signalfd_epoll_deadlock no_wakes", 0, 0, 0, 1, 0, 0);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    signal(SIGALRM, on_alarm);
    alarm(RUN_SECONDS + 20);
    test_deadlock();
    return finish_suite("signalfd_epoll_deadlock");
}
