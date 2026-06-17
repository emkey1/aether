#include "kernel/calls.h"
#include "fs/poll.h"
#include <stdlib.h>
#include <string.h>

static struct fd_ops epoll_ops;

static bool epoll_trace_enabled(void) {
    static int enabled = -1;
    if (enabled < 0)
        enabled = getenv("ISH_TRACE_EPOLL") != NULL ? 1 : 0;
    return enabled == 1;
}

static bool epoll_trace_comm(void) {
    return epoll_trace_enabled() && current != NULL && strcmp(current->comm, "compile") == 0;
}

fd_t sys_epoll_create(int_t flags) {
    STRACE("epoll_create(%#x)", flags);
    if (flags & ~(O_CLOEXEC_))
        return _EINVAL;

    struct fd *fd = adhoc_fd_create(&epoll_ops);
    if (fd == NULL)
        return _ENOMEM;
    struct poll *poll = poll_create();
    if (IS_ERR(poll))
        return PTR_ERR(poll);
    fd->epollfd.poll = poll;
    return f_install(fd, flags);
}
fd_t sys_epoll_create0() {
    return sys_epoll_create(0);
}

struct epoll_event_ {
    uint32_t events;
    uint64_t data;
} __attribute__((packed));

#define EPOLL_CTL_ADD_ 1
#define EPOLL_CTL_DEL_ 2
#define EPOLL_CTL_MOD_ 3
#define EPOLLET_ (1 << 31)
#define EPOLLONESHOT_ (1 << 30)

int_t sys_epoll_ctl_guest(fd_t epoll_f, int_t op, fd_t f, guest_addr_t event_addr) {
    STRACE("epoll_ctl(%d, %d, %d, %#x)", epoll_f, op, f, event_addr);
    struct fd *epoll = f_get_retain(epoll_f);
    if (epoll == NULL)
        return _EBADF;
    if (epoll->ops != &epoll_ops) {
        fd_close(epoll);
        return _EINVAL;
    }
    struct fd *fd = f_get_retain(f);
    if (fd == NULL) {
        fd_close(epoll);
        return _EBADF;
    }

    if (op == EPOLL_CTL_DEL_) {
        int_t res = poll_del_fd(epoll->epollfd.poll, fd);
        fd_close(fd);
        fd_close(epoll);
        return res;
    }

    struct epoll_event_ event;
    if (user_get(event_addr, event)) {
        fd_close(fd);
        fd_close(epoll);
        return _EFAULT;
    }
    STRACE(" {events: %#x, data: %#x}", event.events, event.data);
    if (epoll_trace_comm()) {
        printk("epoll-trace: ctl pid=%d comm=%s epfd=%d op=%d fd=%d real=%d req_events=%#x data=%#llx\n",
               current->pid, current->comm, epoll_f, op, f,
               fd->real_fd, event.events, (unsigned long long) event.data);
    }

    int_t res;
    if (op == EPOLL_CTL_ADD_) {
        if (poll_has_fd(epoll->epollfd.poll, fd))
            res = _EEXIST;
        else
            res = poll_add_fd(epoll->epollfd.poll, fd, event.events, (union poll_fd_info) event.data);
    } else {
        res = poll_mod_fd(epoll->epollfd.poll, fd, event.events, (union poll_fd_info) event.data);
    }
    fd_close(fd);
    fd_close(epoll);
    return res;
}

int_t sys_epoll_ctl(fd_t epoll_f, int_t op, fd_t f, addr_t event_addr) {
    return sys_epoll_ctl_guest(epoll_f, op, f, event_addr);
}

struct epoll_context {
    struct epoll_event_ *events;
    int n;
    int max_events;
};

static int epoll_callback(void *context, int types, union poll_fd_info info) {
    struct epoll_context *c = context;
    if (c->n >= c->max_events)
        return 0;
    if (epoll_trace_comm()) {
        printk("epoll-trace: callback pid=%d comm=%s slot=%d types=%#x data=%#llx\n",
               current->pid, current->comm, c->n, types,
               (unsigned long long) info.num);
    }
    c->events[c->n++] = (struct epoll_event_) {.events = types, .data = info.num};
    return 1;
}

