#include <string.h>
#include "debug.h"
#include "kernel/fs.h"
#include "kernel/time.h"
#include "fs/devices.h"
#include "fs/fd.h"
#include "fs/poll.h"
#include "fs/tty.h"
#include "kernel/calls.h"

static int user_read_or_zero(addr_t addr, void *data, size_t size) {
    if (addr == 0)
        memset(data, 0, size);
    else if (user_read(addr, data, size))
        return _EFAULT;
    return 0;
}

static bool poll_trace_comm(const char *comm) {
    if (comm == NULL)
        return false;
    return strcmp(comm, "apk") == 0 ||
        strcmp(comm, "wget") == 0 ||
        strcmp(comm, "curl") == 0 ||
        strcmp(comm, "ping") == 0 ||
        strcmp(comm, "cat") == 0 ||
        strcmp(comm, "grep") == 0 ||
        strcmp(comm, "which") == 0 ||
        strcmp(comm, "install") == 0 ||
        strncmp(comm, "deboots", 7) == 0 ||
        strncmp(comm, "debootstrap", 11) == 0 ||
        strncmp(comm, "update-ca-certi", 15) == 0;
}

static bool poll_trace_net_enabled(void) {
    if (current == NULL)
        return false;
    return poll_trace_comm(current->comm);
}

static void poll_trace_net_fd(struct fd *fd, int requested, int ready, int revents, const char *phase) {
    if (fd == NULL || !poll_trace_net_enabled())
        return;

    if (fd->ops == &tty_dev.fd) {
        struct tty *tty = fd->tty;
        printk("INFO: net poll %s pid=%d comm=%s tty=%d:%d requested=%#x ready=%#x revents=%#x\n",
               phase, current->pid, current->comm,
               tty != NULL ? tty->driver->major : -1,
               tty != NULL ? tty->num : -1,
               requested, ready, revents);
        return;
    }

    char path[MAX_PATH];
    path[0] = '\0';
    generic_getpath(fd, path);
    printk("INFO: net poll %s pid=%d comm=%s real=%d requested=%#x ready=%#x revents=%#x path=%s\n",
           phase, current->pid, current->comm, fd->real_fd, requested, ready, revents, path);
}

#define SELECT_READ (POLL_READ | POLL_HUP | POLL_ERR)
#define SELECT_WRITE (POLL_WRITE | POLL_ERR)
#define SELECT_EX (POLL_PRI)
struct select_context {
    char *readfds;
    char *writefds;
    char *exceptfds;
};
static int select_event_callback(void *context, int types, union poll_fd_info info) {
    struct select_context *c = context;
    if (types & SELECT_READ)
        bit_set(info.fd, c->readfds);
    if (types & SELECT_WRITE)
        bit_set(info.fd, c->writefds);
    if (types & SELECT_EX)
        bit_set(info.fd, c->exceptfds);
    if (!(types & (SELECT_READ | SELECT_WRITE | SELECT_EX)))
        return 0;
    return 1;
}

static void select_trace_net_fd(struct fd *fd, int requested, int ready, const char *phase) {
    if (fd == NULL || !poll_trace_net_enabled())
        return;

    if (fd->ops == &tty_dev.fd) {
        struct tty *tty = fd->tty;
        printk("INFO: net select %s pid=%d comm=%s tty=%d:%d requested=%#x ready=%#x\n",
               phase, current->pid, current->comm,
               tty != NULL ? tty->driver->major : -1,
               tty != NULL ? tty->num : -1,
               requested, ready);
        return;
    }

    char path[MAX_PATH];
    path[0] = '\0';
    generic_getpath(fd, path);
    printk("INFO: net select %s pid=%d comm=%s real=%d requested=%#x ready=%#x path=%s\n",
           phase, current->pid, current->comm, fd->real_fd, requested, ready, path);
}

static bool select_timeout_valid(struct timespec timeout_ts) {
    return timeout_ts.tv_sec >= 0 && timeout_ts.tv_nsec >= 0 && timeout_ts.tv_nsec < 1000000000;
}

