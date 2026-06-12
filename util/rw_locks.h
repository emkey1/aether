//
//  rw_locks.h
//  iSH-AOK
//
//  Created by Michael Miller on 11/29/23.
//
#ifndef RW_LOCK_H
#define RW_LOCK_H

#include <strings.h>
#include "misc.h"
#include "debug.h"
#include "kernel/errno.h"
#include "kernel/log.h"
#include "pthread.h"
#include <pthread.h>
#include <stdatomic.h>

extern void modify_locks_held_count(struct task *task, int value);
extern void task_ref_cnt_mod(struct task *task, int value);

#define loop_lock_read(lock) loop_lock_generic(lock, 0)
#define loop_lock_write(lock) loop_lock_generic(lock, 1)

typedef struct {
    pthread_rwlock_t l;
    atomic_int val;
    int favor_read;
    const char *file;
    int line;
    int pid;
    char comm[16];
    char lname[16];
    void *last_read_lock_pc;
    void *last_read_unlock_pc;
    const char *last_read_lock_file;
    int last_read_lock_line;
    const char *last_read_unlock_file;
    int last_read_unlock_line;
    struct {
        pthread_mutex_t lock;
        int count; // If positive, don't delete yet, wait_to_delete
        bool ready_to_be_freed; // Should be false initially
    } reference;
} wrlock_t;

void wrlock_init(wrlock_t *lock);
static inline int trylockw(wrlock_t *lock);

extern void _lock_destroy(wrlock_t *lock);

static inline void _read_unlock(wrlock_t *lock, const char *file, int line) {
    // Decrement val and record metadata before releasing the rwlock. Doing it
    // after the release races with the next owner: a writer could set val to
    // -1 in the window and this thread's late update would clobber it.
    int old_val = atomic_fetch_sub_explicit(&lock->val, 1, memory_order_relaxed);
    if(old_val <= 0) {
        // Unbalanced read_unlock. Repair the count and do not release a
        // rwlock we evidently do not hold; storing 0 here (as this used to)
        // could erase a writer's -1, and skipping the pthread unlock while a
        // read hold was real leaked the lock and wedged every later writer.
        atomic_fetch_add_explicit(&lock->val, 1, memory_order_relaxed);
        printk("ERROR: read_unlock(%x) error(val: %d lock=%s holder=%s(%d) at %s:%d)\n",
               lock,
               old_val,
               lock->lname[0] ? lock->lname : "-",
               lock->comm[0] ? lock->comm : "-",
               lock->pid,
               lock->file != NULL ? lock->file : "-",
               lock->line);
        printk("ERROR: read_unlock(%x) pcs last_read_lock=%p last_read_unlock=%p current=%s:%d last_lock=%s:%d last_unlock=%s:%d\n",
               lock, lock->last_read_lock_pc, lock->last_read_unlock_pc,
               file != NULL ? file : "-", line,
               lock->last_read_lock_file != NULL ? lock->last_read_lock_file : "-",
               lock->last_read_lock_line,
               lock->last_read_unlock_file != NULL ? lock->last_read_unlock_file : "-",
               lock->last_read_unlock_line);
        return;
    }
    lock->last_read_unlock_pc = __builtin_return_address(0);
    lock->last_read_unlock_file = file;
    lock->last_read_unlock_line = line;
    if (pthread_rwlock_unlock(&lock->l) != 0)
        printk("URGENT: read_unlock(%x) failed\n", lock);
}

#define read_unlock(lock) _read_unlock(lock, __FILE__, __LINE__)

static inline void _write_unlock(wrlock_t *lock) {
    // Clear val and ownership metadata while still holding the rwlock.
    // Clearing after the release races with the next owner: a reader could
    // acquire in the window, see val still -1 (spurious _read_lock error),
    // then this thread's val=0 store erased that reader's count, which made
    // its eventual read_unlock take the unbalanced path, leak the rwlock,
    // and permanently wedge every later write_lock.
    atomic_store_explicit(&lock->val, 0, memory_order_relaxed);
    lock->line = 0;
    lock->pid = -1;
    lock->comm[0] = 0;
    lock->file = NULL;
    if(pthread_rwlock_unlock(&lock->l) != 0)
        printk("URGENT: write_unlock(%x) error on unlock\n", lock);
}

