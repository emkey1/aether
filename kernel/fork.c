#include "debug.h"
#include "kernel/task.h"
#include "fs/fd.h"
#include "fs/tty.h"
#include "kernel/calls.h"
#include "kernel/mm.h"
#include "kernel/ptrace.h"
#include "util/sync.h"
#include <string.h>

#define CSIGNAL_ 0x000000ff
#define CLONE_VM_ 0x00000100
#define CLONE_FS_ 0x00000200
#define CLONE_FILES_ 0x00000400
#define CLONE_SIGHAND_ 0x00000800
#define CLONE_PIDFD_ 0x00001000
#define CLONE_PTRACE_ 0x00002000
#define CLONE_VFORK_ 0x00004000
#define CLONE_PARENT_ 0x00008000
#define CLONE_THREAD_ 0x00010000
#define CLONE_NEWNS_ 0x00020000
#define CLONE_SYSVSEM_ 0x00040000
#define CLONE_SETTLS_ 0x00080000
#define CLONE_PARENT_SETTID_ 0x00100000
#define CLONE_CHILD_CLEARTID_ 0x00200000
#define CLONE_DETACHED_ 0x00400000
#define CLONE_UNTRACED_ 0x00800000
#define CLONE_CHILD_SETTID_ 0x01000000
#define CLONE_NEWCGROUP_ 0x02000000
#define CLONE_NEWUTS_ 0x04000000
#define CLONE_NEWIPC_ 0x08000000
#define CLONE_NEWUSER_ 0x10000000
#define CLONE_NEWPID_ 0x20000000
#define CLONE_NEWNET_ 0x40000000
#define CLONE_IO_ 0x80000000
#define IMPLEMENTED_FLAGS (CLONE_VM_|CLONE_FILES_|CLONE_FS_|CLONE_SIGHAND_|CLONE_SYSVSEM_|CLONE_VFORK_|CLONE_THREAD_|\
        CLONE_SETTLS_|CLONE_CHILD_SETTID_|CLONE_PARENT_SETTID_|CLONE_CHILD_CLEARTID_|CLONE_DETACHED_)

static bool trace_session_fork_name(const char *name) {
    return strcmp(name, "login") == 0 ||
        strcmp(name, "sshd") == 0 ||
        strcmp(name, "sh") == 0 ||
        strcmp(name, "bash") == 0 ||
        strcmp(name, "dash") == 0 ||
        strcmp(name, "getty") == 0 ||
        strcmp(name, "agetty") == 0;
}

static void trace_fork_tty(struct task *task, int *type_out, int *num_out) {
    int type = -1;
    int num = -1;
    lock(&task->group->lock, 0);
    struct tty *tty = task->group->tty;
    if (tty != NULL) {
        type = tty->type;
        num = tty->num;
    }
    unlock(&task->group->lock);
    if (type_out != NULL)
        *type_out = type;
    if (num_out != NULL)
        *num_out = num;
}

static struct tgroup *tgroup_copy(struct tgroup *old_group) {
    struct tgroup *group = malloc(sizeof(struct tgroup));
    *group = *old_group;
    list_init(&group->threads);
    list_add(&old_group->pgroup, &group->pgroup);
    list_add(&old_group->session, &group->session);
    if (group->tty) {
        lock(&group->tty->lock, 0);
        group->tty->refcount++;
        unlock(&group->tty->lock);
    }
    group->itimer = NULL;
    group->doing_group_exit = false;
    group->children_rusage = (struct rusage_) {};
    cond_init(&group->child_exit);
    cond_init(&group->stopped_cond);
    lock_init(&group->lock, "tgroup_copy\0");
    return group;
}