static dword_t sys_select_common(fd_t nfds, addr_t readfds_addr, addr_t writefds_addr,
        addr_t exceptfds_addr, const struct timespec *timeout_ts_ptr) {
    size_t fdset_size = BITS_SIZE(nfds);
    char readfds[fdset_size];
    if (user_read_or_zero(readfds_addr, readfds, fdset_size))
        return _EFAULT;
    char writefds[fdset_size];
    if (user_read_or_zero(writefds_addr, writefds, fdset_size))
        return _EFAULT;
    char exceptfds[fdset_size];
    if (user_read_or_zero(exceptfds_addr, exceptfds, fdset_size))
        return _EFAULT;
    struct timespec timeout_ts = {};
    if (timeout_ts_ptr != NULL)
        timeout_ts = *timeout_ts_ptr;

    STRACE("select(%d, 0x%x, 0x%x, 0x%x, %s{%lds %ldns}) ",
            nfds, readfds_addr, writefds_addr, exceptfds_addr,
            timeout_ts_ptr == NULL ? "NULL " : "", timeout_ts.tv_sec, timeout_ts.tv_nsec);

    struct poll *poll = poll_create();
    if (IS_ERR(poll))
        return PTR_ERR(poll);
    struct fd *files[nfds];
    memset(files, 0, sizeof(files));

    for (fd_t i = 0; i < nfds; i++) {
        int events = 0;
        if (bit_test(i, readfds))
            events |= SELECT_READ;
        if (bit_test(i, writefds))
            events |= SELECT_WRITE;
        if (bit_test(i, exceptfds))
            events |= SELECT_EX;
        if (events != 0) {
            STRACE("%d{%s%s%s} ", i,
                    bit_test(i, readfds) ? "r" : "",
                    bit_test(i, writefds) ? "w" : "",
                    bit_test(i, exceptfds) ? "x" : "");
            struct fd *fd = f_get_retain(i);
            if (fd == NULL) {
                poll_destroy(poll);
                for (fd_t j = 0; j < i; j++) {
                    if (files[j] != NULL)
                        fd_close(files[j]);
                }
                return _EBADF;
            }
            files[i] = fd;
            poll_add_fd(poll, fd, events, (union poll_fd_info) i);
        }
    }
    STRACE("...\n");

    if (poll_trace_net_enabled()) {
        for (fd_t i = 0; i < nfds; i++) {
            if (files[i] == NULL)
                continue;
            int requested = 0;
            if (bit_test(i, readfds))
                requested |= SELECT_READ;
            if (bit_test(i, writefds))
                requested |= SELECT_WRITE;
            if (bit_test(i, exceptfds))
                requested |= SELECT_EX;
            if (requested == 0)
                continue;
            int ready = files[i]->ops->poll ? files[i]->ops->poll(files[i]) : 0;
            select_trace_net_fd(files[i], requested, ready, "enter");
        }
    }

    memset(readfds, 0, fdset_size);
    memset(writefds, 0, fdset_size);
    memset(exceptfds, 0, fdset_size);
    struct select_context context = {readfds, writefds, exceptfds};
    int err = 0;
    TASK_MAY_BLOCK {
        err = poll_wait(poll, select_event_callback, &context, timeout_ts_ptr == NULL ? NULL : &timeout_ts);
    }
    STRACE("%d end select ", current->pid);
    for (fd_t i = 0; i < nfds; i++) {
        if (bit_test(i, readfds) || bit_test(i, writefds) || bit_test(i, exceptfds)) {
            STRACE("%d{%s%s%s} ", i,
                    bit_test(i, readfds) ? "r" : "",
                    bit_test(i, writefds) ? "w" : "",
                    bit_test(i, exceptfds) ? "x" : "");
        }
    }
    poll_destroy(poll);
    for (fd_t i = 0; i < nfds; i++) {
        if (files[i] != NULL)
            fd_close(files[i]);
    }
    if (err < 0)
        return err;

    if (poll_trace_net_enabled()) {
        for (fd_t i = 0; i < nfds; i++) {
            if (files[i] == NULL)
                continue;
            int revents = 0;
            if (bit_test(i, readfds))
                revents |= SELECT_READ;
            if (bit_test(i, writefds))
                revents |= SELECT_WRITE;
            if (bit_test(i, exceptfds))
                revents |= SELECT_EX;
            if (revents == 0)
                continue;
            int ready = files[i]->ops->poll ? files[i]->ops->poll(files[i]) : 0;
            select_trace_net_fd(files[i], revents, ready, "exit");
        }
    }

    if (readfds_addr && user_write(readfds_addr, readfds, fdset_size))
        return _EFAULT;
    if (writefds_addr && user_write(writefds_addr, writefds, fdset_size))
        return _EFAULT;
    if (exceptfds_addr && user_write(exceptfds_addr, exceptfds, fdset_size))
        return _EFAULT;
    return err;
}

