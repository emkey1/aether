#define _GNU_SOURCE
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include "kernel/calls.h"
#include "kernel/task.h"
#include "emu/memory.h"
#include "emu/tlb.h"
#include "jit/jit.h"
#include "platform/platform.h"
#include "util/sync.h"
#include "util/timer.h"
#include <libkern/OSAtomic.h>
#include <os/proc.h>
#include <dlfcn.h>

#define GRACE_PERIOD 2 // How long we want to deallocate tasks that have exited

pthread_mutex_t multicore_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t extra_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t delay_lock = PTHREAD_MUTEX_INITIALIZER;
extern lock_t atomic_l_lock;
pthread_mutex_t wait_for_lock = PTHREAD_MUTEX_INITIALIZER;
time_t boot_time;  // Store the boot time.

struct list tasks_pending_deletion_queue;
pthread_mutex_t tasks_pending_deletion_lock = PTHREAD_MUTEX_INITIALIZER;

int iOSMajorRelease;

bool doEnableMulticore; // Enable multicore if toggled, should default to false
bool isGlibC = false; // Try to guess if we're running a non-musl distro.
bool doEnableExtraLocking; // Enable extra locking if toggled, should default to true

__thread struct task *current;

static dword_t last_allocated_pid = 0;
static struct pid pids[MAX_PID + 1] = {};
lock_t pids_lock;
lock_t block_lock;
struct list alive_pids_list;

void init_pending_queues(void) {
// Initialize the pending deletion queues.  Tasks, memory and file descriptors (eventually)
    list_init(&tasks_pending_deletion_queue);
    
}

static bool pid_empty(struct pid *pid) {
    return pid->task == NULL && list_empty(&pid->session) && list_empty(&pid->pgroup);
}