static int copy_task(struct task *task, dword_t flags, addr_t stack, addr_t ptid_addr, addr_t tls_addr, addr_t ctid_addr) {
    task->vfork = NULL;
    if (stack != 0)
        task->cpu.esp = stack;

    int err;
    struct mm *mm = task->mm;
    if (flags & CLONE_VM_) {
        mm_retain(mm);
    } else {
        task_set_mm(task, mm_copy(mm));
    }

    if (flags & CLONE_FILES_) {
        task->files->refcount++;
    } else {
        task->files = fdtable_copy(task->files);
        if (IS_ERR(task->files)) {
            err = (int)PTR_ERR(task->files);
            goto fail_free_mem;
        }
    }

    err = _ENOMEM;
    if (flags & CLONE_FS_) {
        task->fs->refcount++;
    } else {
        task->fs = fs_info_copy(task->fs);
        if (task->fs == NULL)
            goto fail_free_files;
    }

    if (flags & CLONE_SIGHAND_) {
        task->sighand->refcount++;
    } else {
        task->sighand = sighand_copy(task->sighand);
        if (task->sighand == NULL)
            goto fail_free_fs;
    }

    struct tgroup *old_group = task->group;
    complex_lockt(&pids_lock, 0);
    lock(&old_group->lock, 0);
    if (!(flags & CLONE_THREAD_)) {
        task->group = tgroup_copy(old_group);
        task->group->leader = task;
        task->tgid = task->pid;
    } else {
        // New threads do not inherit the parent's alternate signal stack
        task->altstack = 0;
        task->altstack_size = 0;
    }
    list_add(&task->group->threads, &task->group_links);
    unlock(&old_group->lock);
    unlock(&pids_lock);

    if (flags & CLONE_SETTLS_) {
        err = task_set_thread_area(task, tls_addr);
        if (err < 0)
            goto fail_free_sighand;
    }

    err = _EFAULT;
    if (flags & CLONE_CHILD_SETTID_)
        if (user_put_task(task, ctid_addr, task->pid))
            goto fail_free_sighand;
    if (flags & CLONE_PARENT_SETTID_)
        if (user_put(ptid_addr, task->pid))
            goto fail_free_sighand;
    if (flags & CLONE_CHILD_CLEARTID_)
        task->clear_tid = ctid_addr;
    task->exit_signal = flags & CSIGNAL_;

    // remember to do CLONE_SYSVSEM
    return 0;

fail_free_sighand:
    while(task_ref_cnt_get(task, 0)) { // Wait for now, task is in one or more critical sections
        nanosleep(&lock_pause, NULL);
    }
    sighand_release(task->sighand);
fail_free_fs:
    fs_info_release(task->fs);
fail_free_files:
    fdtable_release(task->files);
fail_free_mem:
    mm_release(task->mm);
    return err;
}

dword_t sys_clone(dword_t flags, addr_t stack, addr_t ptid, addr_t tls, addr_t ctid) {
    STRACE("clone(0x%x, 0x%x, 0x%x, 0x%x, 0x%x)", flags, stack, ptid, tls, ctid);
    if (flags & ~CSIGNAL_ & ~IMPLEMENTED_FLAGS) {
        FIXME("unimplemented clone flags 0x%x", flags & ~CSIGNAL_ & ~IMPLEMENTED_FLAGS);
        return _EINVAL;
    }
    if (flags & CLONE_SIGHAND_ && !(flags & CLONE_VM_))
        return _EINVAL;
    if (flags & CLONE_THREAD_ && !(flags & CLONE_SIGHAND_))
        return _EINVAL;

    char parent_comm[sizeof(current->comm)];
    lock(&current->general_lock, 0);
    strncpy(parent_comm, current->comm, sizeof(parent_comm));
    parent_comm[sizeof(parent_comm) - 1] = '\0';
    unlock(&current->general_lock);
    bool trace_fork = trace_session_fork_name(parent_comm);
    int tty_type = -1;
    int tty_num = -1;
    if (trace_fork)
        trace_fork_tty(current, &tty_type, &tty_num);

    struct task *task = task_create_(current);
    if (task == NULL)
        return _ENOMEM;
    int err = copy_task(task, flags, stack, ptid, tls, ctid);
    if (err < 0) {
        // FIXME: there is a window between task_create_ and task_destroy where
        // some other thread could get a pointer to the task.
        // FIXME: task_destroy doesn't free all aspects of the task, which
        // could cause leaks
        complex_lockt(&pids_lock, 0);
        task_destroy(task, 3);
        unlock(&pids_lock);
        
        return err;
    }
    task->cpu.eax = 0;

    struct vfork_info vfork;
    if (flags & CLONE_VFORK_) {
        lock_init(&vfork.lock, "sys_clone\0");
        cond_init(&vfork.cond);
        vfork.done = false;
        task->vfork = &vfork;
    }

    // task might be destroyed by the time we finish, so save the pid
    pid_t pid = task->pid;
    bool trace_child = false;
    if (current->ptrace.traced && !(flags & CLONE_UNTRACED_)) {
        dword_t trace_option = 0;
        if (flags & CLONE_VFORK_)
            trace_option = PTRACE_O_TRACEVFORK_;
        else if (flags & CLONE_THREAD_)
            trace_option = PTRACE_O_TRACECLONE_;
        else
            trace_option = PTRACE_O_TRACEFORK_;

        if (flags & CLONE_VFORK_)
            current->ptrace.trap_event = PTRACE_EVENT_VFORK_;
        else if (flags & CLONE_THREAD_)
            current->ptrace.trap_event = PTRACE_EVENT_CLONE_;
        else
            current->ptrace.trap_event = PTRACE_EVENT_FORK_;
        current->ptrace.eventmsg = pid;
        send_signal(current, SIGTRAP_, SIGINFO_NIL);

        if (current->ptrace.options & trace_option) {
            ptrace_attach_fork_child(task, current);
            trace_child = true;
        }
    }

    task_start(task);
    if (trace_child)
        send_signal(task, SIGSTOP_, SIGINFO_NIL);

    if (trace_fork) {
        lock(&task->general_lock, 0);
        char child_comm[sizeof(task->comm)];
        strncpy(child_comm, task->comm, sizeof(child_comm));
        child_comm[sizeof(child_comm) - 1] = '\0';
        unlock(&task->general_lock);
        printk("INFO: fork session parent=%d/%d(%s) child=%d/%d(%s) flags=%#x tty=%d:%d\n",
               current->pid, current->tgid, parent_comm,
               task->pid, task->tgid, child_comm, flags,
               tty_type, tty_num);
    }

    if (flags & CLONE_VFORK_) {
        lock(&vfork.lock, 0);
        while (!vfork.done)
            // FIXME this should stop waiting if a fatal signal is received
            wait_for_ignore_signals(&vfork.cond, &vfork.lock, NULL);
        unlock(&vfork.lock);
        lock(&task->general_lock, 0);
        task->vfork = NULL;
        unlock(&task->general_lock);
        cond_destroy(&vfork.cond);
    }

    return pid;
}

