#include <pthread.h>
#include <signal.h>
#include <string.h>
#include "kernel/calls.h"
#include "kernel/mm.h"
#include "kernel/futex.h"
#include "kernel/ptrace.h"
#include "kernel/task.h"
#include "util/sync.h"
#include "fs/fd.h"
#include "fs/tty.h"

extern bool doEnableExtraLocking;
extern pthread_mutex_t extra_lock;
extern dword_t extra_lock_pid;
extern const char extra_lock_comm;

static void halt_system_locked(void);

static bool trace_session_exit_task(struct task *task) {
    return strcmp(task->comm, "login") == 0 ||
        strcmp(task->comm, "sshd") == 0 ||
        strcmp(task->comm, "sh") == 0 ||
        strcmp(task->comm, "bash") == 0 ||
        strcmp(task->comm, "dash") == 0 ||
        strcmp(task->comm, "getty") == 0 ||
        strcmp(task->comm, "agetty") == 0;
}

static void amd64_decode_wait_status_exit(int status, char *buf, size_t size) {
    if (size == 0)
        return;
    if ((status & 0xff) == 0x7f) {
        snprintf(buf, size, "stopped sig=%d status=%#x", (status >> 8) & 0xff, status);
        return;
    }
    if ((status & 0x7f) == 0) {
        snprintf(buf, size, "exited code=%d status=%#x", (status >> 8) & 0xff, status);
        return;
    }
    snprintf(buf, size, "signaled sig=%d core=%d status=%#x",
             status & 0x7f, (status & 0x80) != 0, status);
}

static bool amd64_trace_task_or_parent_lineage(struct task *task) {
    if (task == NULL)
        return false;
    if (amd64_trace_is_lineage_tgid(task->tgid))
        return true;
    return task->parent != NULL && amd64_trace_is_lineage_tgid(task->parent->tgid);
}

// Removes a task from its thread group. The caller is responsible for ensuring
// the task is quiescent before taking pids_lock and calling this helper.
static bool exit_tgroup(struct task *task) {
    struct tgroup *group = task->group;
    list_remove(&task->group_links);
    bool group_dead = list_empty(&group->threads);
    if (group_dead) {
        // don't need to lock the group since the only pointers to it come from:
        // - other threads' current->group, but there are none left thanks to that list_empty call
        // - locking pids_lock first, which do_exit did
        if (group->itimer)
            timer_free(group->itimer);

        // The group will be removed from its group and session by reap_if_zombie,
        // because fish tries to set the pgid to that of an exited but not reaped
        // task.
        // https://github.com/Microsoft/WSL/issues/2786
    }
    return group_dead;
}

// A function pointer that can be assigned to a cleanup function to be called upon task exit.
void (*exit_hook)(struct task *task, int code) = NULL;

static inline bool exit_wait_needed(struct task *task) {
    return task_ref_cnt_get(task, 0) > 2 || locks_held_count(task);
}

// Finds a new parent for the children of a task that is exiting. If no suitable parent
// is found within the task's group, it returns the 'init' task.
static struct task *find_new_parent(struct task *task) {
    struct task *new_parent;
    list_for_each_entry(&task->group->threads, new_parent, group_links) {
        if (!new_parent->exiting)
            return new_parent;
    }
    return pid_get_task(1);
}

static bool session_has_other_live_groups(struct pid *sid_pid, struct tgroup *group) {
    struct tgroup *session_group;
    list_for_each_entry(&sid_pid->session, session_group, session) {
        if (session_group != group && !list_empty(&session_group->threads))
            return true;
    }
    return false;
}

static void ptrace_detach_from_tracer(struct task *tracer, struct task *tracee) {
    bool traced_by_tracer = tracee->ptrace.tracer == tracer ||
        (tracee->ptrace.traced && tracee->ptrace.tracer == NULL && tracee->parent == tracer);
    if (!traced_by_tracer)
        return;

    lock(&tracee->ptrace.lock, 0);
    traced_by_tracer = tracee->ptrace.tracer == tracer ||
        (tracee->ptrace.traced && tracee->ptrace.tracer == NULL && tracee->parent == tracer);
    if (traced_by_tracer) {
        tracee->ptrace.traced = false;
        tracee->ptrace.tracer = NULL;
        tracee->ptrace.stop_at_syscall = false;
        tracee->ptrace.syscall_stopped = false;
        tracee->ptrace.signal = 0;
        tracee->ptrace.trap_event = 0;
        tracee->ptrace.eventmsg = 0;
        if (tracee->ptrace.stopped) {
            tracee->ptrace.stopped = false;
            notify(&tracee->ptrace.cond);
        }
    }
    unlock(&tracee->ptrace.lock);

    list_remove_safe(&tracee->ptrace_siblings);
}

