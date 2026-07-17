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

// Linux applies __attribute__((packed)) to struct epoll_event ONLY under
// __x86_64__ (uapi/linux/eventpoll.h's EPOLL_PACKED). Every other guest
// architecture keeps the naturally-aligned 16-byte layout — `data` at
// offset 8, not the packed 12-byte layout's offset 4. So the on-the-wire
// epoll_event layout is a GUEST-ABI property: i386/amd64 use the packed
// form, arm64 the aligned form below. Marshalling arm64 with the packed
// layout put the guest's `data` word at the wrong offset AND used the
// wrong array stride, so the Go runtime's netpoll read a garbage pollDesc
// pointer and SIGSEGV'd during `go build` (runtime.netpoll <- sysmon).
struct epoll_event_ {
    uint32_t events;
    uint64_t data;
} __attribute__((packed));

struct epoll_event_arm64 {
    uint32_t events;
    uint32_t pad;
    uint64_t data;
};

static bool epoll_event_aligned(void) {
    if (current == NULL)
        return false;
    return current->abi == GUEST_ABI_ARM64 || current->abi == GUEST_ABI_RISCV64;
}

#define EPOLL_CTL_ADD_ 1
#define EPOLL_CTL_DEL_ 2
#define EPOLL_CTL_MOD_ 3
#define EPOLLET_ (1 << 31)
#define EPOLLONESHOT_ (1 << 30)

// Linux caps epoll nesting depth at EP_MAX_NESTS (fs/eventpoll.c) and treats
// either a genuine cycle or exceeding that depth as ELOOP. There was no such
// check here at all: adding an epoll fd `fd` that already (transitively)
// watches `target` silently succeeded via poll_add_fd, leaving a live cycle
// in memory. That's exactly what stress-ng's --epoll stressor probes for
// (two epoll instances added into each other) -- since the add "succeeded"
// (returned 0), errno was left untouched from an unrelated earlier syscall,
// so the stressor's failure message showed a stale, misleading errno instead
// of the real problem: the call should have failed with ELOOP and didn't.
#define EP_MAX_NESTS 4

// Would adding `target` -> `fd` (i.e. target starts watching fd) create a
// cycle? True if `target` is reachable by following fd's own nested-epoll
// children, or if doing so would exceed Linux's nesting depth limit.
static bool epoll_would_loop(struct fd *target, struct fd *fd, int depth) {
    if (fd->ops != &epoll_ops)
        return false;
    if (depth >= EP_MAX_NESTS)
        return true;
    struct poll *poll = fd->epollfd.poll;
    lock(&poll->lock, 0);
    bool loop = false;
    struct poll_fd *poll_fd;
    list_for_each_entry(&poll->poll_fds, poll_fd, fds) {
        struct fd *child = poll_fd->fd;
        if (child == NULL)
            continue;
        if (child == target || epoll_would_loop(target, child, depth + 1)) {
            loop = true;
            break;
        }
    }
    unlock(&poll->lock);
    return loop;
}

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

    // Linux: targeting the epoll instance itself is EINVAL. (A *different*
    // epoll fd may be nested, but an epoll cannot watch itself.)
    if (fd == epoll) {
        fd_close(fd);
        fd_close(epoll);
        return _EINVAL;
    }
    // Reject unknown ops up front. Without this an unknown op fell through to
    // the MOD path below, so e.g. epoll_ctl(ep, 9999, fd) silently modified a
    // registered fd (or returned ENOENT) instead of EINVAL.
    if (op != EPOLL_CTL_ADD_ && op != EPOLL_CTL_DEL_ && op != EPOLL_CTL_MOD_) {
        fd_close(fd);
        fd_close(epoll);
        return _EINVAL;
    }

    if (op == EPOLL_CTL_DEL_) {
        int_t res = poll_del_fd(epoll->epollfd.poll, fd);
        fd_close(fd);
        fd_close(epoll);
        return res;
    }

    uint32_t ev_events;
    uint64_t ev_data;
    if (epoll_event_aligned()) {
        struct epoll_event_arm64 event;
        if (user_get(event_addr, event)) {
            fd_close(fd);
            fd_close(epoll);
            return _EFAULT;
        }
        ev_events = event.events;
        ev_data = event.data;
    } else {
        struct epoll_event_ event;
        if (user_get(event_addr, event)) {
            fd_close(fd);
            fd_close(epoll);
            return _EFAULT;
        }
        ev_events = event.events;
        ev_data = event.data;
    }
    STRACE(" {events: %#x, data: %#llx}", ev_events, (unsigned long long) ev_data);
    if (epoll_trace_comm()) {
        printk("epoll-trace: ctl pid=%d comm=%s epfd=%d op=%d fd=%d real=%d req_events=%#x data=%#llx\n",
               current->pid, current->comm, epoll_f, op, f,
               fd->real_fd, ev_events, (unsigned long long) ev_data);
    }

    int_t res;
    if (op == EPOLL_CTL_ADD_) {
        if (poll_has_fd(epoll->epollfd.poll, fd))
            res = _EEXIST;
        else if (epoll_would_loop(epoll, fd, 0))
            res = _ELOOP;
        else
            res = poll_add_fd(epoll->epollfd.poll, fd, ev_events, (union poll_fd_info) ev_data);
    } else {
        res = poll_mod_fd(epoll->epollfd.poll, fd, ev_events, (union poll_fd_info) ev_data);
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
        int fault = 0;
        if (res > 0) {
            if (epoll_event_aligned()) {
                struct epoll_event_arm64 aligned[res];
                for (int i = 0; i < res; i++) {
                    aligned[i].events = events[i].events;
                    aligned[i].pad = 0;
                    aligned[i].data = events[i].data;
                }
                fault = user_write(events_addr, aligned, sizeof(aligned));
            } else {
                fault = user_write(events_addr, events, sizeof(struct epoll_event_) * res);
            }
        }
        if (fault) {
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
    .anon_inode_class = "eventpoll",
    .close = epoll_close,
};
