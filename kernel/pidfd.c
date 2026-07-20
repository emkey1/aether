// pidfd support (issue #423): a pidfd is a stable, pollable file descriptor
// referencing a specific task, used by CLONE_PIDFD (kernel/fork.c),
// pidfd_open(2) and pidfd_send_signal(2).
#include "kernel/calls.h"
#include "kernel/fs.h"
#include "fs/poll.h"

struct pidfd_data {
    struct task *task; // pinned via task_ref_cnt_mod
    struct fd *fd;      // back-pointer, for poll_wakeup from pidfd_notify_exit
    struct list pidfd_link; // linked into task->pidfds, locked by pids_lock
};

static struct fd_ops pidfd_ops;

// Takes its own reference on `task` (the caller's own reference, if any, is
// unaffected and must still be released by the caller as usual).
struct fd *pidfd_create(struct task *task) {
    struct pidfd_data *data = malloc(sizeof(struct pidfd_data));
    if (data == NULL)
        return ERR_PTR(_ENOMEM);
    struct fd *fd = adhoc_fd_create(&pidfd_ops);
    if (fd == NULL) {
        free(data);
        return ERR_PTR(_ENOMEM);
    }
    task_ref_cnt_mod(task, 1);
    data->task = task;
    data->fd = fd;
    fd->data = data;

    complex_lockt(&pids_lock, 0);
    list_add(&task->pidfds, &data->pidfd_link);
    unlock(&pids_lock);
    return fd;
}

static int pidfd_close(struct fd *fd) {
    struct pidfd_data *data = fd->data;
    complex_lockt(&pids_lock, 0);
    list_remove(&data->pidfd_link);
    unlock(&pids_lock);
    task_ref_cnt_mod(data->task, -1);
    free(data);
    return 0;
}

static int pidfd_poll(struct fd *fd) {
    struct pidfd_data *data = fd->data;
    complex_lockt(&pids_lock, 0);
    int types = data->task->zombie ? POLL_READ : 0;
    unlock(&pids_lock);
    return types;
}

static struct fd_ops pidfd_ops = {
    .anon_inode_class = "[pidfd]",
    .poll = pidfd_poll,
    .close = pidfd_close,
};

// Called from do_exit (kernel/exit.c) with pids_lock already held, right
// after a task becomes a zombie: wakes any poll()/epoll_wait() blocked on a
// pidfd referencing it.
void pidfd_notify_exit(struct task *task) {
    struct pidfd_data *data;
    list_for_each_entry(&task->pidfds, data, pidfd_link)
        poll_wakeup(data->fd, POLL_READ);
}

#define PIDFD_NONBLOCK_ O_NONBLOCK_

int_t sys_pidfd_open(pid_t_ pid, dword_t flags) {
    STRACE("pidfd_open(%d, %#x)", pid, flags);
    if (flags & ~PIDFD_NONBLOCK_)
        return _EINVAL;
    struct task *task = pid_get_task_ref(pid);
    if (task == NULL)
        return _ESRCH;
    struct fd *fd = pidfd_create(task);
    task_ref_cnt_mod(task, -1); // drop pid_get_task_ref's ref; pidfd_create took its own
    if (IS_ERR(fd))
        return PTR_ERR(fd);
    return f_install(fd, (flags & PIDFD_NONBLOCK_) ? O_NONBLOCK_ : 0);
}

// Resolve a pidfd fd number to its task's pid, for waitid(P_PIDFD, fd, ...)
// (how systemd >= 260 waits for every process it forks, e.g. generators).
int_t pidfd_get_pid(fd_t f) {
    struct fd *fd = f_get(f);
    if (fd == NULL)
        return _EBADF;
    // A valid fd that isn't a pidfd is EBADF too (Linux pidfd_pid()).
    if (fd->ops != &pidfd_ops)
        return _EBADF;
    struct pidfd_data *data = fd->data;
    complex_lockt(&pids_lock, 0);
    pid_t_ pid = data->task->pid;
    unlock(&pids_lock);
    return pid;
}

int_t sys_pidfd_send_signal(fd_t pidfd, dword_t sig, addr_t UNUSED(info_addr), dword_t flags) {
    STRACE("pidfd_send_signal(%d, %d, flags=%#x)", pidfd, sig, flags);
    if (flags != 0)
        return _EINVAL;
    if (sig >= NUM_SIGS)
        return _EINVAL;
    struct fd *fd = f_get(pidfd);
    if (fd == NULL)
        return _EBADF;
    if (fd->ops != &pidfd_ops)
        return _EINVAL;
    struct pidfd_data *data = fd->data;
    return signal_kill_task(data->task, sig, SI_USER_);
}