// Hang up the controlling terminal as soon as the session leader exits, but
// only once the session is otherwise empty. Waiting until the zombie is reaped
// is too late for PTY users such as script, but hanging up while sshd still
// has a child shell running tears down the remote login immediately.
static void exit_hangup_session_tty(struct task *leader) {
    struct tgroup *group = leader->group;
    if (group->tty == NULL || group->sid != leader->pid)
        return;

    struct pid *sid_pid = pid_get(group->sid);
    if (sid_pid == NULL)
        return;
    if (session_has_other_live_groups(sid_pid, group))
        return;

    struct tty *tty = group->tty;
    lock(&ttys_lock, 0);
    lock(&tty->lock, 0);
    tty->session = 0;
    tty->fg_group = 0;
    tty_hangup(tty);
    unlock(&tty->lock);

    struct tgroup *session_group;
    list_for_each_entry(&sid_pid->session, session_group, session) {
        lock(&session_group->lock, 0);
        if (session_group->tty == tty) {
            session_group->tty = NULL;
            unlock(&session_group->lock);
            tty_release(tty);
        } else {
            unlock(&session_group->lock);
        }
    }
    unlock(&ttys_lock);
}

// Handles the termination of the current task. It releases resources, notifies the parent,
// and re-parents any children. It ensures the task is not in a critical section and that
// all locks are released before proceeding.  At least in theory
noreturn void do_exit(struct task *task, int status) {
    if(task->reference.ready_to_be_freed) {
        goto EXIT;
    } else {
        task->exiting = true;
    }

    if (trace_session_exit_task(task)) {
        printk("INFO: exit session pid=%d tgid=%d comm=%s status=%#x did_exec=%d parent=%d\n",
               task->pid, task->tgid, task->comm, status, task->did_exec,
               task->parent != NULL ? task->parent->pid : -1);
    }

    lock(&task->general_lock, 0);
    
    // has to happen before mm_release
    while (exit_wait_needed(task)) { // Wait for other references and locks, but ignore extra pending signals while exiting.
        nanosleep(&lock_pause, NULL);
    }
    guest_addr_t clear_tid = task->clear_tid;
    if (clear_tid) {
        pid_t_ zero = 0;
        if (user_put(clear_tid, zero) == 0)
            futex_wake(clear_tid, 1);
    }

    // release all our resources
    do {
        nanosleep(&lock_pause, NULL);
        nanosleep(&lock_pause, NULL);
    } while (exit_wait_needed(task)); // Wait for now, task is in one or more critical
    mm_release(task->mm);
    task->mm = NULL;
    task->mem = NULL;
    task->cpu.mmu = NULL;
    
    while (exit_wait_needed(task)) { // Wait for now, task is in one or more critical sections, and/or has locks.
        nanosleep(&lock_pause, NULL);
    }
    fdtable_release(task->files);
    task->files = NULL;
    
    while (exit_wait_needed(task)) { // Wait for now, task is in one or more critical sections, and/or has locks.
        nanosleep(&lock_pause, NULL);
    }
    fs_info_release(task->fs);
    task->fs = NULL;
    // sighand must be released below so it can be protected by pids_lock
    // since it can be accessed by other threads

    while (exit_wait_needed(task)) { // Wait for now, task is in one or more critical sections, and/or has locks.
        nanosleep(&lock_pause, NULL);
    }
    // save things that our parent might be interested in
    task->exit_code = status; // FIXME locking
    if (amd64_trace_task_or_parent_lineage(task)) {
        char decoded[64];
        amd64_decode_wait_status_exit(status, decoded, sizeof(decoded));
        printk("tracked exit: pid=%d tgid=%d abi=%d comm=%s parent=%d parent_tgid=%d did_exec=%d %s\n",
               task->pid, task->tgid, task->abi, task->comm,
               task->parent != NULL ? task->parent->pid : -1,
               task->parent != NULL ? task->parent->tgid : -1,
               task->did_exec, decoded);
    }
    struct rusage_ rusage = rusage_get_current();
    lock(&task->group->lock, 0);
    rusage_add(&task->group->rusage, &rusage);
    struct rusage_ group_rusage = task->group->rusage;
    unlock(&task->group->lock);

    // the actual freeing needs pids_lock
    // release the sighand
    while (exit_wait_needed(task)) { // We added one to the task reference count above, thus the check is 2, in case any other thread is accessing.
        nanosleep(&lock_pause, NULL);
    }

    struct task *signal_parent = NULL;
    struct siginfo_ signal_info = {};
    int signal_no = 0;

    // Only hold pids_lock for the process-tree and thread-group teardown below.
    // Holding it across mm/files/fs release and the wait loops above wedges task
    // creation and other global process operations behind a slow exit path.
    complex_lockt(&pids_lock, 0);

    sighand_release(task->sighand);
    task->sighand = NULL;
    struct sigqueue *sigqueue, *sigqueue_tmp;
    list_for_each_entry_safe(&task->queue, sigqueue, sigqueue_tmp, queue) {
        list_remove(&sigqueue->queue);
        free(sigqueue);
    }
    
    struct task *leader = task->group->leader;

    // reparent children
    struct task *new_parent = find_new_parent(task);
    struct task *child, *tmp;
    
    list_for_each_entry_safe(&task->children, child, tmp, siblings) {
        ptrace_detach_from_tracer(task, child);
        child->parent = new_parent;
        list_remove(&child->siblings);
        list_add(&new_parent->children, &child->siblings);
    }
    list_for_each_entry_safe(&task->ptracees, child, tmp, ptrace_siblings)
        ptrace_detach_from_tracer(task, child);
    if (exit_tgroup(task)) {
        exit_hangup_session_tty(leader);
        // notify parent that we died
        struct task *parent = leader->parent;
        if (parent == NULL) {
            // init died
            halt_system_locked();
        } else {
            task_ref_cnt_mod(parent, 1);
            signal_parent = parent;
            signal_no = leader->exit_signal;
            lock(&parent->general_lock, 0);
            leader->zombie = true;
            notify(&parent->group->child_exit);
            signal_info = (struct siginfo_) {
                .code = SI_KERNEL_,
                .child.pid = task->pid,
                .child.uid = task->uid,
                .child.status = task->exit_code,
                .child.utime = clock_from_timeval(group_rusage.utime),
                .child.stime = clock_from_timeval(group_rusage.stime),
            };
            unlock(&parent->general_lock);
        }

        if (exit_hook != NULL)
            exit_hook(task, status);
    }

    vfork_notify(task);
    
    if(task != leader) {
        task_destroy(task, 1);
    } else {
        unlock(&task->general_lock);
    }
    
    unlock(&pids_lock);

    if (signal_parent != NULL) {
        if (signal_no != 0)
            send_signal(signal_parent, signal_no, signal_info);
        task_ref_cnt_mod(signal_parent, -1);
    }
    
EXIT:pthread_exit(NULL);
}

