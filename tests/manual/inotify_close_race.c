// Regression test for a use-after-free in kernel/inotify.c's global instance
// registry.
//
// inotify_notify_*() (called from write()/chmod()/chown()/unlink()/etc. on
// any path, regardless of whether it's watched) walks a global list of every
// live inotify fd via inotify_snapshot_instances(), which retains each one
// with fd_retain() before dropping inotify_instances_lock and touching its
// struct inotify_state.
//
// fd_retain() unconditionally does fd->refcount++. But the decision that an
// fd has hit its LAST reference and must be torn down -- generic fd_close()'s
// `if (--fd->refcount == 0)` -- lives in fs/fd.c and knows nothing about
// inotify_instances_lock. So a concurrent close() of an inotify fd can win
// the race: it decrements refcount to 0 and commits to calling
// inotify_close() (which frees the struct inotify_state and, once fd_close()
// finishes, the struct fd itself) at the exact moment inotify_snapshot_instances
// is still walking inotify_instances and sees this fd's state still linked
// (list_remove_safe() in inotify_close() hasn't run yet). Its fd_retain()
// then resurrects a dying object: refcount goes 0 -> 1, but the closing
// thread has already unconditionally committed to freeing it. Whoever gets
// there first wins; the retaining thread ends up dereferencing (or locking
// the embedded mutex of) already-freed memory.
//
// Under stress-ng's `--inotify --chmod --chown` combination (very frequent
// inotify_init1()+close() cycles racing very frequent inotify_notify_attrib()
// calls from unrelated chmod/chown syscalls) this reproduced within 15-90s as
// a SIGSEGV/malloc-corruption abort deep inside inotify_find_watch(), often
// on a completely unrelated later allocation once the freed memory got
// reused (heap corruption, not a clean null-deref).
//
// Fixed by fd_retain_if_live() (fs/fd.c/fs/fd.h): a CAS-based retain that
// fails instead of resurrecting an fd whose refcount has already reached 0,
// used by inotify_snapshot_instances() in place of plain fd_retain().
//
// Like clone_error_cleanup.c and fakefs_type_race.c, this bug can only be
// observed by crashing (or not); there's no in-process way to detect a UAF
// short of that. Success is simply: survive sustained racing for the full
// duration and exit cleanly.
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "test_common.h"

#define CHURN_THREADS 4
#define TRIGGER_THREADS 4
#define DURATION_SECS 5

static char g_path[512];
static atomic_int g_stop;
static atomic_ullong g_churn_cycles;
static atomic_ullong g_trigger_cycles;

// Repeatedly opens an inotify fd, watches g_path, and closes it -- the
// tight open/close cycle that races inotify_close()'s teardown against
// concurrent inotify_notify_*() walks from the trigger threads below.
static void *churn_thread(void *arg) {
    (void) arg;
    while (!atomic_load_explicit(&g_stop, memory_order_relaxed)) {
        int fd = inotify_init1(IN_NONBLOCK);
        if (fd < 0)
            continue;
        inotify_add_watch(fd, g_path, IN_ATTRIB | IN_MODIFY | IN_CLOSE_WRITE);
        close(fd);
        atomic_fetch_add_explicit(&g_churn_cycles, 1, memory_order_relaxed);
    }
    return NULL;
}

// Fires the two notify code paths that were reachable from real crashes:
// inotify_notify_attrib() (via chmod/chown -> generic_setattrat) and
// inotify_notify_modify() (via write()). Every call walks the FULL global
// instance list, regardless of whether g_path happens to have a live watch
// at that instant -- that's what makes this hit the churn threads' fds too.
static void *trigger_thread(void *arg) {
    (void) arg;
    int mode_bit = 0;
    while (!atomic_load_explicit(&g_stop, memory_order_relaxed)) {
        chmod(g_path, (mode_bit++ & 1) ? 0644 : 0600);
        int fd = open(g_path, O_WRONLY);
        if (fd >= 0) {
            char byte = 'x';
            ssize_t res = write(fd, &byte, 1);
            (void) res;
            close(fd);
        }
        atomic_fetch_add_explicit(&g_trigger_cycles, 1, memory_order_relaxed);
    }
    return NULL;
}

static void on_alarm(int sig) {
    (void) sig;
    const char msg[] = "inotify_close_race: FAIL watchdog timeout (possible deadlock)\n";
    ssize_t res = write(2, msg, sizeof(msg) - 1);
    (void) res;
    _exit(1);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    signal(SIGALRM, on_alarm);
    alarm(test_watchdog_secs(60));

    snprintf(g_path, sizeof(g_path), "%s/inotify_close_race.%d",
             getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", (int) getpid());
    int seed_fd = open(g_path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (seed_fd < 0) {
        perror("open");
        return 2;
    }
    close(seed_fd);

    pthread_t churn[CHURN_THREADS], trigger[TRIGGER_THREADS];
    atomic_store(&g_stop, 0);
    for (int i = 0; i < CHURN_THREADS; i++) {
        if (pthread_create(&churn[i], NULL, churn_thread, NULL) != 0) {
            perror("pthread_create churn");
            return 2;
        }
    }
    for (int i = 0; i < TRIGGER_THREADS; i++) {
        if (pthread_create(&trigger[i], NULL, trigger_thread, NULL) != 0) {
            perror("pthread_create trigger");
            return 2;
        }
    }

    unsigned secs = test_watchdog_secs(DURATION_SECS) < DURATION_SECS
        ? DURATION_SECS : DURATION_SECS; // race duration itself isn't watchdog-scaled
    sleep(secs);

    atomic_store(&g_stop, 1);
    for (int i = 0; i < CHURN_THREADS; i++)
        pthread_join(churn[i], NULL);
    for (int i = 0; i < TRIGGER_THREADS; i++)
        pthread_join(trigger[i], NULL);
    unlink(g_path);
    alarm(0);

    unsigned long long churn_cycles = atomic_load(&g_churn_cycles);
    unsigned long long trigger_cycles = atomic_load(&g_trigger_cycles);
    test_logf("inotify_close_race: %llu open/watch/close cycles, "
              "%llu notify-trigger cycles in %us\n",
              churn_cycles, trigger_cycles, secs);

    // Reaching here (instead of crashing) means the process survived
    // sustained racing between inotify teardown and the global notify walk.
    if (churn_cycles == 0 || trigger_cycles == 0) {
        printf("FAIL inotify_close_race: one side of the race never ran"
               " (churn=%llu trigger=%llu)\n", churn_cycles, trigger_cycles);
        failures_total++;
    }
    return finish_suite("inotify_close_race");
}