dword_t sys_select(fd_t nfds, addr_t readfds_addr, addr_t writefds_addr, addr_t exceptfds_addr, addr_t timeout_addr) {
    struct timespec timeout_ts = {};
    const struct timespec *timeout_ts_ptr = NULL;
    if (timeout_addr != 0) {
        struct timeval_ timeout_timeval;
        if (user_get(timeout_addr, timeout_timeval))
            return _EFAULT;
        timeout_ts.tv_sec = timeout_timeval.sec;
        timeout_ts.tv_nsec = timeout_timeval.usec * 1000;
        // Keep historical select() behavior and normalize invalid timeval input.
        timeout_ts = timespec_normalize(timeout_ts);
        timeout_ts_ptr = &timeout_ts;
    }
    return sys_select_common(nfds, readfds_addr, writefds_addr, exceptfds_addr, timeout_ts_ptr);
}

struct poll_context {
    struct pollfd_ *polls;
    struct fd **files;
    int nfds;
};

static bool poll_trace_script_enabled(void) {
    return current != NULL && strcmp(current->comm, "script") == 0;
}

static void poll_trace_script_fd(struct fd *fd, int requested, int ready) {
    if (fd == NULL)
        return;
    if (fd->ops == &tty_dev.fd) {
        struct tty *tty = fd->tty;
        int slave_entries = -1;
        int slave_refs = -1;
        int master_entries = -1;
        int master_refs = -1;
        int half_closed = 0;
        if (tty != NULL) {
            if (tty->type == TTY_PSEUDO_MASTER_MAJOR && tty->pty.other != NULL) {
                struct tty *slave = tty->pty.other;
                lock(&slave->fds_lock, 0);
                struct fd *slave_fd;
                slave_entries = 0;
                slave_refs = 0;
                list_for_each_entry(&slave->fds, slave_fd, tty_other_fds) {
                    slave_entries++;
                    slave_refs += slave_fd->refcount;
                }
                unlock(&slave->fds_lock);
                half_closed = slave->ever_opened && slave_entries == 0;
            }
            lock(&tty->fds_lock, 0);
            struct fd *tty_fd;
            master_entries = 0;
            master_refs = 0;
            list_for_each_entry(&tty->fds, tty_fd, tty_other_fds) {
                master_entries++;
                master_refs += tty_fd->refcount;
            }
            unlock(&tty->fds_lock);
            printk("INFO: script poll fd tty major=%d num=%d requested=%#x ready=%#x hung=%d packet=%#x tty_fds=%d tty_refs=%d slave_fds=%d slave_refs=%d half_closed=%d\n",
                   tty->driver->major, tty->num, requested, ready, tty->hung_up,
                   tty->packet_flags, master_entries, master_refs, slave_entries, slave_refs, half_closed);
        }
    } else {
        char path[MAX_PATH];
        path[0] = '\0';
        generic_getpath(fd, path);
        printk("INFO: script poll fd real=%d requested=%#x ready=%#x path=%s\n",
               fd->real_fd, requested, ready, path);
    }
}

