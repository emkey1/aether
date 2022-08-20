#define _GNU_SOURCE
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include "kernel/calls.h"
#include "kernel/task.h"
#include "kernel/task.h"
#include "kernel/resource_locking.h"
#include "emu/memory.h"
#include "emu/tlb.h"
#include "platform/platform.h"
#include "util/sync.h"
#include <pthread.h>
#include <libkern/OSAtomic.h>

pthread_mutex_t multicore_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t extra_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t delay_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t atomic_l_lock = PTHREAD_MUTEX_INITIALIZER;

dword_t extra_lock_pid = -1;
char extra_lock_comm[16] = "";
bool extra_lock_held = false;
time_t newest_extra_lock_time = 0;
unsigned maxl = 10; // Max age of an extra_lock
bool BOOTING = true;

bool doEnableMulticore; // Enable multicore if toggled, should default to false
bool doEnableExtraLocking; // Enable extra locking if toggled, should default to false
unsigned doLockSleepNanoseconds; // How many nanoseconds should __lock() sleep between retries

__thread struct task *current;

static dword_t last_allocated_pid = 0;
static struct pid pids[MAX_PID + 1] = {};
lock_t pids_lock = LOCK_INITIALIZER;
lock_t block_lock = LOCK_INITIALIZER;
struct list alive_pids_list;

static bool pid_empty(struct pid *pid) {
    return pid->task == NULL && list_empty(&pid->session) && list_empty(&pid->pgroup);
}

struct pid *pid_get(dword_t id) {
    if (id > sizeof(pids)/sizeof(pids[0]))
        return NULL;
    struct pid *pid = &pids[id];
    if (pid_empty(pid))
        return NULL;
    return pid;
}

struct task *pid_get_task_zombie(dword_t id) {
    struct pid *pid = pid_get(id);
    if (pid == NULL)
        return NULL;
    struct task *task = pid->task;
    return task;
}

struct task *pid_get_task(dword_t id) {
    struct task *task = pid_get_task_zombie(id);
    if (task != NULL && task->zombie)
        return NULL;
    return task;
}

struct pid *pid_get_last_allocated() {
    if (!last_allocated_pid) {
        return NULL;
    }
    return pid_get(last_allocated_pid);
}

dword_t get_count_of_blocked_tasks() {
    complex_lockt(&pids_lock, 0, __FILE__, __LINE__);
    modify_critical_region_counter(current, 1, __FILE__, __LINE__);
    dword_t res = 0;
    struct pid *pid_entry;
    list_for_each_entry(&alive_pids_list, pid_entry, alive) {
        if (pid_entry->task->io_block) {
            res++;
        }
    }
    modify_critical_region_counter(current, -1, __FILE__, __LINE__);
    unlock(&pids_lock);
    return res;
}

dword_t get_count_of_alive_tasks() {
    complex_lockt(&pids_lock, 0, __FILE__, __LINE__);
    dword_t res = 0;
    struct list *item;
    list_for_each(&alive_pids_list, item) {
        res++;
    }
    unlock(&pids_lock);
    return res;
}

struct task *task_create_(struct task *parent) {
    complex_lockt(&pids_lock, 0, __FILE__, __LINE__);
    do {
        last_allocated_pid++;
        if (last_allocated_pid > MAX_PID) last_allocated_pid = 1;
    } while (!pid_empty(&pids[last_allocated_pid]));
    struct pid *pid = &pids[last_allocated_pid];
    pid->id = last_allocated_pid;
    list_init(&pid->alive);
    list_init(&pid->session);
    list_init(&pid->pgroup);

    struct task *task = malloc(sizeof(struct task));
    if (task == NULL) {
        unlock(&pids_lock);
        return NULL;
    }
    *task = (struct task) {};
    if (parent != NULL)
        *task = *parent;

    task->pid = pid->id;
    pid->task = task;
    list_add(&alive_pids_list, &pid->alive);

    list_init(&task->children);
    list_init(&task->siblings);
    if (parent != NULL) {
        task->parent = parent;
        list_add(&parent->children, &task->siblings);
    }
    unlock(&pids_lock);

    task->pending = 0;
    list_init(&task->queue);
    task->clear_tid = 0;
    task->robust_list = 0;
    task->did_exec = false;
    lock_init(&task->general_lock);

    task->sockrestart = (struct task_sockrestart) {};
    list_init(&task->sockrestart.listen);

    task->waiting_cond = NULL;
    task->waiting_lock = NULL;
    lock_init(&task->waiting_cond_lock);
    cond_init(&task->pause);

    lock_init(&task->ptrace.lock);
    cond_init(&task->ptrace.cond);
    
    task->locks_held.count = 0; // counter used to keep track of pending locks associated with task.  Do not delete when locks are present.  -mke
    task->critical_region.count = 0; // counter used to delay task deletion if positive.  --mke
    return task;
}

