#ifndef UTIL_SYNC_H
#define UTIL_SYNC_H
#define JUSTLOG 1

#include <stdatomic.h>
#include <pthread.h>
#include <stdbool.h>
#include <setjmp.h>
#include<errno.h>
#include "misc.h"
#include "debug.h"

// locks, implemented using pthread

#define LOCK_DEBUG 0



extern int current_pid(void);
extern char* current_comm(void);

extern struct timespec lock_pause;

extern pthread_mutex_t nested_lock; // Used to make all lock operations atomic, even read->write and right->read -mke

typedef struct {
    pthread_mutex_t m;
    pthread_t owner;
#if LOCK_DEBUG
    struct lock_debug {
        const char *file; // doubles as locked
        int line;
        int pid;
        bool initialized;
    } debug;
#endif
} lock_t;


static inline void lock_init(lock_t *lock) {
    pthread_mutex_init(&lock->m, NULL);
#if LOCK_DEBUG
    lock->debug = (struct lock_debug) {
        .initialized = true,
    };
#endif
}

#if LOCK_DEBUG
#define LOCK_INITIALIZER {PTHREAD_MUTEX_INITIALIZER, 0, { .initialized = true }}
#else
#define LOCK_INITIALIZER {PTHREAD_MUTEX_INITIALIZER, 0}
#endif

static inline void __lock(lock_t *lock, __attribute__((unused)) const char *file, __attribute__((unused)) int line) {
    unsigned int count = 0;
    long count_max = (255000 - lock_pause.tv_nsec);  // As sleep time increases, decrease acceptable loops.  -mke
    while(pthread_mutex_trylock(&lock->m)) {
        count++;
        nanosleep(&lock_pause, NULL);
        if(count > count_max * .90) {
            printk("ERROR: Possible deadlock, aborted lock attempt(PID: %d Process: %s) from file %s\n",current_pid(), current_comm(), file);
            return;
        }
        // Loop until lock works.  Maybe this will help make the multithreading work? -mke
    }

    if(count > count_max * .90) {
        printk("WARNING: large lock attempt count(__lock(%d)) from file %s\n",count, file);
    }

    lock->owner = pthread_self();
#if LOCK_DEBUG
    assert(lock->debug.initialized);
    assert(!lock->debug.file && "Attempting to recursively lock");
    lock->debug.file = file;
    lock->debug.line = line;
    extern int current_pid(void);
    lock->debug.pid = current_pid();
#endif
}

#define lock(lock) __lock(lock, __FILE__, __LINE__)

static inline void unlock(lock_t *lock) {
#if LOCK_DEBUG
    assert(lock->debug.initialized);
    assert(lock->debug.file && "Attempting to unlock an unlocked lock");
    lock->debug = (struct lock_debug) { .initialized = true };
#endif
    lock->owner = zero_init(pthread_t);
    pthread_mutex_unlock(&lock->m);
}

typedef struct {
    pthread_rwlock_t l;
    // 0: unlocked
    // -1: write-locked
    // >0: read-locked with this many readers
    atomic_int val;
    const char *file;
    int line;
    int pid;
} wrlock_t;


static inline void nested_lockf(unsigned count);
static inline void _read_unlock(wrlock_t *lock);
static inline void write_unlock_and_destroy(wrlock_t *lock);

static inline void loop_lock_read(wrlock_t *lock) {
    unsigned count = 0;
    long count_max = (255000 - lock_pause.tv_nsec);  // As sleep time increases, decrease acceptable loops.  -mke
    while(pthread_rwlock_tryrdlock(&lock->l)) {
        count++;
        if(lock->val > 10000) {  // Housten, we have a problem. most likely the associated task has been reaped.  Ugh  --mke
            printk("ERROR: loop_lock_read() failure.  Pending read locks > 1000, loops = %d.  Faking it to make it.\n", count);
            _read_unlock(lock);
            lock->val = 0;
            loop_lock_read(lock);
            pthread_mutex_unlock(&nested_lock);
            return;
        } else if(count > count_max) {
            printk("ERROR: loop_lock_read() tries excede %d, dealing with likely deadlock.\n", count_max);
            _read_unlock(lock);
            lock->val = 0;
            loop_lock_read(lock);
            pthread_mutex_unlock(&nested_lock);
            return;
        }
        pthread_mutex_unlock(&nested_lock);
        nanosleep(&lock_pause, NULL);
        nested_lockf(count);
    }
}