#define POLL_ALWAYS_LISTENING (POLL_ERR|POLL_HUP|POLL_NVAL)
static int poll_event_callback(void *context, int types, union poll_fd_info info) {
    struct poll_context *c = context;
    struct pollfd_ *polls = c->polls;
    int nfds = c->nfds;
    int res = 0;
    for (int i = 0; i < nfds; i++) {
        if (c->files[i] == info.ptr) {
            polls[i].revents = types & (polls[i].events | POLL_ALWAYS_LISTENING);
            res = 1;
        }
    }
    return res;
}
dword_t sys_poll(addr_t fds, dword_t nfds, int_t timeout) {
    STRACE("poll(0x%x, %d, %d)", fds, nfds, timeout);
    struct pollfd_ polls[nfds];
    if (fds != 0 || nfds != 0)
        if (user_read(fds, polls, sizeof(struct pollfd_) * nfds))
            return _EFAULT;
    struct poll *poll = poll_create();
    if (IS_ERR(poll))
        return PTR_ERR(poll);

    for (unsigned i = 0; i < nfds; i++)
        STRACE(" {%d, %#x}", polls[i].fd, polls[i].events);
    STRACE("...\n");

    struct fd *files[nfds];
    int add_err = 0;
    for (unsigned i = 0; i < nfds; i++) {
        files[i] = f_get_retain(polls[i].fd);
        // clear revents, which is reused to mark whether a pollfd has been added or not
        polls[i].revents = 0;
    }
    // convert polls array into poll_add_fd calls
    // FIXME this is quadratic
    for (unsigned i = 0; i < nfds; i++) {
        if (polls[i].fd < 0 || polls[i].revents)
            continue;

        // if the same fd is listed more than once, merge the events bits together
        int events = polls[i].events;
        polls[i].revents = 1;
        if (files[i] == NULL)
            continue;
        for (unsigned j = 0; j < nfds; j++) {
            if (polls[j].revents)
                continue;
            if (files[i] == files[j]) {
                events |= polls[j].events;
                polls[j].revents = 1;
            }
        }

        add_err = poll_add_fd(poll, files[i], events | POLL_ALWAYS_LISTENING, (union poll_fd_info) (void *) files[i]);
        if (add_err < 0)
            goto out;
    }

    for (unsigned i = 0; i < nfds; i++) {
        polls[i].revents = 0;
        if (f_get(polls[i].fd) == NULL)
            polls[i].revents = POLL_NVAL;
    }
    struct poll_context context = {polls, files, nfds};
    struct timespec timeout_ts;
    if (timeout >= 0) {
        timeout_ts.tv_sec = timeout / 1000;
        timeout_ts.tv_nsec = (timeout % 1000) * 1000000;
    }
    if (poll_trace_script_enabled()) {
        for (unsigned i = 0; i < nfds; i++) {
            if (files[i] == NULL)
                continue;
            int ready = files[i]->ops->poll ? files[i]->ops->poll(files[i]) : 0;
            poll_trace_script_fd(files[i], polls[i].events | POLL_ALWAYS_LISTENING, ready);
        }
    }
    if (poll_trace_net_enabled()) {
        for (unsigned i = 0; i < nfds; i++) {
            if (files[i] == NULL)
                continue;
            int ready = files[i]->ops->poll ? files[i]->ops->poll(files[i]) : 0;
            poll_trace_net_fd(files[i], polls[i].events | POLL_ALWAYS_LISTENING, ready, polls[i].revents, "enter");
        }
    }
    int res = 0;
    TASK_MAY_BLOCK {
        res = poll_wait(poll, poll_event_callback, &context, timeout < 0 ? NULL : &timeout_ts);
    }
out:
    poll_destroy(poll);
    for (unsigned i = 0; i < nfds; i++) {
        if (files[i] != NULL)
            fd_close(files[i]);
    }
    STRACE("%d end poll", current->pid);
    if (poll_trace_net_enabled()) {
        printk("INFO: net poll return pid=%d comm=%s res=%d timeout_ms=%d\n",
               current->pid, current->comm, res, timeout);
        for (unsigned i = 0; i < nfds; i++) {
            if (files[i] == NULL)
                continue;
            int ready = files[i]->ops->poll ? files[i]->ops->poll(files[i]) : 0;
            poll_trace_net_fd(files[i], polls[i].events | POLL_ALWAYS_LISTENING, ready, polls[i].revents, "exit");
        }
    }

    if (add_err < 0)
        return add_err;
    if (res < 0)
        return res;
    if (fds != 0 || nfds != 0)
        if (user_write(fds, polls, sizeof(struct pollfd_) * nfds))
            return _EFAULT;
    return res;
}