void task_destroy(struct task *task) {
    int elock_fail = 0;
    if(doEnableExtraLocking)
        elock_fail = extra_lockf(task->pid);
    
    while((critical_region_count(task) || (locks_held_count(task))) && (task->exiting == false)) { // Wait for now, task is in one or more critical sections, and/or has locks
        nanosleep(&lock_pause, NULL);
    }

    bool IShould = false;
    if(!trylock(&pids_lock)) {  // Non blocking, just in case, be sure pids_lock is set.  -mke
       printk("WARNING: pids_lock was not set\n");
       IShould = true;
    }
    
    while((critical_region_count(task) || (locks_held_count(task))) && (task->exiting == false)) { // Wait for now, task is in one or more critical sections, and/or has locks
        nanosleep(&lock_pause, NULL);
        nanosleep(&lock_pause, NULL);
    }
    list_remove(&task->siblings);
    struct pid *pid = pid_get(task->pid);
    pid->task = NULL;
    
    while((critical_region_count(task) || (locks_held_count(task))) && (task->exiting == false)) { // Wait for now, task is in one or more critical sections, and/or has locks
        nanosleep(&lock_pause, NULL);
    }
    list_remove(&pid->alive);
    if((doEnableExtraLocking) && (!elock_fail))
        extra_unlockf(task->pid);
    
    if(IShould)
        unlock(&pids_lock);
    
    while((critical_region_count(task) || (locks_held_count(task))) && (task->exiting == false)) { // Wait for now, task is in one or more critical sections, and/or has locks
        nanosleep(&lock_pause, NULL);
    }
    
    free(task);
}

void run_at_boot() {  // Stuff we run only once, at boot time.
    //atomic_thread_fence(__ATOMIC_SEQ_CST);
    struct uname uts;
    do_uname(&uts);
    unsigned short ncpu = get_cpu_count();
    printk("iSH-AOK %s booted on %d emulated %s CPU(s)\n",uts.release, ncpu, uts.arch);
    BOOTING = false;

}

void task_run_current() {
    struct cpu_state *cpu = &current->cpu;
    struct tlb tlb = {};
    tlb_refresh(&tlb, &current->mem->mmu);
    
    while (true) {
        read_lock(&current->mem->lock, __FILE__, __LINE__);
        
        if(doEnableMulticore) {
            // Do nothing
        } else {
            //pthread_mutex_lock(&multicore_lock);
            threaded_lock(&multicore_lock, 1);
        }
        
        int interrupt = cpu_run_to_interrupt(cpu, &tlb);
        
        if(!doEnableMulticore)
            pthread_mutex_unlock(&multicore_lock);
 
        read_unlock(&current->mem->lock, __FILE__, __LINE__);
        //struct timespec while_pause = {0 /*secs*/, WAIT_SLEEP /*nanosecs*/};
        if(current->parent != NULL) {
            current->parent->group->group_count_in_int++; // Keep track of how many children the parent has
            handle_interrupt(interrupt);
            current->parent->group->group_count_in_int--;
        } else {
            handle_interrupt(interrupt);
        }
    }
}

static void *task_thread(void *task) {
    
    current = task;
    // int elock_fail = 0;
    
    current->critical_region.count = 0; // Is this needed?  -mke
    
    //////modify_critical_region_counter(task, 1);
   // if(doEnableExtraLocking)
    //    elock_fail = extra_lockf(current->pid);
    update_thread_name();
    //////modify_critical_region_counter(task, -1);
    //if((doEnableExtraLocking) && (!elock_fail))
     //   extra_unlockf(0);
    
    task_run_current();
    die("task_thread returned"); // above function call should never return
}

static pthread_attr_t task_thread_attr;
__attribute__((constructor)) static void create_attr() {
    pthread_attr_init(&task_thread_attr);
    pthread_attr_setdetachstate(&task_thread_attr, PTHREAD_CREATE_DETACHED);
}

void task_start(struct task *task) {
    if (pthread_create(&task->thread, &task_thread_attr, task_thread, task) < 0)
        die("could not create thread");
}

int_t sys_sched_yield() {
    STRACE("sched_yield()");
    sched_yield();
    return 0;
}

void update_thread_name() {
    char name[16]; // As long as Linux will let us make this
    snprintf(name, sizeof(name), "-%d", current->pid);
    size_t pid_width = strlen(name);
    size_t name_width = snprintf(name, sizeof(name), "%s", current->comm);
    sprintf(name + (name_width < sizeof(name) - 1 - pid_width ? name_width : sizeof(name) - 1 - pid_width), "-%d", current->pid);
#if __APPLE__
    pthread_setname_np(name);
#else
    pthread_setname_np(pthread_self(), name);
#endif
}

