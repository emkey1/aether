#include <errno.h>
#include <limits.h>
#include "kernel/task.h"
#include "util/sync.h"
#include "debug.h"
#include "kernel/errno.h"
#include <string.h>

int noprintk = 0; // Used to suppress calls to printk.
extern bool doEnableExtraLocking;
extern pthread_mutex_t wait_for_lock; // Synchroniztion lock

static int wait_for_internal(cond_t *cond, lock_t *lock, struct timespec *timeout, bool interruptible);
#if __linux__
static struct timespec timespec_add_local(struct timespec x, struct timespec y) {
    x.tv_sec += y.tv_sec;
    x.tv_nsec += y.tv_nsec;
    if (x.tv_nsec >= 1000000000) {
        x.tv_nsec -= 1000000000;
        x.tv_sec++;
    }
    return x;
}
#endif

static int cond_wait_with_optional_timeout(cond_t *cond, lock_t *lock, struct timespec *timeout) {
    if (timeout == NULL) {
        lock->wait4 = true;
        return pthread_cond_wait(&cond->cond, &lock->m);
    }

#if __linux__
    struct timespec abs_timeout;
    clock_gettime(CLOCK_MONOTONIC, &abs_timeout);
    abs_timeout = timespec_add_local(abs_timeout, *timeout);
    return pthread_cond_timedwait(&cond->cond, &lock->m, &abs_timeout);
#elif __APPLE__
    return pthread_cond_timedwait_relative_np(&cond->cond, &lock->m, timeout);
#else
#error Unimplemented pthread_cond_wait relative timeout.
#endif
}

void cond_init(cond_t *cond) {
    pthread_condattr_t cond_attr;
    pthread_condattr_init(&cond_attr);
#if __linux__
    pthread_condattr_setclock(&cond_attr, CLOCK_MONOTONIC);
#endif
    pthread_cond_init(&cond->cond, &cond_attr);
    pthread_condattr_destroy(&cond_attr); // Clean up the condition variable attribute

    // Initialize the mutex without specific attributes
    pthread_mutex_init(&cond->reference.lock, NULL);

    cond->reference.count = 0;
}

void cond_destroy(cond_t *cond) {
    pthread_cond_destroy(&cond->cond);
}

static bool is_signal_pending(lock_t *lock) {
    if (!current)
        return false;
    sigset_t_ pending = __atomic_load_n(&current->pending, __ATOMIC_ACQUIRE);
    sigset_t_ blocked = __atomic_load_n(&current->blocked, __ATOMIC_ACQUIRE);
    if ((pending & ~blocked) == 0)
        return false;
    if (lock != &current->sighand->lock)
        lock(&current->sighand->lock, 0);
    bool has_pending = !!(current->pending & ~current->blocked);
    if (lock != &current->sighand->lock)
        unlock(&current->sighand->lock);
    return has_pending;
}

static bool consume_wait_interrupted(void) {
    if (!current)
        return false;
    return __atomic_exchange_n(&current->wait_interrupted, false, __ATOMIC_ACQ_REL);
}

int wait_for(cond_t *cond, lock_t *lock, struct timespec *timeout) {
    if (consume_wait_interrupted() || is_signal_pending(lock))
        return _EINTR;
    int err = wait_for_internal(cond, lock, timeout, true);
    if (consume_wait_interrupted() || is_signal_pending(lock))
        return _EINTR;
    if (err < 0)
        return _ETIMEDOUT;
    return 0;
}