// Exits all tasks in the current task's thread group and then calls do_exit to terminate
// the current task itself.
noreturn void do_exit_group(int status) {
    struct tgroup *group = current->group;
    complex_lockt(&pids_lock, 0);
    lock(&group->lock, 0);
    if (amd64_trace_is_lineage_tgid(current->tgid)) {
        printk("tracked exit_group begin: current=%d tgid=%d abi=%d status=%#x threads=%lu doing=%d\n",
               current->pid, current->tgid, current->abi, status,
               list_size(&group->threads), group->doing_group_exit);
    }
    if (!group->doing_group_exit) {
        group->doing_group_exit = true;
        group->group_exit_code = status;
    } else {
        status = group->group_exit_code;
    }

    // kill everyone else in the group
    struct task *task;
    task_ref_cnt_mod(current, 1);
    list_for_each_entry(&group->threads, task, group_links) {
        if (amd64_trace_is_lineage_tgid(current->tgid)) {
            printk("tracked exit_group member: current=%d target=%d tgid=%d exiting=%d zombie=%d io_block=%d pending=%#llx blocked=%#llx self=%d\n",
                   current->pid, task->pid, task->tgid, task->exiting, task->zombie,
                   task->io_block,
                   (unsigned long long) task->pending,
                   (unsigned long long) task->blocked,
                   task == current);
        }
        if (task != current) {
            deliver_signal(task, SIGKILL_, SIGINFO_NIL);
            task->group->stopped = false;
            notify(&task->group->stopped_cond);
        }
    }

    unlock(&pids_lock);
    unlock(&group->lock);
    if(current->pid <= MAX_PID)
        do_exit(current, status);
    
    task_ref_cnt_mod(current, -1);
    unlock(&pids_lock);  // Shouldn't get here
    pthread_exit(NULL);
}