static inline void write_unlock(wrlock_t *lock) { // Wrapper so external calls take the meta-lock.
    _write_unlock(lock);
    return;
}

static inline void loop_lock_generic(wrlock_t *lock, int is_write) {
    if (is_write)
        pthread_rwlock_wrlock(&lock->l);
    else
        pthread_rwlock_rdlock(&lock->l);
}

static inline void _read_lock(wrlock_t *lock, const char *file, int line) {
    loop_lock_read(lock);
    //pthread_rwlock_rdlock(&lock->l);
    // assert(lock->val >= 0);  // If it is negative, a writer is recorded here.
    int old_val = atomic_fetch_add_explicit(&lock->val, 1, memory_order_relaxed);
    int new_val = old_val + 1;
    if(old_val < 0)
        printk("ERROR: _read_lock(%x lock=%s) val is %d\n",
               lock,
               lock->lname[0] ? lock->lname : "-",
               old_val);
    
    if(new_val > 1000 && (new_val == 1001 || (new_val % 256) == 0)) { // We likely have a problem.
        printk("WARNING: _read_lock(%x lock=%s) has %d active readers. holder=%s(%d) at %s:%d current=%s(%d)\n",
               lock,
               lock->lname[0] ? lock->lname : "-",
               new_val,
               lock->comm[0] ? lock->comm : "-",
               lock->pid,
               lock->file != NULL ? lock->file : "-",
               lock->line,
               "-",
               -1);
    }
    lock->last_read_lock_pc = __builtin_return_address(0);
    lock->last_read_lock_file = file;
    lock->last_read_lock_line = line;
    //STRACE("read_lock(%d, %s(%d), %s, %d\n", lock, lock->comm, lock->pid, file, line);
}

#define read_lock(lock) _read_lock(lock, __FILE__, __LINE__)


static inline void _write_lock(wrlock_t *lock) { // Write lock
    // pthread_rwlock_wrlock (unlike a retry loop around trywrlock) registers
    // writer intent on macOS, which prevents new readers from acquiring once a
    // writer is pending.
    pthread_rwlock_wrlock(&lock->l);

    // assert(lock->val == 0);
    atomic_store_explicit(&lock->val, -1, memory_order_relaxed);
}

static inline void write_lock(wrlock_t *lock) {
    _write_lock(lock);
}


static inline void read_to_write_lock(wrlock_t *lock) {  // Try to atomically swap a read lock to a write lock.
    _read_unlock(lock, __FILE__, __LINE__);
    _write_lock(lock);
}

static inline void write_to_read_lock(wrlock_t *lock) { // Try to atomically swap a write lock to a read lock.
    _write_unlock(lock);
    _read_lock(lock, __FILE__, __LINE__);
}

static inline void write_unlock_and_destroy(wrlock_t *lock) {
    _write_unlock(lock);
    _lock_destroy(lock);
}

static inline int trylockw(wrlock_t *lock) {
    int status = pthread_rwlock_trywrlock(&lock->l);
#if LOCK_DEBUG
    if (!status) {
        lock->debug.file = file;
        lock->debug.line = line;
        extern int current_pid(current);
        lock->debug.pid = current_pid(current);
    }
#endif
    if(status == 0) {
        //modify_locks_held_count(current, 1);
        //STRACE("trylockw(%x, %s(%d), %s, %d\n", lock, lock->comm, lock->pid, file, line);
        //lock->pid = current_pid(current);
        //strncpy(lock->comm, current_comm(current), 16);
    }
    return status;
}

static inline int _trylockr(wrlock_t *lock, const char *file, int line) {
    int status = pthread_rwlock_tryrdlock(&lock->l);
    if (status == 0) {
        int old_val = atomic_fetch_add_explicit(&lock->val, 1, memory_order_relaxed);
        if (old_val < 0)
            printk("ERROR: trylockr(%x) succeeded while val is %d\n", lock, old_val);
        lock->last_read_lock_pc = __builtin_return_address(0);
        lock->last_read_lock_file = file;
        lock->last_read_lock_line = line;
    }
    return status;
}

#define trylockr(lock) _trylockr(lock, __FILE__, __LINE__)

#endif // RW_LOCK_H