struct pid *pid_get(dword_t id) {
    if (id >= sizeof(pids)/sizeof(pids[0]))
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

struct task *pid_get_task_ref(dword_t id) {
    complex_lockt(&pids_lock, 0);
    struct task *task = pid_get_task(id);
    if (task != NULL)
        task_ref_cnt_mod(task, 1);
    unlock(&pids_lock);
    return task;
}

void task_snapshot_release(struct task_snapshot *snapshot) {
    for (unsigned i = 0; i < snapshot->count; i++)
        task_ref_cnt_mod(snapshot->tasks[i], -1);
    free(snapshot->tasks);
    snapshot->tasks = NULL;
    snapshot->count = 0;
}

int task_snapshot_collect(struct task_snapshot *snapshot, bool leaders_only) {
    unsigned cap = 0;
    complex_lockt(&pids_lock, 0);
    struct pid *pid_entry;
    list_for_each_entry(&alive_pids_list, pid_entry, alive) {
        struct task *task = pid_entry->task;
        if (task == NULL || task->zombie || task->exiting)
            continue;
        if (leaders_only && !task_is_leader(task))
            continue;
        if (snapshot->count == cap) {
            unsigned new_cap = cap ? cap * 2 : 64;
            struct task **new_tasks = realloc(snapshot->tasks, sizeof(*new_tasks) * new_cap);
            if (new_tasks == NULL) {
                unlock(&pids_lock);
                task_snapshot_release(snapshot);
                return _ENOMEM;
            }
            snapshot->tasks = new_tasks;
            cap = new_cap;
        }
        task_ref_cnt_mod(task, 1);
        snapshot->tasks[snapshot->count++] = task;
    }
    unlock(&pids_lock);
    return 0;
}

struct pid *pid_get_last_allocated(void) {
    if (!last_allocated_pid) {
        return NULL;
    }
    return pid_get(last_allocated_pid);
}

inline void task_ref_cnt_mod(struct task *task, int value) { // value should only be -1 or 1.
    // Keep track of how many threads are referencing this task. This used to
    // be skipped when doEnableExtraLocking was off, but the flag is a live
    // preference toggle: flipping it mid-run left counts taken under one
    // setting and released under the other permanently imbalanced. The count
    // gates task teardown, so it is now maintained unconditionally; as a
    // lock-free atomic it is cheap enough to always be on.
    if(task == NULL) {
        if(current != NULL) {
            task = current;
        } else {
            return;
        }
    }

    if (value != 1 && value != -1) {
        printk("ERROR: invalid task refcount delta %d for %s:%d\n",
               value, task->comm, task->pid);
        return;
    }

    int old_count = atomic_load_explicit(&task->reference.count, memory_order_relaxed);
    do {
        if(((old_count + value) < 0) && (task->pid > 9)) { // Prevent the count from going negative.
            void *caller = __builtin_return_address(0);
            Dl_info caller_info = {};
            const char *caller_name = "?";
            ptrdiff_t caller_offset = 0;
            if (caller != NULL && dladdr(caller, &caller_info) != 0 && caller_info.dli_sname != NULL) {
                caller_name = caller_info.dli_sname;
                caller_offset = (char *) caller - (char *) caller_info.dli_saddr;
            }
            printk("ERROR: Attempt to decrement task reference count to be negative, ignoring(%s:%d) (%d - %d) caller=%s+%td addr=%p\n",
                   task->comm, task->pid, old_count, value, caller_name, caller_offset, caller);
            return;
        }
    } while (!atomic_compare_exchange_weak_explicit(&task->reference.count, &old_count, old_count + value,
                                                    memory_order_acq_rel, memory_order_relaxed));
}

dword_t get_count_of_blocked_tasks(void) {
    // task_ref_cnt_mod(current, 1);  // Not needed?
    dword_t res = 0;
    struct pid *pid_entry;
    complex_lockt(&pids_lock, 0);
    list_for_each_entry(&alive_pids_list, pid_entry, alive) {
        if (pid_entry->task->io_block) {
            res++;
        }
    }
    // task_ref_cnt_mod(current, -1);
    unlock(&pids_lock);
    return res;
}

dword_t get_count_of_alive_tasks(void) {
    complex_lockt(&pids_lock, 0);
    dword_t res = 0;
    struct list *item;
    list_for_each(&alive_pids_list, item) {
        res++;
    }
    unlock(&pids_lock);
    return res;
}

// Linux-style load average computed over the guest's OWN runnable tasks, so
// /proc/loadavg reflects the guest rather than the host load that the platform
// getloadavg returns. The EMA is advanced lazily on read, one step per elapsed
// 5-second interval (the classic calc_load cadence).
#define GUEST_LOAD_FSHIFT 11
#define GUEST_LOAD_FIXED_1 (1u << GUEST_LOAD_FSHIFT)
void get_guest_loadavg(uint64_t out[3]) {
    static const unsigned exp[3] = {1884, 2014, 2037}; // 1/exp(5s/{1,5,15}min) in FIXED_1
    static lock_t load_lock = LOCK_INITIALIZER;
    static uint64_t load[3];
    static time_t last_sec;

    // Runnable tasks = alive minus io-blocked, excluding this reader itself.
    long active = (long) get_count_of_alive_tasks() - (long) get_count_of_blocked_tasks() - 1;
    if (active < 0)
        active = 0;
    struct timespec now = timespec_now(CLOCK_MONOTONIC);

    lock(&load_lock, 0);
    if (last_sec == 0)
        last_sec = now.tv_sec;
    long steps = (now.tv_sec - last_sec) / 5;
    if (steps > 0) {
        long do_steps = steps > 64 ? 64 : steps;
        for (long s = 0; s < do_steps; s++)
            for (int i = 0; i < 3; i++)
                load[i] = (load[i] * exp[i] +
                           (uint64_t) active * GUEST_LOAD_FIXED_1 * (GUEST_LOAD_FIXED_1 - exp[i]))
                          >> GUEST_LOAD_FSHIFT;
        last_sec = steps > 64 ? now.tv_sec : last_sec + steps * 5;
    }
    for (int i = 0; i < 3; i++)
        out[i] = load[i] << (16 - GUEST_LOAD_FSHIFT);
    unlock(&load_lock);
}

struct task *task_create_(struct task *parent) {
    struct task *task = malloc(sizeof(struct task));
    if (task == NULL)
        return NULL;