static int epoll_wait_common(fd_t epoll_f, guest_addr_t events_addr, int_t max_events, struct timespec *timeout_ts_ptr) {
    struct fd *epoll = f_get_retain(epoll_f);
    if (epoll == NULL)
        return _EBADF;
    if (epoll->ops != &epoll_ops) {
        fd_close(epoll);
        return _EINVAL;
    }

    if (max_events <= 0) {
        fd_close(epoll);
        return _EINVAL;
    }
    struct epoll_event_ events[max_events];

    struct epoll_context context = {.events = events, .n = 0, .max_events = max_events};
    STRACE("...\n");
    // A guest infinite wait (timeout == -1, i.e. timeout_ts_ptr == NULL) must
    // block until an fd is ready or a signal arrives. Real Linux never returns
    // 0 events from epoll_wait(-1); some guests assert on it -- notably libuv's
    // uv__io_poll does `assert(timeout != -1)` when epoll_wait returns 0, so
    // cmake/node/etc. abort. We still cap each underlying wait at 2 s as a
    // safety net against a missed readiness notification (the reason the Go
    // scheduler originally needed this -- a re-armed wait re-scans readiness and
    // recovers the missed transition), but when the cap expires with nothing
    // ready we wait again instead of reporting a spurious timeout to the guest.
    bool guest_infinite = (timeout_ts_ptr == NULL);
    struct timespec bounded = { .tv_sec = 2 };
    int res;
    do {
        context.n = 0;
        res = poll_wait(epoll->epollfd.poll, epoll_callback, &context,
                        guest_infinite ? &bounded : timeout_ts_ptr);
    } while (guest_infinite && res == 0);
    STRACE("%d end epoll_wait", current->pid);
    if (res >= 0) {
        for (int i = 0; i < res; i++) {
            STRACE(" {events: %#x, data: %#x}", events[i].events, events[i].data);
        }
        if (user_write(events_addr, events, sizeof(struct epoll_event_) * res)) {
            fd_close(epoll);
            return _EFAULT;
        }
    }
    fd_close(epoll);
    return res;
}

int_t sys_epoll_wait_guest(fd_t epoll_f, guest_addr_t events_addr, int_t max_events, int_t timeout) {
    STRACE("epoll_wait(%d, %#x, %d, %d)", epoll_f, events_addr, max_events, timeout);
    struct timespec timeout_ts;
    struct timespec *timeout_ts_ptr = NULL;
    if (timeout >= 0) {
        timeout_ts.tv_sec = timeout / 1000;
        timeout_ts.tv_nsec = (timeout % 1000) * 1000000;
        timeout_ts_ptr = &timeout_ts;
    }
    return epoll_wait_common(epoll_f, events_addr, max_events, timeout_ts_ptr);
}

int_t sys_epoll_wait(fd_t epoll_f, addr_t events_addr, int_t max_events, int_t timeout) {
    return sys_epoll_wait_guest(epoll_f, events_addr, max_events, timeout);
}

int_t sys_epoll_pwait_guest(fd_t epoll_f, guest_addr_t events_addr, int_t max_events, int_t timeout, guest_addr_t sigmask_addr, dword_t sigsetsize) {
    sigset_t_ mask;
    if (sigmask_addr != 0) {
        if (sigsetsize != sizeof(sigset_t_))
            return _EINVAL;
        if (user_get(sigmask_addr, mask))
            return _EFAULT;
        sigmask_set_temp(mask);
    }

    struct timespec timeout_ts;
    struct timespec *timeout_ts_ptr = NULL;
    if (timeout >= 0) {
        timeout_ts.tv_sec = timeout / 1000;
        timeout_ts.tv_nsec = (timeout % 1000) * 1000000;
        timeout_ts_ptr = &timeout_ts;
    }

    return epoll_wait_common(epoll_f, events_addr, max_events, timeout_ts_ptr);
}

int_t sys_epoll_pwait(fd_t epoll_f, addr_t events_addr, int_t max_events, int_t timeout,
        addr_t sigmask_addr, dword_t sigsetsize) {
    return sys_epoll_pwait_guest(epoll_f, events_addr, max_events, timeout, sigmask_addr, sigsetsize);
}

int_t sys_epoll_pwait2_guest(fd_t epoll_f, guest_addr_t events_addr, int_t max_events, guest_addr_t timeout_addr, guest_addr_t sigmask_addr, dword_t sigsetsize) {
    sigset_t_ mask;
    if (sigmask_addr != 0) {
        if (sigsetsize != sizeof(sigset_t_))
            return _EINVAL;
        if (user_get(sigmask_addr, mask))
            return _EFAULT;
        sigmask_set_temp(mask);
    }

    struct timespec timeout_ts;
    struct timespec *timeout_ts_ptr = NULL;
    if (timeout_addr != 0) {
        struct timespec64_ timeout_ts64;
        if (user_get(timeout_addr, timeout_ts64))
            return _EFAULT;
        timeout_ts.tv_sec = timeout_ts64.sec;
        timeout_ts.tv_nsec = timeout_ts64.nsec;
        if (timeout_ts.tv_sec < 0 || timeout_ts.tv_nsec < 0 || timeout_ts.tv_nsec >= 1000000000)
            return _EINVAL;
        timeout_ts_ptr = &timeout_ts;
    }

    return epoll_wait_common(epoll_f, events_addr, max_events, timeout_ts_ptr);
}

int_t sys_epoll_pwait2(fd_t epoll_f, addr_t events_addr, int_t max_events,
        addr_t timeout_addr, addr_t sigmask_addr, dword_t sigsetsize) {
    return sys_epoll_pwait2_guest(epoll_f, events_addr, max_events, timeout_addr, sigmask_addr, sigsetsize);
}

static int epoll_close(struct fd *fd) {
    poll_destroy(fd->epollfd.poll);
    return 0;
}

static struct fd_ops epoll_ops = {
    .close = epoll_close,
};
