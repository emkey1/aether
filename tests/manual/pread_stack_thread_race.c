// pread_stack_thread_race.c — stress repro attempt for a device crash: nvim's
// libuv worker thread (a CLONE_VM sibling of the main thread) SIGABRT'd inside
// jit_crash_fn() because a host EXC_BAD_ACCESS happened *outside* JIT
// execution — a memcpy in __user_write_task_mem (kernel/user.c), reached from
// sys_pread_guest (kernel/fs.c) copying pread'd bytes into a guest stack
// buffer. mem_ptr() (emu/memory.c) had already validated the page as mapped
// and writable and handed back a host pointer; the memcpy itself then faulted,
// meaning the host backing mem_ptr() trusted was stale (freed/moved) even
// though the guest-side page-table check passed.
//
// Working theory: this is a CLONE_VM multi-threaded process (matching nvim +
// its libuv thread pool sharing one struct mem). The suspected trigger is
// concurrent, unsynchronized *readers* of a still-growing/shrinking address
// space: one sibling thread doing pread()-into-its-own-stack (mem_ptr() write
// path, resolves COW / growsdown under mem->lock) while OTHER sibling threads
// concurrently (a) spawn/exit short-lived pthreads -- each pthread_create
// mmaps a fresh stack via the "pure growth" fast path (kernel/mmap.c
// mem_growth_lock, read-lock only) and each pthread exit/join munmaps it via
// the full evicting writer path (mem_write_lock_with_pokes) -- and (b) fork()
// children (pt_copy_on_write under the same writer path) that promptly exit,
// so COW pages the pread-ing thread's OWN stack was just marked with get
// resolved (mem_ptr's internal read_to_write_lock upgrade) right as siblings
// hammer the same mem's page table from the writer side.
//
// This is NOT a confirmed repro -- it exercises the suspected interleaving
// (growth-fast mmap + evicting munmap + fork/exec COW + pread-into-stack, all
// on one shared address space, sustained) so it can be run under ThreadSanitizer
// or with repeated iterations to try to surface a race that pure code reading
// couldn't confirm. A clean PASS does not clear the code of the bug; a crash
// or TSAN report here would confirm it and point at the exact racing accesses.
#define _GNU_SOURCE
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/wait.h>
#include <stdatomic.h>
#include "test_common.h"

#ifndef PREAD_RACE_SECONDS
#define PREAD_RACE_SECONDS 8
#endif
#define PREAD_WORKERS 6
#define CHURN_WORKERS 3
#define FORK_WORKERS 2
#define READ_CHUNK 4095 // matches the observed device crash's pread() size

static _Atomic int stop_flag;
static _Atomic long pread_iters, churn_iters, fork_iters;
static int src_fd = -1;

static void alarm_handler(int sig) {
    (void) sig;
    atomic_store(&stop_flag, 1);
}

// Mirrors the crash: pread() into a buffer that lives on THIS thread's own
// guest stack (a fresh mmap under CLONE_VM thread creation, potentially still
// COW/growsdown at the time of the very first writes).
static void *pread_worker(void *arg) {
    (void) arg;
    while (!atomic_load_explicit(&stop_flag, memory_order_relaxed)) {
        char stack_buf[READ_CHUNK];
        ssize_t n = pread(src_fd, stack_buf, sizeof(stack_buf), 0);
        if (n < 0) {
            perror("pread");
            _exit(3);
        }
        // Touch every page of the buffer (write, not just the memcpy target's
        // first page) so a growsdown/COW page deeper in this thread's stack
        // gets exercised too, not just whatever pread's user_write chunked.
        volatile char sink = 0;
        for (ssize_t i = 0; i < n; i += 4096)
            sink ^= stack_buf[i];
        (void) sink;
        atomic_fetch_add_explicit(&pread_iters, 1, memory_order_relaxed);
    }
    return NULL;
}