static inline void loop_lock_write(wrlock_t *lock) {
    
    unsigned count = 0;
    long count_max = (255000 - lock_pause.tv_nsec);  // As sleep time increases, decrease acceptable loops.  -mke
    while(pthread_rwlock_trywrlock(&lock->l)) {
        count++;
        if(lock->val > 1000) {  // Housten, we have a problem. most likely the associated task has been reaped.  Ugh  --mke
            printk("ERROR: loop_lock_write() failure.  Pending read locks > 1000, loops = %d.  Faking it to make it.\n", count);
            _read_unlock(lock);
            lock->val = 0;
            loop_lock_write(lock);
            pthread_mutex_unlock(&nested_lock);
            return;
        } else if(count > count_max) {
            printk("ERROR: loop_lock_write() tries excede %d, dealing with likely deadlock.\n", count_max);
            _read_unlock(lock);
            lock->val = 0;
            pthread_rwlock_wrlock(&lock->l);
            pthread_mutex_unlock(&nested_lock);
            return;
        }
        pthread_mutex_unlock(&nested_lock);
        nanosleep(&lock_pause, NULL);
        unsigned count = 0;  // This is a different scope from the 'count' above
        nested_lockf(count);
    }
}

static inline int trylockw(wrlock_t *lock, __attribute__((unused)) const char *file, __attribute__((unused)) int line) {
    int status = pthread_rwlock_trywrlock(&lock->l);
#if LOCK_DEBUG
    if (!status) {
        lock->debug.file = file;
        lock->debug.line = line;
        extern int current_pid(void);
        lock->debug.pid = current_pid();
    }
#endif
    return status;
}

#define trylockw(lock) trylockw(lock, __FILE__, __LINE__)

static inline int trylock(lock_t *lock, __attribute__((unused)) const char *file, __attribute__((unused)) int line) {
    int status = pthread_mutex_trylock(&lock->m);
#if LOCK_DEBUG
    if (!status) {
        lock->debug.file = file;
        lock->debug.line = line;
        extern int current_pid(void);
        lock->debug.pid = current_pid();
    }
#endif
    return status;
}

#define trylock(lock) trylock(lock, __FILE__, __LINE__)

// conditions, implemented using pthread conditions but hacked so you can also
// be woken by a signal

typedef struct {
    pthread_cond_t cond;
} cond_t;
#define COND_INITIALIZER ((cond_t) {PTHREAD_COND_INITIALIZER})

// Must call before using the condition
void cond_init(cond_t *cond);
// Must call when finished with the condition (currently doesn't do much but might do something important eventually I guess)
void cond_destroy(cond_t *cond);
// Releases the lock, waits for the condition, and reacquires the lock.
// Returns _EINTR if waiting stopped because the thread received a signal,
// _ETIMEDOUT if waiting stopped because the timout expired, 0 otherwise.
// Will never return _ETIMEDOUT if timeout is NULL.
int must_check wait_for(cond_t *cond, lock_t *lock, struct timespec *timeout);
// Same as wait_for, except it will never return _EINTR
int wait_for_ignore_signals(cond_t *cond, lock_t *lock, struct timespec *timeout);
// Wake up all waiters.
void notify(cond_t *cond);
// Wake up one waiter.
void notify_once(cond_t *cond);

static inline void read_to_write_lock(wrlock_t *lock);
static inline void read_unlock_and_destroy(wrlock_t *lock);

// this is a read-write lock that prefers writers, i.e. if there are any
// writers waiting a read lock will block.
// on darwin pthread_rwlock_t is already like this, on linux you can configure
// it to prefer writers. not worrying about anything else right now.

static inline void wrlock_init(wrlock_t *lock) {
    pthread_rwlockattr_t *pattr = NULL;
#if defined(__GLIBC__)
    pthread_rwlockattr_t attr;
    pattr = &attr;
    pthread_rwlockattr_init(pattr);
    pthread_rwlockattr_setkind_np(pattr, PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP);
#endif
#ifdef JUSTLOG
    if (pthread_rwlock_init(&lock->l, pattr)) printk("URGENT: wrlock_init() error(PID: %d Process: %s)\n",current_pid(), current_comm());
#else
    if (pthread_rwlock_init(&lock->l, pattr)) __builtin_trap();
#endif
    lock->val = lock->line = lock->pid = 0;
    lock->file = NULL;
}

static inline void _lock_destroy(wrlock_t *lock) {
#ifdef JUSTLOG
    if (pthread_rwlock_destroy(&lock->l) != 0) printk("URGENT: wlock_destroy() error(PID: %d Process: %s)\n",current_pid(), current_comm());
#else
    if (pthread_rwlock_destroy(&lock->l) != 0) __builtin_trap();
#endif
}

static inline void lock_destroy(wrlock_t *lock) {
    unsigned count = 0;
    
    nested_lockf(count);
    
    _lock_destroy(lock);
    
    pthread_mutex_unlock(&nested_lock);
}