// always called from init process. Intended to be called when the init process exits.
static void halt_system_locked(void) {
    // brutally murder everything
    // which will leave everything in an inconsistent state. I will solve this problem later.
    for (int i = 2; i < MAX_PID; i++) {
        struct task *task = pid_get_task(i);
        if (task != NULL)
            pthread_kill(task->thread, SIGKILL);
    }

    // unmount all filesystems
    lock(&mounts_lock, 0);
    struct mount *mount, *tmp;
    list_for_each_entry_safe(&mounts, mount, tmp, mounts) {
        mount_remove(mount);
    }
    unlock(&mounts_lock);
}

dword_t sys_exit(dword_t status) {
    STRACE("exit(%d)\n", status);
    do_exit(current, status << 8);
}

dword_t sys_exit_group(dword_t status) {
    STRACE("exit_group(%d)\n", status);
    do_exit_group(status << 8);
}

#define WNOHANG_ (1 << 0)
#define WUNTRACED_ (1 << 1)
#define WEXITED_ (1 << 2)
#define WCONTINUED_ (1 << 3)
#define WNOWAIT_ (1 << 24)
#define __WALL_ (1 << 30)

#define P_ALL_ 0
#define P_PID_ 1
#define P_PGID_ 2

// returns false if the task cannot be reaped and true if the task was reaped
static bool reap_if_zombie(struct task *task, struct siginfo_ *info_out, struct rusage_ *rusage_out, int options) {
    if (!task->zombie)
        return false;
    lock(&task->group->lock, 0);

    dword_t exit_code = task->exit_code;
    if (task->group->doing_group_exit)
        exit_code = task->group->group_exit_code;
    info_out->child.status = exit_code;

    struct rusage_ rusage = task->group->rusage;
    if (!(options & WNOWAIT_)) {
        lock(&current->group->lock, 0);
        rusage_add(&current->group->children_rusage, &rusage);
        unlock(&current->group->lock);
    }
    if (rusage_out != NULL)
        *rusage_out = rusage;

    unlock(&task->group->lock);

    // WNOWAIT means don't destroy the child, instead leave it so it could be waited for again.
    if (options & WNOWAIT_)
        return true;

    // tear down group
    cond_destroy(&task->group->child_exit);
    task_leave_session(task);
    list_remove(&task->group->pgroup);
    free(task->group);

    task_destroy(task, 2);
    return true;
}


static bool notify_if_stopped(struct task *task, struct siginfo_ *info_out) {
    complex_lockt(&task->group->lock, 0);
    bool stopped = task->group->stopped;
    unlock(&task->group->lock);
    if (!stopped || task->group->group_exit_code == 0)
        return false;
    dword_t exit_code = task->group->group_exit_code;
    task->group->group_exit_code = 0;
    info_out->child.status = exit_code;
    return true;
}

static bool notify_if_ptrace_stopped(struct task *task, struct siginfo_ *info_out) {
    lock(&task->ptrace.lock, 0);
    if (task->ptrace.stopped && task->ptrace.signal) {
        info_out->child.status = task->ptrace.trap_event << 16 | task->ptrace.signal << 8 | 0x7f;
        task->ptrace.signal = 0;
        task->ptrace.trap_event = 0;
        task->ptrace.eventmsg = 0;
        unlock(&task->ptrace.lock);
        return true;
    }
    unlock(&task->ptrace.lock);
    return false;
}

static bool reap_if_needed(struct task *task, struct siginfo_ *info_out, struct rusage_ *rusage_out, int options) {
    assert(task_is_leader(task));
    if ((options & WUNTRACED_ && notify_if_stopped(task, info_out)) ||
        (options & WEXITED_ && reap_if_zombie(task, info_out, rusage_out, options))) {
        info_out->sig = SIGCHLD_;
        return true;
    }
    if (notify_if_ptrace_stopped(task, info_out))
        return true;
    return false;
}

