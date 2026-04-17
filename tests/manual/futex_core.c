#define _GNU_SOURCE

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "test_common.h"

#ifndef FUTEX_WAIT
#define FUTEX_WAIT 0
#endif

#ifndef FUTEX_WAKE
#define FUTEX_WAKE 1
#endif

#ifndef FUTEX_PRIVATE_FLAG
#define FUTEX_PRIVATE_FLAG 128
#endif

static volatile uint32_t futex_word;
static volatile sig_atomic_t handler_count;
static volatile sig_atomic_t last_sig;

struct wait_result {
    int rc;
    int err;
    int timeout_ms;
};

struct wake_args {
    int delay_ms;
    int delay_wake_ms;
    int send_signal;
    int wake_count;
    uint32_t set_value;
    pid_t target_pid;
    pid_t target_tid;
};

static void sleep_ms(int ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long) (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static int futex_wait_raw(volatile uint32_t *uaddr, uint32_t expected, const struct timespec *timeout) {
    return (int) syscall(SYS_futex, (uint32_t *) uaddr, FUTEX_WAIT | FUTEX_PRIVATE_FLAG,
                         expected, timeout, NULL, 0);
}

static int futex_wake_raw(volatile uint32_t *uaddr, int count) {
    return (int) syscall(SYS_futex, (uint32_t *) uaddr, FUTEX_WAKE | FUTEX_PRIVATE_FLAG,
                         count, NULL, NULL, 0);
}

static void reset_handler_state(void) {
    handler_count = 0;
    last_sig = 0;
}

static void signal_handler(int sig) {
    handler_count++;
    last_sig = sig;
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

static void *wake_helper(void *arg) {
    struct wake_args *args = arg;
    sleep_ms(args->delay_ms);
    if (args->send_signal) {
        if (syscall(SYS_tgkill, args->target_pid, args->target_tid, SIGUSR1) != 0) {
            perror("tgkill");
            exit(1);
        }
    }
    if (args->wake_count > 0) {
        sleep_ms(args->delay_wake_ms);
        futex_word = args->set_value;
        futex_wake_raw(&futex_word, args->wake_count);
    }
    return NULL;
}

static void *waiter_thread(void *arg) {
    struct wait_result *result = arg;
    struct timespec timeout;
    timeout.tv_sec = result->timeout_ms / 1000;
    timeout.tv_nsec = (long) (result->timeout_ms % 1000) * 1000000L;
    errno = 0;
    result->rc = futex_wait_raw(&futex_word, 0, &timeout);
    result->err = errno;
    return NULL;
}

static void test_futex_wait_eagain(void) {
    struct timespec timeout = {.tv_sec = 0, .tv_nsec = 1000000};
    int rc;
    int err;

    futex_word = 1;
    errno = 0;
    rc = futex_wait_raw(&futex_word, 0, &timeout);
    err = errno;

    test_logf("futex wait eagain: rc=%d errno=%d word=%u\n", rc, err, futex_word);

    if (rc != -1 || err != EAGAIN)
        failf("futex wait eagain", rc, err, futex_word, -1, EAGAIN, 1);
}

static void test_futex_wait_timeout(void) {
    struct timespec timeout = {.tv_sec = 0, .tv_nsec = 50000000};
    int rc;
    int err;

    futex_word = 0;
    errno = 0;
    rc = futex_wait_raw(&futex_word, 0, &timeout);
    err = errno;

    test_logf("futex wait timeout: rc=%d errno=%d word=%u\n", rc, err, futex_word);

    if (rc != -1 || err != ETIMEDOUT)
        failf("futex wait timeout", rc, err, futex_word, -1, ETIMEDOUT, 0);
}

static void test_futex_wait_wake_single(void) {
    pthread_t thread;
    struct wait_result result = {.timeout_ms = 1000};
    int woke;
    int failed;

    futex_word = 0;
    if (pthread_create(&thread, NULL, waiter_thread, &result) != 0) {
        perror("pthread_create");
        exit(1);
    }
    sleep_ms(20);
    futex_word = 1;
    errno = 0;
    woke = futex_wake_raw(&futex_word, 1);
    pthread_join(thread, NULL);

    failed = woke != 1 || result.rc != 0 || result.err != 0 || futex_word != 1;
    test_log_if(failed, "futex wait/wake single: wake=%d rc=%d errno=%d word=%u\n",
                woke, result.rc, result.err, futex_word);

    if (woke != 1 || result.rc != 0 || result.err != 0)
        failf("futex wait wake single", woke, result.rc, result.err, 1, 0, 0);
}

static void test_futex_wake_counts(void) {
    pthread_t threads[2];
    struct wait_result results[2] = {
        {.timeout_ms = 1000},
        {.timeout_ms = 1000},
    };
    int wake1;
    int wake2;
    int done_after_wake1 = 0;
    int failed;

    futex_word = 0;
    for (int i = 0; i < 2; i++) {
        if (pthread_create(&threads[i], NULL, waiter_thread, &results[i]) != 0) {
            perror("pthread_create");
            exit(1);
        }
    }
    sleep_ms(20);
    futex_word = 1;
    wake1 = futex_wake_raw(&futex_word, 1);
    sleep_ms(20);
    for (int i = 0; i < 2; i++)
        done_after_wake1 += results[i].rc == 0;
    wake2 = futex_wake_raw(&futex_word, 1);
    for (int i = 0; i < 2; i++)
        pthread_join(threads[i], NULL);

    failed = wake1 != 1 || wake2 != 1 || done_after_wake1 != 1 ||
             results[0].rc != 0 || results[1].rc != 0 ||
             results[0].err != 0 || results[1].err != 0;
    test_log_if(failed, "futex wake counts: wake1=%d wake2=%d done_after_wake1=%d rc0=%d err0=%d rc1=%d err1=%d\n",
                wake1, wake2, done_after_wake1,
                results[0].rc, results[0].err, results[1].rc, results[1].err);

    if (wake1 != 1 || wake2 != 1 || done_after_wake1 != 1)
        failf("futex wake counts", wake1, wake2, done_after_wake1, 1, 1, 1);
}

static void test_futex_signal_no_restart(void) {
    pthread_t thread;
    struct timespec timeout = {.tv_sec = 0, .tv_nsec = 200000000};
    struct wake_args args = {
        .delay_ms = 20,
        .delay_wake_ms = 0,
        .send_signal = 1,
        .wake_count = 0,
        .set_value = 0,
        .target_pid = getpid(),
        .target_tid = (pid_t) syscall(SYS_gettid),
    };
    int rc;
    int err;

    reset_handler_state();
    install_handler(0);
    futex_word = 0;
    if (pthread_create(&thread, NULL, wake_helper, &args) != 0) {
        perror("pthread_create");
        exit(1);
    }

    errno = 0;
    rc = futex_wait_raw(&futex_word, 0, &timeout);
    err = errno;
    pthread_join(thread, NULL);

    test_logf("futex signal no-restart: rc=%d errno=%d count=%d sig=%d\n",
              rc, err, handler_count, last_sig);

    if (rc != -1 || err != EINTR || handler_count != 1 || last_sig != SIGUSR1)
        failf("futex signal no-restart", rc, err, handler_count, -1, EINTR, 1);
}

static void test_futex_signal_restart(void) {
    pthread_t thread;
    struct timespec timeout = {.tv_sec = 1, .tv_nsec = 0};
    struct wake_args args = {
        .delay_ms = 20,
        .delay_wake_ms = 20,
        .send_signal = 1,
        .wake_count = 1,
        .set_value = 1,
        .target_pid = getpid(),
        .target_tid = (pid_t) syscall(SYS_gettid),
    };
    int rc;
    int err;

    reset_handler_state();
    install_handler(SA_RESTART);
    futex_word = 0;
    if (pthread_create(&thread, NULL, wake_helper, &args) != 0) {
        perror("pthread_create");
        exit(1);
    }

    errno = 0;
    rc = futex_wait_raw(&futex_word, 0, &timeout);
    err = errno;
    pthread_join(thread, NULL);

    test_logf("futex signal restart: rc=%d errno=%d count=%d sig=%d word=%u\n",
              rc, err, handler_count, last_sig, futex_word);

    if (rc != 0 || handler_count != 1 || last_sig != SIGUSR1 || futex_word != 1)
        failf("futex signal restart", rc, err, handler_count, 0, 0, 1);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    test_futex_wait_eagain();
    test_futex_wait_timeout();
    test_futex_wait_wake_single();
    test_futex_wake_counts();
    test_futex_signal_no_restart();
    test_futex_signal_restart();
    return finish_suite("futex_core");
}