    *task = (struct task) {};
    if (parent != NULL)
        *task = *parent;
    else {
        // Treat init/root as starting with the full Linux capability set so
        // guest helpers such as setpriv can drop or reshuffle capabilities
        // without tripping over uninitialized state.
        task->abi = GUEST_ABI_I386;
        task->cap_effective[0] = task->cap_effective[1] = UINT32_MAX;
        task->cap_permitted[0] = task->cap_permitted[1] = UINT32_MAX;
        task->cap_inheritable[0] = task->cap_inheritable[1] = UINT32_MAX;
    }
    list_init(&task->group_links);
    list_init(&task->children);
    list_init(&task->siblings);
    list_init(&task->ptracees);
    list_init(&task->ptrace_siblings);
    task->pending = 0;
    task->waiting = 0;
    list_init(&task->queue);
    task->saved_mask = 0;
    task->has_saved_mask = false;
    task->clear_tid = 0;
    task->robust_list = 0;
    task->pdeath_signal = 0;
    task->did_exec = false;
    task->exit_code = 0;
    task->zombie = false;
    task->exiting = false;
    task->io_block = false;
    task->vfork = NULL;
    task->exit_signal = 0;

    lock_init(&task->general_lock, "task_creat_gen\0");

    task->sockrestart = (struct task_sockrestart) {};
    list_init(&task->sockrestart.listen);

    task->waiting_cond = NULL;
    task->waiting_lock = NULL;
    task->waiting_interrupt_flag = NULL;
    task->wait_interrupted = false;
    task->restart_interrupted_syscall = false;
    lock_init(&task->waiting_cond_lock, "task_creat_wait\0");
    cond_init(&task->pause);

    task->ptrace = (typeof(task->ptrace)) {};
    lock_init(&task->ptrace.lock, "task_creat_ptr\0");
    cond_init(&task->ptrace.cond);

    task->locks_held.count = 0;
    pthread_mutex_init(&task->locks_held.lock, NULL);
    atomic_store_explicit(&task->reference.count, 0, memory_order_relaxed);
    task->reference.ready_to_be_freed = false;

    complex_lockt(&pids_lock, 0);
    do {
        last_allocated_pid++;
        if (last_allocated_pid > MAX_PID) last_allocated_pid = 1;
    } while (!pid_empty(&pids[last_allocated_pid]));
    struct pid *pid = &pids[last_allocated_pid];
    pid->id = last_allocated_pid;
    list_init(&pid->alive);
    list_init(&pid->session);
    list_init(&pid->pgroup);
    task->pid = pid->id;