static int wait_for_internal(cond_t *cond, lock_t *lock, struct timespec *timeout, bool interruptible) {
    if (current) {
        lock(&current->waiting_cond_lock, 0);
        current->waiting_cond = cond;
        current->waiting_lock = lock;
        unlock(&current->waiting_cond_lock);
    }
    int rc = 0;
    char saveme[16];
    strncpy(saveme, lock->lname, 16); // Save for later
#if LOCK_DEBUG
    struct lock_debug lock_tmp = lock->debug;
    lock->debug = (struct lock_debug) { .initialized = lock->debug.initialized };
#endif

    if (interruptible && (consume_wait_interrupted() || is_signal_pending(lock)))
        goto out;
    bool old_should_mark_wait_interrupted = should_mark_wait_interrupted;
    if (interruptible)
        should_mark_wait_interrupted = true;
    rc = cond_wait_with_optional_timeout(cond, lock, timeout);
    should_mark_wait_interrupted = old_should_mark_wait_interrupted;
#if LOCK_DEBUG
out:
    lock->debug = lock_tmp;
#else
out:
#endif

    if(current) {
        lock(&current->waiting_cond_lock, 0);
        current->waiting_cond = NULL;
        current->waiting_lock = NULL;
        current->waiting_interrupt_flag = NULL;
        unlock(&current->waiting_cond_lock);
    }
    lock->wait4 = false;
    if(rc == ETIMEDOUT)
        return _ETIMEDOUT;
    return 0;
}

int wait_for_ignore_signals(cond_t *cond, lock_t *lock, struct timespec *timeout) {
    return wait_for_internal(cond, lock, timeout, false);
}

void notify(cond_t *cond) {
    pthread_cond_broadcast(&cond->cond);
}
void notify_once(cond_t *cond) {
    pthread_cond_signal(&cond->cond);
}

__thread sigjmp_buf unwind_buf;
__thread bool should_unwind = false;
__thread bool should_mark_wait_interrupted = false;

void sigusr1_handler(int UNUSED(sig)) {
    if (should_mark_wait_interrupted && current != NULL)
        __atomic_store_n(&current->wait_interrupted, true, __ATOMIC_RELEASE);
    if (should_unwind) {
        should_unwind = false;
        siglongjmp(unwind_buf, 1);
    }
}

// Force this thread's thread-local storage for everything sigusr1_handler
// touches to be instantiated *now*, on a normal call stack where malloc is
// safe. On Darwin the first access to a __thread variable is resolved lazily by
// _tlv_get_addr, which malloc()s the per-thread TLV block. If SIGUSR1 is
// delivered before that has happened, sigusr1_handler's own __thread access
// re-enters malloc from async-signal context; if the interrupted code already
// holds the (non-recursive) malloc lock, the process aborts in
// _os_unfair_lock_recursive_abort. Every thread that unblocks SIGUSR1 must call
// this first. Taking each variable's address forces _tlv_get_addr; the volatile
// loads keep the compiler from eliding the accesses.
void signal_thread_locals_init(void) {
    volatile struct task *const *cur = (volatile struct task *const *) &current;
    volatile bool *unwind = &should_unwind;
    volatile bool *mark = &should_mark_wait_interrupted;
    volatile char *buf = (volatile char *) &unwind_buf;
    (void) *cur;
    (void) *unwind;
    (void) *mark;
    (void) *buf;
}

// This is how you would mitigate the unlock/wait race if the wait
// is async signal safe. wait_for *should* be safe from this race
// because of synchronization involving the waiting_cond_lock.
#if 0
    sigset_t sigusr1;
    sigemptyset(&sigusr1);
    sigaddset(&sigusr1, SIGUSR1);

    if (current) {
        if (sigsetjmp(unwind_buf, 1)) {
            return _EINTR;
        }
        should_unwind = true;
        sigprocmask(SIG_BLOCK, &sigusr1, NULL);
        if (lock != &current->sighand->lock)
            lock(&current->sighand->lock, 0);
        bool pending = !!(current->pending & ~current->blocked);
        if (lock != &current->sighand->lock)
            unlock(&current->sighand->lock);
        sigprocmask(SIG_UNBLOCK, &sigusr1, NULL);
        if (pending) {
            should_unwind = false;
            return _EINTR;
        }
    }
#endif