dword_t sys_pselect(fd_t nfds, addr_t readfds_addr, addr_t writefds_addr, addr_t exceptfds_addr, addr_t timeout_addr, addr_t sigmask_addr) {
    // a system call can only take 6 parameters, so the last two need to be passed as a pointer to a struct
    struct {
        addr_t mask_addr;
        dword_t mask_size;
    } sigmask = {};
    if (sigmask_addr != 0) {
        if (user_get(sigmask_addr, sigmask))
            return _EFAULT;
    }
    sigset_t_ mask;
    struct timespec timeout_ts = {};
    const struct timespec *timeout_ts_ptr = NULL;

    if (timeout_addr != 0) {
        struct timespec_ timeout_timespec;
        if (user_get(timeout_addr, timeout_timespec))
            return _EFAULT;
        timeout_ts.tv_sec = timeout_timespec.sec;
        timeout_ts.tv_nsec = timeout_timespec.nsec;
        if (!select_timeout_valid(timeout_ts))
            return _EINVAL;
        timeout_ts_ptr = &timeout_ts;
    }

    if (sigmask.mask_addr != 0) {
        if (sigmask.mask_size != sizeof(sigset_t_))
            return _EINVAL;
        if (user_get(sigmask.mask_addr, mask))
            return _EFAULT;
        sigmask_set_temp(mask);
    }

    // Keep the temporary mask installed until interrupt-exit signal delivery
    // runs. That path knows how to restore saved_mask after deciding whether
    // the pending signal should interrupt the wait.
    return sys_select_common(nfds, readfds_addr, writefds_addr, exceptfds_addr, timeout_ts_ptr);
}

dword_t sys_pselect_time64(fd_t nfds, addr_t readfds_addr, addr_t writefds_addr, addr_t exceptfds_addr,
        addr_t timeout_addr, addr_t sigmask_addr) {
    struct {
        addr_t mask_addr;
        dword_t mask_size;
    } sigmask = {};
    if (sigmask_addr != 0) {
        if (user_get(sigmask_addr, sigmask))
            return _EFAULT;
    }
    sigset_t_ mask;
    struct timespec timeout_ts = {};
    const struct timespec *timeout_ts_ptr = NULL;

    if (timeout_addr != 0) {
        struct timespec64_ timeout_timespec;
        if (user_get(timeout_addr, timeout_timespec))
            return _EFAULT;
        timeout_ts.tv_sec = timeout_timespec.sec;
        timeout_ts.tv_nsec = timeout_timespec.nsec;
        if (!select_timeout_valid(timeout_ts))
            return _EINVAL;
        timeout_ts_ptr = &timeout_ts;
    }

    if (sigmask.mask_addr != 0) {
        if (sigmask.mask_size != sizeof(sigset_t_))
            return _EINVAL;
        if (user_get(sigmask.mask_addr, mask))
            return _EFAULT;
        sigmask_set_temp(mask);
    }

    // See sys_pselect(): receive_signals() restores the saved mask after it
    // handles any signal that interrupted the wait.
    return sys_select_common(nfds, readfds_addr, writefds_addr, exceptfds_addr, timeout_ts_ptr);
}

dword_t sys_ppoll(addr_t fds, dword_t nfds, addr_t timeout_addr, addr_t sigmask_addr, dword_t sigsetsize) {
    int timeout = -1;
    if (timeout_addr != 0) {
        struct timespec_ timeout_timespec;
        if (user_get(timeout_addr, timeout_timespec))
            return _EFAULT;
        timeout = timeout_timespec.sec * 1000 + timeout_timespec.nsec / 1000000;
    }

    sigset_t_ mask;
    if (sigmask_addr != 0) {
        if (sigsetsize != sizeof(sigset_t_))
            return _EINVAL;
        if (user_get(sigmask_addr, mask))
            return _EFAULT;
        sigmask_set_temp(mask);
    }

    // Leave restoration to receive_signals() so an interrupting signal is
    // observed against the temporary mask instead of the restored one.
    return sys_poll(fds, nfds, timeout);
}
