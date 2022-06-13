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
extern int current_delay_task_delete_requests(void);

extern struct timespec lock_pause;

extern pthread_mutex_t nested_lock; // Used to make all lock operations atomic, even read->write and right->read -mke

typedef struct {
    pthread_mutex_t m;
    pthread_t owner;
    const char *comm;
    int pid;
#if LOCK_DEBUG
    struct lock_debug {
        const char *file; // doubles as locked
        int line;
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

static inline void __lock(lock_t *lock, int log_lock, __attribute__((unused)) const char *file, __attribute__((unused)) int line) {
    unsigned int count = 0;
    int random_wait = WAIT_SLEEP + rand() % WAIT_SLEEP/2;
    struct timespec lock_pause = {0 /*secs*/, random_wait /*nanosecs*/};
    long count_max = (WAIT_MAX_UPPER - random_wait);  // As sleep time increases, decrease acceptable loops.  -mke
    
    while(pthread_mutex_trylock(&lock->m)) {
        count++;
        nanosleep(&lock_pause, NULL);
        if(count > count_max) {
            if(!log_lock)
                printk("ERROR: Possible deadlock(lock(%d)), aborted lock attempt(PID: %d Process: %s) (File: %s Line: %d)\n", lock, current_pid(), current_comm(), file, line);
            return;
        }
        // Loop until lock works.  Maybe this will help make the multithreading work? -mke
    }

    if(count > count_max * .90) {
        if(!log_lock)
            printk("WARNING: large lock attempt count(%d) in Function: __lock(%d) (PID: %d Process: %s) (File: %s Line: %d)\n",count, lock, lock->pid, lock->comm, file, line);
    }

    lock->owner = pthread_self();
    lock->pid = current_pid();
    lock->comm = current_comm();
#if LOCK_DEBUG
    assert(lock->debug.initialized);
    assert(!lock->debug.file && "Attempting to recursively lock");
    lock->debug.file = file;
    lock->debug.line = line;
    extern int current_pid(void);
    lock->debug.pid = current_pid();
#endif
}

#define lock(lock, log_lock) __lock(lock, log_lock, __FILE__, __LINE__)

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
    const char *comm;
} wrlock_t;


static inline void nested_lockf(unsigned count);
static inline void nested_unlockf(void);
static inline void _read_unlock(wrlock_t *lock);
static inline void _write_unlock(wrlock_t *lock);
static inline void write_unlock_and_destroy(wrlock_t *lock);

static inline void loop_lock_read(wrlock_t *lock) {
    unsigned count = 0;
    int random_wait = WAIT_SLEEP + rand() % WAIT_SLEEP/2;
//    struct timespec lock_pause = {0 /*secs*/, random_wait /*nanosecs*/};
    struct timespec lock_pause = {0 /*secs*/, random_wait * .5 /*nanosecs*/};
    long count_max = (WAIT_MAX_UPPER - random_wait);  // As sleep time increases, decrease acceptable loops.  -mke
    while(pthread_rwlock_tryrdlock(&lock->l)) {
        count++;
        if(lock->val > 1000) {  // Housten, we have a problem. most likely the associated task has been reaped.  Ugh  --mke
            printk("ERROR: loop_lock_read(%d) failure.  Pending read locks > 1000, loops = %d.  Faking it to make it (PID: %d, Process: %s).\n", lock, count, current_pid(), current_comm());
            _read_unlock(lock);
            lock->val = 0;
            loop_lock_read(lock);
            
            return;
        } else if(count > (count_max * 5)) { // Need to be more persistent for RO locks
            printk("ERROR: loop_lock_read(%d) tries excede %d, dealing with likely deadlock.  (PID: %d, Process: %s).\n", lock, count_max * 5, current_pid(), current_comm());
            if(lock->val > 0) {
                lock->val++;
            } else if (lock->val < 0) {
                _write_unlock(lock);
            } else {
                printk("ERROR: lock->val = 0 in loop_lock_write(PID: %d Process: %s)\n", lock->pid, lock->comm);
            }
            
            return;
        }
        nested_unlockf(); // Give some other process a little time to get the lock.  Bad perhaps?
        nanosleep(&lock_pause, NULL);
        nested_lockf(count);
    }
}

static inline void loop_lock_write(wrlock_t *lock) {
    unsigned count = 0;
    int random_wait = WAIT_SLEEP + rand() % WAIT_SLEEP*2;
    struct timespec lock_pause = {0 /*secs*/, random_wait /*nanosecs*/};
    long count_max = (WAIT_MAX_UPPER - random_wait);  // As sleep time increases, decrease acceptable loops.  -mke
    while(pthread_rwlock_trywrlock(&lock->l)) {
        count++;
        if(lock->val > 1000) {  // Housten, we have a problem. most likely the associated task has been reaped.  Ugh  --mke
            printk("ERROR: loop_lock_write(%d) failure.  Pending read locks > 1000, loops = %d.  Faking it to make it.(PID: %d Process: %s)\n", lock, count, lock->pid, lock->comm);
            _read_unlock(lock);
            lock->val = 0;
            lock->pid = 0;
            lock->comm = NULL;
            loop_lock_write(lock);
            return;
        } else if(count > count_max) {
            printk("ERROR: loop_lock_write(%d) tries excede %d, dealing with likely deadlock.(PID: %d Process: %s)\n", lock, count_max, lock->pid, lock->comm);
	        if(lock->val > 0) {
                _read_unlock(lock);
	        } else if (lock->val < 0) {
	            _write_unlock(lock);
	        } else {
	            printk("ERROR: lock->val = 0 in loop_lock_write(PID: %d Process: %s)\n", lock->pid, lock->comm);
	        }
            
            lock->val = 0;
            loop_lock_write(lock);
            return;
        }
        
        nested_unlockf();
        nanosleep(&lock_pause, NULL);
        unsigned mycount = 0;  
        nested_lockf(mycount);
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
    if (pthread_rwlock_init(&lock->l, pattr))
        printk("URGENT: wrlock_init() error(PID: %d Process: %s)\n",current_pid(), current_comm());
#else
    if (pthread_rwlock_init(&lock->l, pattr)) __builtin_trap();
#endif
    lock->val = lock->line = lock->pid = 0;
    lock->comm = NULL;
    lock->file = NULL;
}

static inline void _lock_destroy(wrlock_t *lock) {
    while(current_delay_task_delete_requests()) { // Wait for now, task is in one or more critical sections
        nanosleep(&lock_pause, NULL);
    }
#ifdef JUSTLOG
    if (pthread_rwlock_destroy(&lock->l) != 0) {
        printk("URGENT: lock_destroy() error(PID: %d Process: %s)\n",current_pid(), current_comm());
        printk("INFO: lock_destroy(), delay_task_delete_requests = %d\n", current_delay_task_delete_requests());
    }
#else
    if (pthread_rwlock_destroy(&lock->l) != 0) __builtin_trap();
#endif
}

static inline void lock_destroy(wrlock_t *lock) {
    unsigned count = 0;
    
    nested_lockf(count);
    _lock_destroy(lock);
    nested_unlockf();
}


static inline void _read_lock(wrlock_t *lock) {
    loop_lock_read(lock);
    // assert(lock->val >= 0);  //  If it isn't >= zero we have a problem since that means there is a write lock somehow.  -mke
    if(lock->val) {
        lock->val++;
    } else if (lock->val > -1){  // Deal with insanity.  -mke
        lock->val++;
    } else {
        printk("ERROR: _read_lock() val is %d\n", lock->val);
        lock->val++;
    }
    
    if(lock->val > 1000) { // We likely have a problem.
        printk("WARNING: _read_lock() has 1000+ pending read locks.  (File: %s, Line: %d) Breaking likely deadlock/process corruption(PID: %d Process: %s.\n", lock->file, lock->line,lock->pid, lock->comm);
        read_unlock_and_destroy(lock);
        return;
    }
    
    lock->pid = current_pid();
    lock->comm = current_comm();
}

static inline void read_lock(wrlock_t *lock) { // Wrapper so that external calls lock, internal calls using _read_unlock() don't -mke
    unsigned count = 0;
    nested_lockf(count);
    _read_lock(lock);
    nested_unlockf();
}

static inline void _read_unlock(wrlock_t *lock) {
    if(lock->val <=0) {
        printk("ERROR: read_unlock(%d) error(PID: %d Process: %s count %d) \n",lock, current_pid(), current_comm(), lock->val);
        lock->val = 0;
	    return;
    }
    assert(lock->val > 0);
    if (pthread_rwlock_unlock(&lock->l) != 0)
        printk("URGENT: read_unlock(%d) error(PID: %d Process: %s)\n", lock, current_pid(), current_comm());
    lock->val--;
}

static inline void read_unlock(wrlock_t *lock) {
    unsigned count = 0;
    nested_lockf(count);
    _read_unlock(lock);
    nested_unlockf();
}

static inline void _write_unlock(wrlock_t *lock) {
    if(pthread_rwlock_unlock(&lock->l) != 0)
        printk("URGENT: write_unlock(%d:%d) error(PID: %d Process: %s)\n", lock, lock->val, current_pid(), current_comm());
    if(lock->val != -1) {
        printk("ERROR: write_unlock(%d) on lock with val of %d (PID: %d Process: %s", lock, lock->val, current_pid(), current_comm());
    }
    //assert(lock->val == -1);
    lock->val = lock->line = lock->pid = 0;
    lock->comm = NULL;
    lock->file = NULL;
}

static inline void write_unlock(wrlock_t *lock) { // Wrap it.  External calls lock, internal calls using _write_unlock() don't -mke
    unsigned count = 0;
    nested_lockf(count);
    _write_unlock(lock);
    nested_unlockf();
}

static inline void __write_lock(wrlock_t *lock, const char *file, int line) { // Write lock
    loop_lock_write(lock);

    // assert(lock->val == 0);
    lock->val = -1;
    lock->file = file;
    lock->line = line;
    lock->pid = current_pid();
    lock->comm = current_comm();
}

static inline void _write_lock(wrlock_t *lock, const char *file, int line) {
    unsigned count = 0;
    nested_lockf(count);
    __write_lock(lock, file, line);
    nested_unlockf();
}

#define write_lock(lock) _write_lock(lock, __FILE__, __LINE__)

static inline void read_to_write_lock(wrlock_t *lock) {  // Try to atomically swap a RO lock to a Write lock.  -mke
    unsigned count = 0;
    nested_lockf(count);
    _read_unlock(lock);
    __write_lock(lock, __FILE__, __LINE__);
    nested_unlockf();
}

static inline void write_to_read_lock(wrlock_t *lock) { // Try to atomically swap a Write lock to a RO lock.  -mke
    unsigned count = 0;
    nested_lockf(count);
    _write_unlock(lock);
    _read_lock(lock);
    nested_unlockf();
}

static inline void write_unlock_and_destroy(wrlock_t *lock) {
    unsigned count = 0;
    nested_lockf(count);
    _write_unlock(lock);
    _lock_destroy(lock);
    nested_unlockf();
}

static inline void read_unlock_and_destroy(wrlock_t *lock) {
    unsigned count = 0;
    nested_lockf(count);
    _read_unlock(lock);
    _lock_destroy(lock);
    nested_unlockf();
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

static inline void nested_unlockf(void) {
    pthread_mutex_unlock(&nested_lock);
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
