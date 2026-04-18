#define _GNU_SOURCE

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

#include "test_common.h"

static volatile sig_atomic_t handler_count;
static volatile sig_atomic_t last_sig;

struct signal_args {
    pthread_t target_thread;
    int delay_ms;
    int sig;
};

struct waiter_result {
    int fd;
    int rc;
    int err;
    int aux;
    int ready;
};

static void sleep_ms(int ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long) (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static void signal_handler(int sig) {
    handler_count++;
    last_sig = sig;
}

static void reset_state(void) {
    handler_count = 0;
    last_sig = 0;
}

static void install_handler(int flags) {
    struct sigaction act;
    memset(&act, 0, sizeof(act));
    act.sa_handler = signal_handler;
    act.sa_flags = flags;
    sigemptyset(&act.sa_mask);
    if (sigaction(SIGUSR1, &act, NULL) != 0) {
        perror("sigaction");
        exit(1);
    }
}

static void *send_signal_after_delay(void *arg) {
    struct signal_args *args = arg;
    sleep_ms(args->delay_ms);
    if (pthread_kill(args->target_thread, args->sig) != 0) {
        perror("pthread_kill");
        exit(1);
    }
    return NULL;
}

static void wait_until_ready(struct waiter_result *result) {
    while (!__atomic_load_n(&result->ready, __ATOMIC_ACQUIRE))
        sched_yield();
    sleep_ms(20);
}

static void *poll_waiter_thread(void *arg) {
    struct waiter_result *result = arg;
    struct pollfd pfd = {
        .fd = result->fd,
        .events = POLLIN,
        .revents = 0,
    };

    __atomic_store_n(&result->ready, 1, __ATOMIC_RELEASE);
    errno = 0;
    result->rc = poll(&pfd, 1, 1000);
    result->err = errno;
    result->aux = pfd.revents;
    return NULL;
}

static void *select_waiter_thread(void *arg) {
    struct waiter_result *result = arg;
    fd_set rfds;
    struct timeval tv = {
        .tv_sec = 1,
        .tv_usec = 0,
    };

    FD_ZERO(&rfds);
    FD_SET(result->fd, &rfds);
    __atomic_store_n(&result->ready, 1, __ATOMIC_RELEASE);
    errno = 0;
    result->rc = select(result->fd + 1, &rfds, NULL, NULL, &tv);
    result->err = errno;
    result->aux = FD_ISSET(result->fd, &rfds);
    return NULL;
}

static void test_poll_waiter_thread_no_restart(void) {
    int pipefd[2];
    pthread_t waiter;
    pthread_t sender;
    struct waiter_result result = {
        .fd = -1,
        .rc = 0,
        .err = 0,
        .aux = 0,
        .ready = 0,
    };
    struct signal_args args;

    if (pipe(pipefd) != 0) {
        perror("pipe");
        exit(1);
    }

    reset_state();
    install_handler(0);
    result.fd = pipefd[0];
    if (pthread_create(&waiter, NULL, poll_waiter_thread, &result) != 0) {
        perror("pthread_create");
        exit(1);
    }
    wait_until_ready(&result);

    args = (struct signal_args) {
        .target_thread = waiter,
        .delay_ms = 20,
        .sig = SIGUSR1,
    };
    if (pthread_create(&sender, NULL, send_signal_after_delay, &args) != 0) {
        perror("pthread_create");
        exit(1);
    }

    pthread_join(sender, NULL);
    pthread_join(waiter, NULL);
    close(pipefd[0]);
    close(pipefd[1]);

    test_logf("signal poll waiter thread no-restart: rc=%d errno=%d revents=%#x count=%d sig=%d\n",
              result.rc, result.err, result.aux, handler_count, last_sig);

    if (result.rc != -1 || result.err != EINTR || handler_count != 1 || last_sig != SIGUSR1)
        failf("signal poll waiter thread no-restart", result.rc, result.err, handler_count,
              -1, EINTR, 1);
}

static void test_select_waiter_thread_no_restart(void) {
    int pipefd[2];
    pthread_t waiter;
    pthread_t sender;
    struct waiter_result result = {
        .fd = -1,
        .rc = 0,
        .err = 0,
        .aux = 0,
        .ready = 0,
    };
    struct signal_args args;

    if (pipe(pipefd) != 0) {
        perror("pipe");
        exit(1);
    }

    reset_state();
    install_handler(0);
    result.fd = pipefd[0];
    if (pthread_create(&waiter, NULL, select_waiter_thread, &result) != 0) {
        perror("pthread_create");
        exit(1);
    }
    wait_until_ready(&result);

    args = (struct signal_args) {
        .target_thread = waiter,
        .delay_ms = 20,
        .sig = SIGUSR1,
    };
    if (pthread_create(&sender, NULL, send_signal_after_delay, &args) != 0) {
        perror("pthread_create");
        exit(1);
    }

    pthread_join(sender, NULL);
    pthread_join(waiter, NULL);
    close(pipefd[0]);
    close(pipefd[1]);

    test_logf("signal select waiter thread no-restart: rc=%d errno=%d isset=%d count=%d sig=%d\n",
              result.rc, result.err, result.aux, handler_count, last_sig);

    if (result.rc != -1 || result.err != EINTR || handler_count != 1 || last_sig != SIGUSR1)
        failf("signal select waiter thread no-restart", result.rc, result.err, handler_count,
              -1, EINTR, 1);
}

static void test_pselect_mask_unblock(void) {
    int pipefd[2];
    pthread_t sender;
    struct signal_args args;
    sigset_t blocked;
    sigset_t empty;
    sigset_t oldset;
    fd_set rfds;
    struct timespec timeout = {
        .tv_sec = 0,
        .tv_nsec = 200 * 1000 * 1000L,
    };
    int rc;
    int err;

    if (pipe(pipefd) != 0) {
        perror("pipe");
        exit(1);
    }

    sigemptyset(&blocked);
    sigaddset(&blocked, SIGUSR1);
    if (pthread_sigmask(SIG_BLOCK, &blocked, &oldset) != 0) {
        perror("pthread_sigmask");
        exit(1);
    }
    sigemptyset(&empty);

    reset_state();
    install_handler(0);
    args = (struct signal_args) {
        .target_thread = pthread_self(),
        .delay_ms = 20,
        .sig = SIGUSR1,
    };
    if (pthread_create(&sender, NULL, send_signal_after_delay, &args) != 0) {
        perror("pthread_create");
        exit(1);
    }

    FD_ZERO(&rfds);
    FD_SET(pipefd[0], &rfds);
    errno = 0;
    rc = pselect(pipefd[0] + 1, &rfds, NULL, NULL, &timeout, &empty);
    err = errno;
    pthread_join(sender, NULL);
    pthread_sigmask(SIG_SETMASK, &oldset, NULL);
    close(pipefd[0]);
    close(pipefd[1]);

    test_logf("signal pselect mask-unblock: rc=%d errno=%d count=%d sig=%d\n",
              rc, err, handler_count, last_sig);

    if (rc != -1 || err != EINTR || handler_count != 1 || last_sig != SIGUSR1)
        failf("signal pselect mask-unblock", rc, err, handler_count, -1, EINTR, 1);
}

#if !defined(__APPLE__)
static void test_ppoll_mask_unblock(void) {
    int pipefd[2];
    pthread_t sender;
    struct signal_args args;
    sigset_t blocked;
    sigset_t empty;
    sigset_t oldset;
    struct pollfd pfd;
    struct timespec timeout = {
        .tv_sec = 0,
        .tv_nsec = 200 * 1000 * 1000L,
    };
    int rc;
    int err;

    if (pipe(pipefd) != 0) {
        perror("pipe");
        exit(1);
    }

    sigemptyset(&blocked);
    sigaddset(&blocked, SIGUSR1);
    if (pthread_sigmask(SIG_BLOCK, &blocked, &oldset) != 0) {
        perror("pthread_sigmask");
        exit(1);
    }
    sigemptyset(&empty);

    reset_state();
    install_handler(0);
    args = (struct signal_args) {
        .target_thread = pthread_self(),
        .delay_ms = 20,
        .sig = SIGUSR1,
    };
    if (pthread_create(&sender, NULL, send_signal_after_delay, &args) != 0) {
        perror("pthread_create");
        exit(1);
    }

    pfd = (struct pollfd) {
        .fd = pipefd[0],
        .events = POLLIN,
        .revents = 0,
    };
    errno = 0;
    rc = ppoll(&pfd, 1, &timeout, &empty);
    err = errno;
    pthread_join(sender, NULL);
    pthread_sigmask(SIG_SETMASK, &oldset, NULL);
    close(pipefd[0]);
    close(pipefd[1]);

    test_logf("signal ppoll mask-unblock: rc=%d errno=%d revents=%#x count=%d sig=%d\n",
              rc, err, pfd.revents, handler_count, last_sig);

    if (rc != -1 || err != EINTR || handler_count != 1 || last_sig != SIGUSR1)
        failf("signal ppoll mask-unblock", rc, err, handler_count, -1, EINTR, 1);
}
#endif

int main(int argc, char **argv) {
    test_init(argc, argv);
    test_poll_waiter_thread_no_restart();
    test_select_waiter_thread_no_restart();
    test_pselect_mask_unblock();
#if !defined(__APPLE__)
    test_ppoll_mask_unblock();
#endif
    return finish_suite("signal_poll");
}