/* Let me be clear here.  The following two functions (extra_lockf() & extra_unlockf() are horrible hacks.  
   If I were a better programmer I'd actually figure out and fix the problems they are mitigating.  After a couple of
   years of trying the better programmer approach on and off I've given up and gone full on kludge King.  -mke */
int extra_lockf(dword_t pid) {
    //if(current != NULL)
        ////modify_critical_region_counter(current, 1, __FILE__, __LINE__);
    pthread_mutex_lock(&extra_lock);
    //extra_lock_pid = pid;
    extra_lock_pid = current->pid;
    extra_lock_held = true; //
    if(current != NULL)
        ////modify_critical_region_counter(current, -1, __FILE__, __LINE__);
    return 0;
    
    time_t now;
    time(&now);

    if(!newest_extra_lock_time)
        time(&newest_extra_lock_time); // Initialize
    
    unsigned int count = 0;
    int random_wait = WAIT_SLEEP + rand() % WAIT_SLEEP/2;
    struct timespec mylock_pause = {0 /*secs*/, random_wait /*nanosecs*/};
    long count_max = (WAIT_MAX_UPPER - WAIT_SLEEP);  // As sleep time increases, decrease acceptable loops.  -mke
    
    if((now - newest_extra_lock_time > maxl) && (extra_lock_held)) { // If we have a lock, and there has been no activity for awhile, kill it
        printk("ERROR: The newest_extra_lock time(extra_lockf) has exceeded %d seconds (%d). Resetting\n", maxl, now - newest_extra_lock_time);
        pthread_mutex_unlock(&extra_lock);
        
        while(pthread_mutex_trylock(&extra_lock)) {
            count++;
            nanosleep(&mylock_pause, NULL);
            //mylock_pause.tv_nsec+=10;
            if(count > count_max) {
                printk("ERROR: Possible deadlock(extra_lockf(), aborted lock attempt(PID: %d Process: %s)\n", current_pid(), current_comm() );
                extra_lock_pid = 0;
                strcpy(extra_lock_comm, "");
                pthread_mutex_unlock(&extra_lock);
                return 1;
            }
            // Loop until lock works.  Maybe this will help make the multithreading work? -mke
        }
        
        extra_lock_pid = pid;
        extra_lock_held = true; //
        time(&newest_extra_lock_time);  // Update time
        current->locks_held.count++;
        return 0;
    }
        
    while(pthread_mutex_trylock(&extra_lock)) {
        count++;
        nanosleep(&mylock_pause, NULL);
        //mylock_pause.tv_nsec+=10;
        if(count > count_max) {
            printk("ERROR: Possible deadlock(extra_lockf), aborted lock attempt(PID: %d )\n", extra_lock_pid);
            extra_lock_pid = 0;
            pthread_mutex_unlock(&extra_lock);
            strcpy(extra_lock_comm, "");
            return 1;
        }
            // Loop until lock works.  Maybe this will help make the multithreading work? -mke
    }

    if(count > count_max * .90) {
        //printk("WARNING: large lock attempt count(Function: extra_lockf(%d) PID: %d Process: %s)\n",count, current->pid, current->comm);
        printk("WARNING: large lock attempt count(Function: extra_lockf(%d) PID: %d)\n",count, extra_lock_pid);
    }
    
    time(&newest_extra_lock_time);  // Update time
    extra_lock_pid = pid;  //Save, we may need it later to make sure the lock gets removed if the pid is killed
    extra_lock_held = true; //
    return 0;
}

void extra_unlockf(dword_t pid) {
    if(current != NULL)
        ////modify_critical_region_counter(current, 1, __FILE__, __LINE__);
    pthread_mutex_unlock(&extra_lock);
    if(current != NULL)
        ////modify_critical_region_counter(current, -1, __FILE__, __LINE__);
    
    return;
    
    time_t now;
    time(&now);
    if((now - newest_extra_lock_time > maxl) && (extra_lock_held)) { // If we have a lock, and there has been no activity for awhile, kill it
        printk("ERROR: The newest_extra_lock time(unlockf) has exceeded %d seconds (%d) (%d).  Resetting\n", maxl, now, newest_extra_lock_time);
        pthread_mutex_unlock(&extra_lock);
        current->locks_held.count--;
        extra_lock_pid = 0;
        strcpy(extra_lock_comm, "");
        extra_lock_held = false;
        return;
    }
    
    if((pid_get(extra_lock_pid) == NULL) && (extra_lock_pid)) {
        printk("WARNING: Previous extra_lock() PID(%d) missing\n", extra_lock_pid); // It will be zero if not relevant
    }
    
    if(pid)
        // Placeholder
        if(!extra_lock_held) {
            printk("WARNING: Trying to extra_unlockf() when no lock exists\n");
            return;
        }
    pthread_mutex_unlock(&extra_lock);
    extra_lock_pid = 0;
    strcpy(extra_lock_comm, "");
    extra_lock_held = false; //
    current->locks_held.count--;
    return;
}