// Repeatedly spawns and joins a short-lived pthread: each create is a
// "pure growth" mmap of a fresh stack (kernel/mmap.c mmap_is_pure_growth,
// read-lock-only fast path); each join's stack teardown is a real munmap
// under the full evicting writer path (mem_write_lock_with_pokes).
static void *tiny_worker(void *arg) {
    (void) arg;
    return NULL;
}

static void *churn_worker(void *arg) {
    (void) arg;
    while (!atomic_load_explicit(&stop_flag, memory_order_relaxed)) {
        pthread_t t;
        if (pthread_create(&t, NULL, tiny_worker, NULL) == 0)
            pthread_join(t, NULL);
        atomic_fetch_add_explicit(&churn_iters, 1, memory_order_relaxed);
    }
    return NULL;
}

// fork()+exit() churn on the shared address space: pt_copy_on_write marks
// this thread-group's mapped pages (including every sibling thread's stack,
// e.g. the pread workers' stacks) P_COW under the full writer lock, so the
// pread workers' very next stack write has to resolve a fresh COW fault via
// mem_ptr()'s internal read_to_write_lock upgrade -- concurrently with the
// growth/evicting churn above mutating unrelated pages of the same mem.
static void *fork_worker(void *arg) {
    (void) arg;
    while (!atomic_load_explicit(&stop_flag, memory_order_relaxed)) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            _exit(4);
        }
        if (pid == 0)
            _exit(0);
        int status;
        if (waitpid(pid, &status, 0) != pid) {
            perror("waitpid");
            _exit(5);
        }
        atomic_fetch_add_explicit(&fork_iters, 1, memory_order_relaxed);
    }
    return NULL;
}

int main(int argc, char **argv) {
    test_init(argc, argv);

    char path[] = "/tmp/pread_stack_thread_race.XXXXXX";
    src_fd = mkstemp(path);
    if (src_fd < 0) {
        perror("mkstemp");
        return 2;
    }
    unlink(path);
    char fill[READ_CHUNK];
    memset(fill, 'A', sizeof(fill));
    if (write(src_fd, fill, sizeof(fill)) != (ssize_t) sizeof(fill)) {
        perror("write");
        return 2;
    }

    signal(SIGALRM, alarm_handler);
    alarm(PREAD_RACE_SECONDS);

    pthread_t pread_t[PREAD_WORKERS], churn_t[CHURN_WORKERS], fork_t[FORK_WORKERS];
    for (int i = 0; i < PREAD_WORKERS; i++) {
        if (pthread_create(&pread_t[i], NULL, pread_worker, NULL) != 0) {
            perror("pthread_create pread");
            return 2;
        }
    }
    for (int i = 0; i < CHURN_WORKERS; i++) {
        if (pthread_create(&churn_t[i], NULL, churn_worker, NULL) != 0) {
            perror("pthread_create churn");
            return 2;
        }
    }
    for (int i = 0; i < FORK_WORKERS; i++) {
        if (pthread_create(&fork_t[i], NULL, fork_worker, NULL) != 0) {
            perror("pthread_create fork");
            return 2;
        }
    }

    for (int i = 0; i < PREAD_WORKERS; i++)
        pthread_join(pread_t[i], NULL);
    for (int i = 0; i < CHURN_WORKERS; i++)
        pthread_join(churn_t[i], NULL);
    for (int i = 0; i < FORK_WORKERS; i++)
        pthread_join(fork_t[i], NULL);

    close(src_fd);

    test_logf("pread_iters=%ld churn_iters=%ld fork_iters=%ld\n",
               (long) pread_iters, (long) churn_iters, (long) fork_iters);

    // Surviving to here (no crash/hang/sanitizer report) is the actual
    // pass/fail signal for this stress test; the iteration counts just
    // confirm all three racers actually ran a meaningful number of times.
    if (pread_iters < 10 || fork_iters < 1) {
        printf("FAIL pread_stack_thread_race: too few iterations to be a meaningful stress run "
               "(pread=%ld fork=%ld)\n", (long) pread_iters, (long) fork_iters);
        failures_total++;
    } else {
        test_logf("ok pread_stack_thread_race iterations\n");
    }

    return finish_suite("pread_stack_thread_race");
}