int do_wait(int idtype, pid_t_ id, struct siginfo_ *info, struct rusage_ *rusage, int options) {
    if (idtype != P_ALL_ && idtype != P_PID_ && idtype != P_PGID_)
        return _EINVAL;
    if (options & ~(WNOHANG_|WUNTRACED_|WEXITED_|WCONTINUED_|WNOWAIT_|__WALL_))
        return _EINVAL;

    complex_lockt(&pids_lock, 0);
    int err;
    bool got_signal = false;

retry:
        if (idtype != P_PID_) {
            // look for a zombie child
            bool no_children = true;
            struct task *parent;
            list_for_each_entry(&current->group->threads, parent, group_links) {
            struct task *task;
            list_for_each_entry(&current->children, task, siblings) {
                if (!task_is_leader(task))
                    continue;
                if (idtype == P_PGID_ && task->group->pgid != id)
                    continue;
                no_children = false;
                info->child.pid = task->pid;
                if (reap_if_needed(task, info, rusage, options))
                    goto found_something;
            }
            list_for_each_entry(&current->ptracees, task, ptrace_siblings) {
                if (!task_is_leader(task))
                    continue;
                no_children = false;
                info->child.pid = task->pid;
                if (notify_if_ptrace_stopped(task, info)) {
                    info->sig = SIGCHLD_;
                    goto found_something;
                }
            }
        }
        err = _ECHILD;
        if (no_children)
            goto error;
    } else {
        // check if this child is a zombie
        struct task *task = pid_get_task_zombie(id);
        err = _ECHILD;
        if (task == NULL)
            goto error;
        task = task->group->leader;
        info->child.pid = id;
        bool is_child = task->parent != NULL && task->parent->group == current->group;
        bool is_ptrace_child = task->ptrace.tracer != NULL && task->ptrace.tracer->group == current->group;
        if (!is_child && !is_ptrace_child)
            goto error;
        if (is_ptrace_child && notify_if_ptrace_stopped(task, info)) {
            info->sig = SIGCHLD_;
            goto found_something;
        }
        if (reap_if_needed(task, info, rusage, options))
            goto found_something;
    }

    // WNOHANG leaves the info in an implementation-defined state. set the pid
    // to 0 so wait4 can pass that along correctly.
    info->child.pid = 0;
    if (options & WNOHANG_) {
        info->sig = SIGCHLD_;
        goto found_something;
    }

    err = _EINTR;
    if (got_signal)
        goto error;

    // no matching zombie found, wait for one
    if (wait_for(&current->group->child_exit, &pids_lock, NULL)) {
        // maybe we got a SIGCHLD! go through the loop one more time to make
        // sure the newly exited process is returned in that case.
        got_signal = true;
        goto retry;
    }
    goto retry;

    info->sig = SIGCHLD_;
found_something:
    if (amd64_trace_is_lineage_tgid(current->tgid)) {
        char decoded[64];
        amd64_decode_wait_status_exit(info->child.status, decoded, sizeof(decoded));
        printk("amd64 tracked reap: pid=%d tgid=%d abi=%d comm=%s child=%d %s options=%#x\n",
               current->pid, current->tgid, current->abi, current->comm,
               info->child.pid, decoded, options);
    }
    unlock(&pids_lock);
    return 0;

error:
    unlock(&pids_lock);
    return err;
}

dword_t sys_waitid(int_t idtype, pid_t_ id, addr_t info_addr, int_t options) {
    return sys_waitid_guest(idtype, id, info_addr, options);
}

dword_t sys_waitid_guest(int_t idtype, pid_t_ id, guest_addr_t info_addr, int_t options) {
    STRACE("waitid(%d, %d, %#x, %#x)", idtype, id, info_addr, options);
    struct siginfo_ info = {};
    int_t res = 0;
    TASK_MAY_BLOCK {
        res = do_wait(idtype, id, &info, NULL, options);
    }
    if (res < 0 || (res == 0 && info.child.pid == 0))
        return res;
    if (info_addr != 0 && siginfo_to_user(current, info_addr, &info))
        return _EFAULT;
    return 0;
}

dword_t sys_wait4(pid_t_ id, addr_t status_addr, dword_t options, addr_t rusage_addr) {
    return sys_wait4_guest(id, status_addr, options, rusage_addr);
}

dword_t sys_wait4_guest(pid_t_ id, guest_addr_t status_addr, dword_t options, guest_addr_t rusage_addr) {
    STRACE("wait4(%d, %#x, %#x, %#x)", id, status_addr, options, rusage_addr);
    if (options & WNOWAIT_)
        return _EINVAL;

    int idtype;
    if (id > 0)
        idtype = P_PID_;
    else if (id == -1)
        idtype = P_ALL_;
    else {
        idtype = P_PGID_;
        if (id == 0)
            id = current->group->pgid;
        else
            id = -id;
    }

    struct siginfo_ info = {.child.pid = 0xbaba};
    struct rusage_ rusage;
    int_t res = 0;
    TASK_MAY_BLOCK {
        res = do_wait(idtype, id, &info, &rusage, options | WEXITED_);
    }
    if (res < 0 || (res == 0 && info.child.pid == 0))
        return res;
    if (status_addr != 0 && user_put(status_addr, info.child.status))
        return _EFAULT;
    if (rusage_addr != 0 && write_guest_rusage_abi(current->abi, rusage_addr, &rusage))
        return _EFAULT;
    return info.child.pid;
}

dword_t sys_waitpid(pid_t_ pid, addr_t status_addr, dword_t options) {
    return sys_wait4(pid, status_addr, options, 0);
}