static inline void _read_lock(wrlock_t *lock) {
    loop_lock_read(lock);
    // assert(lock->val >= 0);  //  If it isn't >= zero we have a problem since that means there is a write lock somehow.  -mke
    if(lock->val) {
        lock->val++;
    } else if (lock->val > -1){  // Deal with insanity.  -mke
        lock->val++;
    } else {
        lock->val++;
        printk("ERROR: _read_lock() val is %d\n", lock->val);
    }
    if(lock->val > 1000) { // We likely have a problem.
        printk("WARNING: _read_lock() has 1000+ pending read locks.  (File: %s, Line: %d) Breaking likely deadlock.\n", lock->file, lock->line);
        read_unlock_and_destroy(lock);
    }
}

static inline void read_lock(wrlock_t *lock) { // Wrapper so that external calls lock, internal calls using _write_unlock() don't -mke
    unsigned count = 0;
    nested_lockf(count);
    _read_lock(lock);
    pthread_mutex_unlock(&nested_lock);
}

static inline void _read_unlock(wrlock_t *lock) {
    if(lock->val <=0) {
        printk("URGENT: pthread_rwlock_unlock error(PID: %d Process: %s count %d) \n",current_pid(), current_comm(), lock->val);
	    return;
    }
    assert(lock->val > 0);
    lock->val--;
#ifdef JUSTLOG
    if (pthread_rwlock_unlock(&lock->l) != 0) printk("URGENT: read_unlock() error(PID: %d Process: %s)\n",current_pid(), current_comm());
#else
    if (pthread_rwlock_unlock(&lock->l) != 0) __builtin_trap();
#endif
}

static inline void read_unlock(wrlock_t *lock) {
    unsigned count = 0;
    nested_lockf(count);

    _read_unlock(lock);
    pthread_mutex_unlock(&nested_lock);
}

static inline void _write_unlock(wrlock_t *lock) {
    assert(lock->val == -1);
    if (pthread_rwlock_unlock(&lock->l) != 0) printk("URGENT: write_lock() error(PID: %d Process: %s)\n",current_pid(), current_comm());
    lock->val = lock->line = lock->pid = 0;
    lock->file = NULL;
}

static inline void write_unlock(wrlock_t *lock) { // Wrap it.  External calls lock, internal calls using _write_unlock() don't -mke
    unsigned count = 0;
    nested_lockf(count);
    _write_unlock(lock);
    pthread_mutex_unlock(&nested_lock);
}

static inline void __write_lock(wrlock_t *lock, const char *file, int line) { // Write lock
    loop_lock_write(lock);

    // assert(lock->val == 0);
    if(lock->val == 0) {
        lock->val = -1;
    } else {
        lock->val = -1;  // I need some place to get a break.  -mke
    }
    lock->file = file;
    lock->line = line;
    lock->pid = current_pid();
}

static inline void _write_lock(wrlock_t *lock, const char *file, int line) {
    unsigned count = 0;
    nested_lockf(count);
    __write_lock(lock, file, line);
    pthread_mutex_unlock(&nested_lock);
}

#define write_lock(lock) _write_lock(lock, __FILE__, __LINE__)

static inline void read_to_write_lock(wrlock_t *lock) {  // Try to atomically swap a RO lock to a Write lock.  -mke
    unsigned count = 0;
    nested_lockf(count);
    _read_unlock(lock);
    __write_lock(lock, __FILE__, __LINE__);
    pthread_mutex_unlock(&nested_lock);
}

static inline void write_to_read_lock(wrlock_t *lock) { // Try to atomically swap a Write lock to a RO lock.  -mke
    unsigned count = 0;
    nested_lockf(count);
    _write_unlock(lock);
    _read_lock(lock);
    pthread_mutex_unlock(&nested_lock);
}

static inline void write_unlock_and_destroy(wrlock_t *lock) {
    unsigned count = 0;
    nested_lockf(count);
    _write_unlock(lock);
    _lock_destroy(lock);
    pthread_mutex_unlock(&nested_lock);
}

static inline void read_unlock_and_destroy(wrlock_t *lock) {
    unsigned count = 0;
    nested_lockf(count);
    _read_unlock(lock);
    _lock_destroy(lock);
    pthread_mutex_unlock(&nested_lock);
}

static inline void nested_lockf(unsigned count) {
    //return; // Short circuit for now
    unsigned myrand = rand() % 50000 + 10000;
    while(pthread_mutex_trylock(&nested_lock)) {
        count++;
        if(count > myrand )
            return;
        nanosleep(&lock_pause, NULL);
    }
}

extern __thread sigjmp_buf unwind_buf;
extern __thread bool should_unwind;
static inline int sigunwind_start() {
    if (sigsetjmp(unwind_buf, 1)) {
        should_unwind = false;
        return 1;
    } else {
        should_unwind = true;
        return 0;
    }
}

static inline void sigunwind_end() {
    should_unwind = false;
}

#endif
