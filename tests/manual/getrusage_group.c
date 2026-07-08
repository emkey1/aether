// Covers: getrusage(RUSAGE_SELF) and clock_gettime(CLOCK_PROCESS_CPUTIME_ID)
// must sum usage across every thread in the process, not just report the
// calling thread's own usage (issue #423 Tier 3). A worker thread burns CPU
// while the main thread stays idle; RUSAGE_SELF must reflect the worker's
// work even though the calling (main) thread did essentially none.
#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <sys/resource.h>
#include <time.h>
#include "test_common.h"

static double timeval_secs(struct timeval tv) {
    return tv.tv_sec + tv.tv_usec / 1e6;
}

static double rusage_self_secs(int who) {
    struct rusage r;
    getrusage(who, &r);
    return timeval_secs(r.ru_utime) + timeval_secs(r.ru_stime);
}

static void burn_cpu(double secs) {
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile long x = 0;
    do {
        for (int i = 0; i < 1000000; i++)
            x += i;
        clock_gettime(CLOCK_MONOTONIC, &now);
    } while ((now.tv_sec - start.tv_sec) + (now.tv_nsec - start.tv_nsec) / 1e9 < secs);
}

static void *worker(void *arg) {
    (void) arg;
    burn_cpu(0.3);
    return NULL;
}

static void test_rusage_self_sums_threads(void) {
    double before_self = rusage_self_secs(RUSAGE_SELF);
    double before_thread = rusage_self_secs(RUSAGE_THREAD);

    pthread_t thread;
    if (pthread_create(&thread, NULL, worker, NULL) != 0) {
        printf("FAIL rusage_self_sums_threads: pthread_create failed\n");
        failures_total++;
        return;
    }
    pthread_join(thread, NULL);

    double after_self = rusage_self_secs(RUSAGE_SELF);
    double after_thread = rusage_self_secs(RUSAGE_THREAD);
    double self_delta = after_self - before_self;
    double thread_delta = after_thread - before_thread;

    test_logf("rusage_self_sums_threads: self_delta=%f thread_delta=%f\n", self_delta, thread_delta);

    // The main thread did essentially no work of its own; if RUSAGE_SELF only
    // reflected the caller (the pre-fix bug), self_delta would track
    // thread_delta closely instead of the worker's ~0.3s contribution.
    if (self_delta < 0.15) {
        printf("FAIL rusage_self_sums_threads: RUSAGE_SELF delta %f too small to include worker thread\n", self_delta);
        failures_total++;
    } else if (self_delta - thread_delta < 0.1) {
        printf("FAIL rusage_self_sums_threads: RUSAGE_SELF (%f) barely exceeds RUSAGE_THREAD (%f) -- looks like only the caller was counted\n",
               self_delta, thread_delta);
        failures_total++;
    } else {
        test_logf("rusage_self_sums_threads: PASS\n");
    }
}

static void test_process_cputime_matches_rusage_self(void) {
    struct timespec ts;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    double cputime = ts.tv_sec + ts.tv_nsec / 1e9;
    double self = rusage_self_secs(RUSAGE_SELF);

    test_logf("process_cputime_matches_rusage_self: cputime=%f self=%f\n", cputime, self);

    // CLOCK_PROCESS_CPUTIME_ID is total (user+system) across the whole
    // process; it should track RUSAGE_SELF's user+system total closely.
    double diff = cputime > self ? cputime - self : self - cputime;
    if (diff > 0.1 || cputime < self * 0.5) {
        printf("FAIL process_cputime_matches_rusage_self: cputime=%f diverges from RUSAGE_SELF=%f\n", cputime, self);
        failures_total++;
    } else {
        test_logf("process_cputime_matches_rusage_self: PASS\n");
    }
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    test_rusage_self_sums_threads();
    test_process_cputime_matches_rusage_self();
    return finish_suite("getrusage_group");
}