    pid->task = task;
    list_add(&alive_pids_list, &pid->alive);
    if (parent != NULL) {
        task->parent = parent;
        list_add(&parent->children, &task->siblings);
    }
    unlock(&pids_lock);
    return task;
}

// We consolidate the check for whether the task is in a critical section,
// holds locks, or has pending signals into a single function.
bool should_wait(struct task *t) {
    return task_ref_cnt_get(t, 0) > 1 || locks_held_count(t) || !!(t->pending & ~t->blocked);
}

void task_unlink_locked(struct task *task) {
    task->exiting = true;
    list_remove(&task->siblings);
    list_remove_safe(&task->ptrace_siblings);
    struct pid *pid = pid_get(task->pid);
    pid->task = NULL;
    list_remove(&pid->alive);
}

static void task_free_final(struct task *task) {
    if (task != NULL && task_is_leader(task) && task->group != NULL) {
        cond_destroy(&task->group->child_exit);
        free(task->group);
        task->group = NULL;
    }
    free(task);
}

void task_destroy_unlinked(struct task *task, int caller) {
    task->exiting = true;

    // We use a single loop to wait for the task to be ready to destroy.
    // This loop replaces all the similar while-loops in the original code.
    // Reap paths should not stall a waiting parent just to synchronously free
    // the task object. If references are still draining, defer cleanup.
    int count = caller == 2 ? 0 : -4000; // Counter to limit the number of times we check.
    while (count < 0 && should_wait(task)) {
        nanosleep(&lock_pause, NULL); // Sleep for a defined amount of time.
        count++;
    }

    if (task_ref_cnt_get(task, 1)) { // Check to see if another thread is accessing this process.  If yes, note that and defer freeing it
        struct task_pending_deletion *pd = malloc(sizeof(struct task_pending_deletion));
        if (pd) {
            task->reference.ready_to_be_freed = true;
            pd->task = task;
            pd->added_time = time(NULL);
            list_init(&pd->list);
            pthread_mutex_lock(&tasks_pending_deletion_lock);
            list_add(&tasks_pending_deletion_queue, &pd->list);
            pthread_mutex_unlock(&tasks_pending_deletion_lock);
        }
        // Lets cleanup any expired pending deletions here for now
        cleanup_pending_deletions();
        return;
    } else {
        task_free_final(task);
    }
}

void task_destroy(struct task *task, int caller) {
    task_unlink_locked(task);
    unlock(&pids_lock);
    task_destroy_unlinked(task, caller);
    complex_lockt(&pids_lock, 0);
}

// Cleanup function to delete tasks after the grace period
void cleanup_pending_deletions(void) {
    pthread_mutex_lock(&tasks_pending_deletion_lock);
    struct task_pending_deletion *pd, *tmp;
    list_for_each_entry_safe(&tasks_pending_deletion_queue, pd, tmp, list) {
        if (difftime(time(NULL), pd->added_time) >= GRACE_PERIOD &&
                atomic_load_explicit(&pd->task->reference.count, memory_order_acquire) == 0) { // Delete reaped tasks old and no longer referenced
            task_free_final(pd->task);
            list_remove(&pd->list);
            free(pd);
        }
    }
    pthread_mutex_unlock(&tasks_pending_deletion_lock);
}

void run_at_boot(void) {  // Stuff we run only once, at boot time.
    //atomic_thread_fence(__ATOMIC_SEQ_CST);
    struct uname uts;
    do_uname(&uts);
    unsigned short ncpu = get_cpu_count();
    lock_init(&pids_lock, "pids");
    lock_init(&block_lock, "block");
    lock_init(&atomic_l_lock, "run_at_boot");
    printk("iSH-AOK %s built %s %s booted on %d emulated x86_64 CPU(s)\n",
            uts.release, __DATE__, __TIME__, ncpu);
    // Get boot time
    extern time_t boot_time;
         
    boot_time = time(NULL);
    //printk("Seconds since January 1, 1970 = %ld\n", boot_time);
}

void task_poke_shared_mem(struct task *task, struct mem *mem) {
    if (task == NULL || mem == NULL)
        return;

    if (trylock(&pids_lock) != 0)
        return;
    struct pid *pid_entry;
    list_for_each_entry(&alive_pids_list, pid_entry, alive) {
        struct task *other = pid_entry->task;
        if (other == NULL || other == task)
            continue;
        if (other->mem != mem)
            continue;
        if (other->zombie || other->exiting)
            continue;
        pthread_kill(other->thread, SIGUSR1);
        if (other->cpu.poked_ptr == NULL)
            continue;
        cpu_poke(&other->cpu);
    }
    unlock(&pids_lock);
}

static void task_wait_for_mem_quiesce(struct task *task) {
    struct mem *mem = task != NULL ? task->mem : NULL;
    while (mem != NULL &&
           atomic_load_explicit(&mem->quiesce_requested, memory_order_acquire) > 0) {
        nanosleep(&lock_pause, NULL);
    }
}

void task_run_current(void) {
    struct task* save = current; // Because I kinda suspect that current gets messed up sometimes
    struct cpu_state *cpu = &save->cpu;
    struct tlb tlb = {};
    tlb_refresh(&tlb, &save->mem->mmu);
    
    while (true) {
        task_wait_for_mem_quiesce(save);
        read_lock(&save->mem->lock);

        qword_t amd64_rip_before = cpu->amd64_rip;
        int interrupt = cpu_run_to_interrupt(cpu, &tlb);
        if (save->abi == GUEST_ABI_AMD64 && strcmp(save->comm, "apk") == 0 &&
                (amd64_rip_before == 0 || cpu->amd64_rip == 0)) {
            printk("[amd64-jit] task loop apk int=%d rip=%#llx->%#llx eip=%#x rsp=%#llx rax=%#llx rcx=%#llx\n",
                   interrupt,
                   (unsigned long long) amd64_rip_before,
                   (unsigned long long) cpu->amd64_rip,
                   cpu->eip,
                   (unsigned long long) cpu->amd64_regs[amd64_rsp],
                   (unsigned long long) cpu->amd64_regs[amd64_rax],
                   (unsigned long long) cpu->amd64_regs[amd64_rcx]);
        }

        read_unlock(&save->mem->lock);
        jit_cleanup_jetsam_after_interrupt(cpu);
 
        handle_interrupt(interrupt);
    }
}

static void *task_thread(void *task) {
    sigset_t sigusr1;
    sigemptyset(&sigusr1);
    sigaddset(&sigusr1, SIGUSR1);
    pthread_sigmask(SIG_UNBLOCK, &sigusr1, NULL);

    current = task;
    
    update_thread_name();
    
    task_run_current();
    die("task_thread returned"); // above function call should never return
    return NULL;
}

static pthread_attr_t task_thread_attr;
__attribute__((constructor)) static void create_attr(void) {
    pthread_attr_init(&task_thread_attr);
    pthread_attr_setdetachstate(&task_thread_attr, PTHREAD_CREATE_DETACHED);
#if defined(__APPLE__)
    // Run emulated guest threads one QoS band below the UI thread
    // (USER_INTERACTIVE). A multi-threaded guest workload spawns one OS thread
    // per emulated CPU (e.g. `go run`/`go build` with GOMAXPROCS = ncpu); left
    // at the default priority those threads are CPU-bound at the same band as
    // the main thread and starve the terminal/UI, making the app unresponsive
    // for the duration of the burst. USER_INITIATED still runs guest work
    // promptly on the performance cores but lets the UI preempt it.
    pthread_attr_set_qos_class_np(&task_thread_attr, QOS_CLASS_USER_INITIATED, 0);
#endif
}

void task_start(struct task *task) {
    if (pthread_create(&task->thread, &task_thread_attr, task_thread, task) < 0)
        die("could not create thread");
}

int_t sys_sched_yield(void) {
    STRACE("sched_yield()");
    sched_yield();
    return 0;
}

void update_thread_name(void) {
    char name[16]; // Maximum length for thread names in many systems, including Linux
    int result;

    // Ensure that the name buffer is always null-terminated
    memset(name, 0, sizeof(name));

    // Create the thread name with PID
    //result = snprintf(name, sizeof(name) - 1, "%s-%d", current->comm, current->pid);
    result = snprintf(name, sizeof(name) - 1, "%.7s-%d", current->comm, current->pid);

    // Check if the output was truncated
    if (result >= (int)sizeof(name)) {
        // Handle truncation (e.g., by logging, adjusting the name format, etc.)
        // For this example, we just log a warning
        printk("WARNING: Thread name truncated in update_thread_name(%s).\n", name);
    }

#if __APPLE__
    pthread_setname_np(name);
#else
    pthread_setname_np(pthread_self(), name);
#endif
}

inline void modify_locks_held_count(struct task *task, int value) { // value should only be -1 or 1.
    if ((task == NULL) && (current != NULL)) {
        task = current;
    } else if (task == NULL) {
        return;
    }

    if (value != 1 && value != -1) {
        printk("ERROR: invalid locks_held delta %d for %s:%d\n",
               value, task->comm, task->pid);
        return;
    }

    int old_count;
    int new_count;
    do {
        old_count = __atomic_load_n(&task->locks_held.count, __ATOMIC_RELAXED);
        new_count = old_count + value;
        if (new_count < 0 && task->pid > 9) {
            printk("ERROR: Attempt to decrement locks_held count below zero, ignoring\n");
            return;
        }
    } while (!__atomic_compare_exchange_n(&task->locks_held.count, &old_count, new_count,
                                          true, __ATOMIC_RELAXED, __ATOMIC_RELAXED));

    if (new_count < 0) {
        __atomic_store_n(&task->locks_held.count, 0, __ATOMIC_RELAXED);
        printk("ERROR: Attempt to decrement locks_held count below zero, ignoring\n");
    }
}

bool current_is_valid(void) {
    if(current != NULL)
        return true;
    
    return false;
}