struct clone_args_ {
    qword_t flags;
    qword_t pidfd;
    qword_t child_tid;
    qword_t parent_tid;
    qword_t exit_signal;
    qword_t stack;
    qword_t stack_size;
    qword_t tls;
    qword_t set_tid;
    qword_t set_tid_size;
    qword_t cgroup;
};

dword_t sys_clone3(addr_t uargs_addr, dword_t size) {
    STRACE("clone3(%#x, %u)", uargs_addr, size);

    struct clone_args_ args = {};
    if (size < offsetof(struct clone_args_, tls) + sizeof(args.tls))
        return _EINVAL;
    if (user_read(uargs_addr, &args, size < sizeof(args) ? size : sizeof(args)))
        return _EFAULT;

    if ((args.flags >> 32) != 0)
        return _ENOSYS;
    if (args.pidfd != 0 || args.set_tid != 0 || args.set_tid_size != 0 || args.cgroup != 0)
        return _ENOSYS;

    dword_t flags = (dword_t) args.flags;
    dword_t exit_signal = (dword_t) args.exit_signal;
    if ((args.exit_signal >> 32) != 0)
        return _EINVAL;
    if ((flags & CSIGNAL_) != 0 && (flags & CSIGNAL_) != exit_signal)
        return _EINVAL;
    flags = (flags & ~CSIGNAL_) | exit_signal;

    if (flags & CLONE_PIDFD_)
        return _ENOSYS;

    if ((args.child_tid >> 32) != 0 || (args.parent_tid >> 32) != 0 || (args.tls >> 32) != 0)
        return _EINVAL;

    qword_t child_stack = args.stack;
    if (child_stack != 0 && args.stack_size != 0)
        child_stack += args.stack_size;
    if ((child_stack >> 32) != 0)
        return _EINVAL;

    return sys_clone(flags, (addr_t) child_stack, (addr_t) args.parent_tid,
        (addr_t) args.tls, (addr_t) args.child_tid);
}

dword_t sys_unshare(dword_t flags) {
    STRACE("unshare(%#x)", flags);

    const dword_t supported = CLONE_FILES_ | CLONE_FS_ | CLONE_SYSVSEM_;
    const dword_t known_unsupported = CLONE_VM_ | CLONE_SIGHAND_ | CLONE_THREAD_ |
        CLONE_NEWNS_ | CLONE_NEWCGROUP_ | CLONE_NEWUTS_ | CLONE_NEWIPC_ |
        CLONE_NEWUSER_ | CLONE_NEWPID_ | CLONE_NEWNET_ | CLONE_IO_;
    const dword_t known = supported | known_unsupported;

    if (flags & ~known)
        return _EINVAL;
    if (flags & known_unsupported)
        return _ENOSYS;

    if (flags & CLONE_FILES_) {
        int err = fdtable_unshare_current();
        if (err < 0)
            return err;
    }

    if ((flags & CLONE_FS_) && current->fs->refcount != 1) {
        struct fs_info *old_fs = current->fs;
        struct fs_info *new_fs = fs_info_copy(old_fs);
        if (new_fs == NULL)
            return _ENOMEM;
        current->fs = new_fs;
        fs_info_release(old_fs);
    }

    // SysV semaphore undo lists are not modeled separately, so treat this as a no-op.
    return 0;
}

dword_t sys_fork(void) {
    return sys_clone(SIGCHLD_, 0, 0, 0, 0);
}

dword_t sys_vfork(void) {
    return sys_clone(CLONE_VFORK_ | CLONE_VM_ | SIGCHLD_, 0, 0, 0, 0);
}

void vfork_notify(struct task *task) {
    if (task == NULL || task->pid > MAX_PID)
        return;

    // Callers already own the task lifetime here, and do_exit() can invoke us
    // while still holding task->general_lock. Re-locking it here self-deadlocks
    // the exiting task and wedges any later pids_lock users behind it.
    struct vfork_info *vfork = task->vfork;
    if (vfork == NULL)
        return;

    lock(&vfork->lock, 0);
    vfork->done = true;
    notify(&vfork->cond);
    unlock(&vfork->lock);
}
