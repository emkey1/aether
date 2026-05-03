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
    struct {
        pthread_mutex_t lock;
        int count; // If positive, don't delete yet, wait_to_delete
        bool ready_to_be_freed; // Should be false initially
    } reference;
} wrlock_t;

void wrlock_init(wrlock_t *lock);
static inline void read_unlock_and_destroy(wrlock_t *lock);
static inline int trylockw(wrlock_t *lock);

extern void _lock_destroy(wrlock_t *lock);

static inline void _read_unlock(wrlock_t *lock) {
    int old_val = atomic_load_explicit(&lock->val, memory_order_relaxed);
    if(old_val <= 0) {
        //printk("ERROR: read_unlock(%x) error(PID: %d Process: %s count %d) (%s:%d)\n",lock, current_pid(current), current_comm(current), lock->val);
        printk("ERROR: read_unlock(%x) error(val: %d)\n", lock, old_val);
        atomic_store_explicit(&lock->val, 0, memory_order_relaxed);
        lock->pid = -1;
        lock->comm[0] = 0;
        //modify_locks_held_count(current, -1);
        //STRACE("read_unlock(%x, %s(%d), %s, %d\n", lock, lock->comm, lock->pid, file, line);
        return;
    }
    if (pthread_rwlock_unlock(&lock->l) != 0)
//        printk("URGENT: read_unlock(%x) error(PID: %d Process: %s)\n", lock, current_pid(current), current_comm(current));
        printk("URGENT: read_unlock(%x) failed\n", lock);
    atomic_fetch_sub_explicit(&lock->val, 1, memory_order_relaxed);
    //modify_locks_held_count(current, -1);
    //STRACE("read_unlock(%x, %s(%d), %s, %d\n", lock, lock->comm, lock->pid, file, line);
}

static inline void read_unlock(wrlock_t *lock) {
    _read_unlock(lock);
    return;
}

static inline void _write_unlock(wrlock_t *lock) {
    if(pthread_rwlock_unlock(&lock->l) != 0)
      //  printk("URGENT: write_unlock(%x:%d) error(PID: %d Process: %s) \n", lock, lock->val, current_pid(current), current_comm(current));
        printk("URGENT: write_unlock(%x:%d) error on unlock\n", lock,
               atomic_load_explicit(&lock->val, memory_order_relaxed));
    if(atomic_load_explicit(&lock->val, memory_order_relaxed) != -1) {
        //printk("ERROR: write_unlock(%x) on lock with val of %d (PID: %d Process: %s )\n", lock, lock->val, current_pid(current), current_comm(current));
        // printk("ERROR: write_unlock(%x) on lock with val of %d\n", lock, lock->val);  // Comment out for now.  Much noise, little impact (So far as I can tell)
    }
    //assert(lock->val == -1);
    atomic_store_explicit(&lock->val, 0, memory_order_relaxed);
    lock->line = lock->pid = 0;
    lock->pid = -1;
    lock->comm[0] = 0;
    //STRACE("write_unlock(%x, %s(%d), %s, %d\n", lock, lock->comm, lock->pid, file, line);
    lock->file = NULL;
    //modify_locks_held_count(current, -1);
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

static inline void _read_lock(wrlock_t *lock) {
    loop_lock_read(lock);
    //pthread_rwlock_rdlock(&lock->l);
    // assert(lock->val >= 0);  // If it is negative, a writer is recorded here.
    int old_val = atomic_fetch_add_explicit(&lock->val, 1, memory_order_relaxed);
    int new_val = old_val + 1;
    if(old_val < 0)
        printk("ERROR: _read_lock() val is %d\n", old_val);
    
    if(new_val > 1000) { // We likely have a problem.
        printk("WARNING: _read_lock(%x) has 1000+ pending read locks.  (File: %s, Line: %d) Breaking likely deadlock/process corruption(PID: %d Process: %s.\n", lock, lock->file, lock->line,lock->pid, lock->comm);
        read_unlock_and_destroy(lock);
        //STRACE("read_lock(%d, %s(%d), %s, %d\n", lock, lock->comm, lock->pid, file, line);
        return;
    }
    
    /* lock->pid = current_pid(current);
    if(lock->pid > 9)
        strncpy((char *)lock->comm, current_comm(current), 16); */
    //STRACE("read_lock(%d, %s(%d), %s, %d\n", lock, lock->comm, lock->pid, file, line);
}

static inline void read_lock(wrlock_t *lock) { // Wrapper so external calls take the meta-lock.
    _read_lock(lock);
}


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
    _read_unlock(lock);
    _write_lock(lock);
}

static inline void write_to_read_lock(wrlock_t *lock) { // Try to atomically swap a write lock to a read lock.
    _write_unlock(lock);
    _read_lock(lock);
}

static inline void write_unlock_and_destroy(wrlock_t *lock) {
    _write_unlock(lock);
    _lock_destroy(lock);
}

static inline void read_unlock_and_destroy(wrlock_t *lock) {
    if(trylockw(lock)) // Expected to already be held; only fall back to read unlock if it is still active.
        _read_unlock(lock);
    
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

static inline int trylockr(wrlock_t *lock) {
    int status = pthread_rwlock_tryrdlock(&lock->l);
    if (status == 0) {
        int old_val = atomic_fetch_add_explicit(&lock->val, 1, memory_order_relaxed);
        if (old_val < 0)
            printk("ERROR: trylockr(%x) succeeded while val is %d\n", lock, old_val);
    }
    return status;
}

#endif // RW_LOCK_H
