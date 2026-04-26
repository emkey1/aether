#include "kernel/task.h"
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include "misc.h"
#include "util/list.h"
#include "util/timer.h"
#include "kernel/errno.h"
#include "kernel/fs.h"
#include "fs/fd.h"
#include "fs/poll.h"
#include "fs/real.h"
#include "fs/sock.h"

#include "fs/sockrestart.h"

#if defined(__linux__)
#include <sys/epoll.h>
#define HAVE_EPOLL 1
#elif defined(__APPLE__)
#include <sys/event.h>
#define HAVE_KQUEUE 1
#endif

static int real_poll_init(struct real_poll *real);
static void real_poll_close(struct real_poll *real);
struct real_poll_event {
#if HAVE_EPOLL
    struct epoll_event real;
#elif HAVE_KQUEUE
    struct kevent real;
#endif
};
static void *rpe_data(struct real_poll_event *rpe);
static int rpe_events(struct real_poll_event *rpe);
static int real_poll_wait(struct real_poll *real, struct real_poll_event *events, int max, struct timespec *timeout);
static int real_poll_update(struct real_poll *real, int fd, int types, void *data);
static inline bool poll_fd_has_host_wait(struct poll_fd *pollfd);
static void poll_fd_free(struct poll_fd *poll_fd);

static bool poll_fd_needs_periodic_host_rescan(struct poll_fd *poll_fd) {
#if defined(__APPLE__)
    if (poll_fd == NULL || poll_fd->fd == NULL)
        return false;
    if (poll_fd->fd->ops != &realfs_fdops)
        return false;
    if (!(poll_fd->types & POLL_WRITE))
        return false;
    return is_adhoc_fd(poll_fd->fd) && S_ISFIFO(poll_fd->fd->stat.mode);
#else
    (void) poll_fd;
    return false;
#endif
}

static bool poll_needs_periodic_host_rescan(struct poll *poll_) {
    struct poll_fd *poll_fd;
    list_for_each_entry(&poll_->poll_fds, poll_fd, fds) {
        if (poll_fd_needs_periodic_host_rescan(poll_fd))
            return true;
    }
    return false;
}

static bool poll_deadline_remaining(const struct timespec *deadline, struct timespec *remaining) {
    if (deadline == NULL || remaining == NULL)
        return false;
    struct timespec now = timespec_now(CLOCK_MONOTONIC);
    if ((deadline->tv_sec < now.tv_sec) ||
            (deadline->tv_sec == now.tv_sec && deadline->tv_nsec <= now.tv_nsec)) {
        *remaining = (struct timespec) {0};
        return false;
    }
    *remaining = timespec_subtract(*deadline, now);
    return timespec_positive(*remaining);
}

static bool poll_trace_comm(const char *comm) {
    if (comm == NULL)
        return false;
    return strcmp(comm, "apk") == 0 ||
        strcmp(comm, "apt") == 0 ||
        strcmp(comm, "apt-get") == 0 ||
        strncmp(comm, "http", 4) == 0 ||
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

static bool poll_wait_trace_enabled(void) {
    static int enabled = -1;
    if (enabled < 0)
        enabled = getenv("ISH_TRACE_POLL_WAIT") != NULL ? 1 : 0;
    if (!enabled)
        return false;
    if (current == NULL)
        return false;
    return poll_trace_comm(current->comm);
}

static void poll_wait_trace_fd(struct poll_fd *poll_fd, int host_events, const char *phase) {
    if (!poll_wait_trace_enabled() || poll_fd == NULL || poll_fd->fd == NULL)
        return;

    char path[MAX_PATH];
    path[0] = '\0';
    generic_getpath(poll_fd->fd, path);
    fprintf(stderr, "ish-pollwait: %s pid=%d comm=%s real=%d types=%#x host=%#x path=%s\n",
           phase, current->pid, current->comm, poll_fd->fd->real_fd,
           poll_fd->types, host_events, path);
}

static void poll_wait_trace_raw_event(struct poll *poll_, struct real_poll_event *event, const char *phase) {
    if (!poll_wait_trace_enabled() || event == NULL)
        return;
#if HAVE_KQUEUE
    fprintf(stderr, "ish-pollwait: %s pid=%d comm=%s ident=%llu filter=%d flags=%#x fflags=%#x data=%lld udata=%p notify_fd=%d\n",
           phase, current->pid, current->comm,
           (unsigned long long) event->real.ident, event->real.filter,
           event->real.flags, event->real.fflags,
           (long long) event->real.data, event->real.udata,
           poll_ != NULL ? poll_->notify_pipe[0] : -1);
#elif HAVE_EPOLL
    fprintf(stderr, "ish-pollwait: %s pid=%d comm=%s events=%#x udata=%p notify_fd=%d\n",
           phase, current->pid, current->comm,
           event->real.events, event->real.data.ptr,
           poll_ != NULL ? poll_->notify_pipe[0] : -1);
#endif
}

static void poll_drop_unknown_event(struct poll *poll_, struct real_poll_event *event) {
#if HAVE_KQUEUE
    if (poll_ == NULL || event == NULL)
        return;
    int ident = (int) event->real.ident;
    if (ident < 0 || ident == poll_->notify_pipe[0])
        return;
    int err = real_poll_update(&poll_->real, ident, 0, NULL);
    if (poll_wait_trace_enabled()) {
        fprintf(stderr, "ish-pollwait: drop-raw pid=%d comm=%s ident=%d err=%d errno=%d\n",
               current->pid, current->comm, ident, err, err < 0 ? errno : 0);
    }
#else
    (void) poll_;
    (void) event;
#endif
}

static int poll_deliver_ready_locked(struct poll *poll_, struct poll_fd *poll_fd,
                                     int poll_types, poll_callback_t callback,
                                     void *context, const char *phase) {
    struct fd *fd = poll_fd->fd;

    if (poll_fd->types & POLL_EDGETRIGGERED)
        poll_types &= ~poll_fd->triggered_types;
    if (!poll_types)
        return 0;

    int handled = callback(context, poll_types, poll_fd->info);
    if (poll_wait_trace_enabled()) {
        fprintf(stderr, "ish-pollwait: %s pid=%d comm=%s real=%d events=%#x handled=%d\n",
               phase, current->pid, current->comm,
               fd != NULL ? fd->real_fd : -1, poll_types, handled);
    }
    int res = handled == 1 ? 1 : 0;

    // The real poll does not actually get the FDs set as oneshot.
    // But this loop is done while holding the lock, so only one
    // thread can get each oneshot event. This doesn't solve the
    // thundering herd problem at all, but at least the semantics
    // are right. I'll just leave that as a TODO.
    if (poll_fd->types & POLL_ONESHOT) {
        list_remove(&poll_fd->polls);
        list_remove(&poll_fd->fds);
        if (poll_fd_has_host_wait(poll_fd))
            real_poll_update(&poll_->real, fd->real_fd, 0, NULL);
        // Keep poll_fd storage alive on the freelist so stale host
        // readiness events cannot turn into a use-after-free.
        poll_fd_free(poll_fd);
        return res;
    }

    if (poll_fd->types & POLL_EDGETRIGGERED)
        poll_fd->triggered_types |= poll_types;
    return res;
}

static int poll_scan_ready_locked(struct poll *poll_, poll_callback_t callback, void *context) {
    int res = 0;
    struct poll_fd *poll_fd, *tmp;
    list_for_each_entry_safe(&poll_->poll_fds, poll_fd, tmp, fds) {
        struct fd *fd = poll_fd->fd;
        int raw_poll_types = 0;
        if (fd->ops->poll)
            raw_poll_types = fd->ops->poll(fd);
        int poll_types = raw_poll_types;
        poll_types &= poll_fd->types | POLL_HUP | POLL_ERR | POLL_NVAL;
        if (poll_fd->types & POLL_EDGETRIGGERED)
            poll_types &= ~poll_fd->triggered_types;
        if (poll_wait_trace_enabled()) {
            char path[MAX_PATH];
            path[0] = '\0';
            if (fd != NULL)
                generic_getpath(fd, path);
            fprintf(stderr, "ish-pollwait: scan pid=%d comm=%s real=%d raw=%#x masked=%#x types=%#x path=%s\n",
                   current->pid, current->comm,
                   fd != NULL ? fd->real_fd : -1,
                   raw_poll_types, poll_types,
                   poll_fd->types, path);
        }
        if (!poll_types)
            continue;

        res += poll_deliver_ready_locked(poll_, poll_fd, poll_types,
                                         callback, context, "callback");
    }
    return res;
}

// lock order: fd, then poll

struct poll *poll_create(void) {
    struct poll *poll = malloc(sizeof(struct poll));
    if (poll == NULL)
        return ERR_PTR(_ENOMEM);
    int err = real_poll_init(&poll->real);
    if (err < 0)
        return ERR_PTR(errno_map());
    poll->waiters = 0;
    poll->notify_pipe[0] = -1;
    poll->notify_pipe[1] = -1;
    poll->notify_pending = false;
    list_init(&poll->poll_fds);
    list_init(&poll->pollfd_freelist);
    lock_init(&poll->lock, "poll_create\0");
    return poll;
}

static inline bool poll_fd_has_host_wait(struct poll_fd *pollfd) {
    if (pollfd->fd == NULL || pollfd->fd->real_fd < 0)
        return false;
    return pollfd->fd->ops == &realfs_fdops || pollfd->fd->ops == &socket_fdops;
}

// does not do its own locking
static struct poll_fd *poll_find_fd(struct poll *poll, struct fd *fd) {
    struct poll_fd *poll_fd, *tmp;
    list_for_each_entry_safe(&poll->poll_fds, poll_fd, tmp, fds) {
        if (poll_fd->fd == fd)
            return poll_fd;
    }
    return NULL;
}

// See comment on pollfd_freelist for context
static void poll_fd_free(struct poll_fd *poll_fd) {
    struct poll *poll = poll_fd->poll;
    memset(poll_fd, 0xba, sizeof(*poll_fd));
    poll_fd->poll = NULL; // used to mark it as free
    list_add(&poll->pollfd_freelist, &poll_fd->fds);
}

// Host poll backends can return stale udata pointers after a watched fd has
// been removed. Only trust pointers that still correspond to poll_fd storage
// owned by this poll instance.
static struct poll_fd *poll_find_ptr(struct poll *poll, struct poll_fd *candidate) {
    struct poll_fd *poll_fd;
    list_for_each_entry(&poll->poll_fds, poll_fd, fds) {
        if (poll_fd == candidate)
            return poll_fd;
    }
    list_for_each_entry(&poll->pollfd_freelist, poll_fd, fds) {
        if (poll_fd == candidate)
            return poll_fd;
    }
    return NULL;
}

static struct poll_fd *poll_find_real_fd(struct poll *poll, int real_fd) {
    struct poll_fd *poll_fd;
    list_for_each_entry(&poll->poll_fds, poll_fd, fds) {
        if (poll_fd->fd != NULL && poll_fd->fd->real_fd == real_fd)
            return poll_fd;
    }
    return NULL;
}

bool poll_has_fd(struct poll *poll, struct fd *fd) {
    return poll_find_fd(poll, fd) != NULL;
}

int poll_add_fd(struct poll *poll, struct fd *fd, int types, union poll_fd_info info) {
    int err;
    lock(&fd->poll_lock, 0);
    lock(&poll->lock, 0);

    struct poll_fd *poll_fd;
    if (!list_empty(&poll->pollfd_freelist)) {
        poll_fd = list_first_entry(&poll->pollfd_freelist, struct poll_fd, fds);
        list_remove(&poll_fd->fds);
    } else {
        poll_fd = malloc(sizeof(struct poll_fd));
        if (poll_fd == NULL) {
            err = _ENOMEM;
            goto out;
        }
    }
    poll_fd->fd = fd;
    poll_fd->poll = poll;
    poll_fd->types = types;
    poll_fd->info = info;
    poll_fd->triggered_types = 0;

    if (poll_fd_has_host_wait(poll_fd)) {
        err = real_poll_update(&poll->real, fd->real_fd, types, poll_fd);
        if (err < 0) {
            free(poll_fd);
            err = errno_map();
            goto out;
        }
    }

    list_add(&fd->poll_fds, &poll_fd->polls);
    list_add(&poll->poll_fds, &poll_fd->fds);

    err = 0;
out:
    unlock(&poll->lock);
    unlock(&fd->poll_lock);
    return err;
}

int poll_del_fd(struct poll *poll, struct fd *fd) {
    int err;
    lock(&fd->poll_lock, 0);
    lock(&poll->lock, 0);
    struct poll_fd *poll_fd = poll_find_fd(poll, fd);
    if (poll_fd == NULL) {
        err = _ENOENT;
        goto out;
    }

    if (poll_fd_has_host_wait(poll_fd)) {
        err = real_poll_update(&poll->real, fd->real_fd, 0, poll_fd);
        if (err < 0) {
            err = errno_map();
            goto out;
        }
    }

    list_remove(&poll_fd->polls);
    list_remove(&poll_fd->fds);
    poll_fd_free(poll_fd);

    err = 0;
out:
    unlock(&poll->lock);
    unlock(&fd->poll_lock);
    return err;
}

int poll_mod_fd(struct poll *poll, struct fd *fd, int types, union poll_fd_info info) {
    int err;
    lock(&fd->poll_lock, 0);
    lock(&poll->lock, 0);
    struct poll_fd *poll_fd = poll_find_fd(poll, fd);
    if (poll_fd == NULL) {
        err = _ENOENT;
        goto out;
    }

    if (poll_fd_has_host_wait(poll_fd)) {
        err = real_poll_update(&poll->real, fd->real_fd, types, poll_fd);
        if (err < 0) {
            err = errno_map();
            goto out;
        }
    }

    poll_fd->types = types;
    poll_fd->info = info;
    poll_fd->triggered_types &= types;

    err = 0;
out:
    unlock(&poll->lock);
    unlock(&fd->poll_lock);
    return err;
}

void poll_cleanup_fd(struct fd *fd) {
    lock(&fd->poll_lock, 0);
    struct poll_fd *poll_fd, *tmp;
    list_for_each_entry_safe(&fd->poll_fds, poll_fd, tmp, polls) {
        lock(&poll_fd->poll->lock, 0);
        if (poll_fd_has_host_wait(poll_fd))
            real_poll_update(&poll_fd->poll->real, fd->real_fd, 0, poll_fd);
        list_remove(&poll_fd->polls);
        list_remove(&poll_fd->fds);
        unlock(&poll_fd->poll->lock);
        poll_fd_free(poll_fd);
    }
    unlock(&fd->poll_lock);
}

void poll_wakeup(struct fd *fd, int events) {
    struct poll_fd *poll_fd;
    lock(&fd->poll_lock, 0);
    list_for_each_entry(&fd->poll_fds, poll_fd, polls) {
        struct poll *poll = poll_fd->poll;
        lock(&poll->lock,0);
        if (poll_fd->types & POLL_EDGETRIGGERED)
            poll_fd->triggered_types &= ~events;
        if (poll->notify_pipe[1] != -1 && !poll->notify_pending) {
            ssize_t wrote;
            do {
                wrote = write(poll->notify_pipe[1], "", 1);
            } while (wrote < 0 && errno == EINTR);
            if (wrote >= 0 || errno == EAGAIN) {
                poll->notify_pending = true;
            } else if (wrote < 0) {
                FIXME("poll wake notify write failed: %s", strerror(errno));
            }
        }
        unlock(&poll->lock);
        // oneshot?
    }
    unlock(&fd->poll_lock);
}

int poll_wait(struct poll *poll_, poll_callback_t callback, void *context, struct timespec *timeout) {
    lock(&poll_->lock, 0);

    // acquire the pipe
    if (poll_->waiters++ == 0) {
        assert(poll_->notify_pipe[0] == -1 && poll_->notify_pipe[1] == -1);
        if (pipe(poll_->notify_pipe) < 0) {
            unlock(&poll_->lock);
            return errno_map();
        }
        fcntl(poll_->notify_pipe[0], F_SETFL, O_NONBLOCK);
        fcntl(poll_->notify_pipe[1], F_SETFL, O_NONBLOCK);
        real_poll_update(&poll_->real, poll_->notify_pipe[0], POLL_READ, NULL);
    }

    struct timespec deadline_storage = {0};
    struct timespec *deadline = NULL;
    if (timeout != NULL) {
        deadline_storage = timespec_add(timespec_now(CLOCK_MONOTONIC), *timeout);
        deadline = &deadline_storage;
    }

    int res = 0;
    while (true) {
        // check if any fds are ready
        struct poll_fd *poll_fd;
        res += poll_scan_ready_locked(poll_, callback, context);
        if (res > 0)
            break;

        lock(&current->sighand->lock,0);
        bool signal_pending = !!(current->pending & ~current->blocked);
        unlock(&current->sighand->lock);
        if (signal_pending) {
            res = _EINTR;
            break;
        }

        // wait for a ready notification
        list_for_each_entry(&poll_->poll_fds, poll_fd, fds) {
            sockrestart_begin_listen_wait(poll_fd->fd);
        }
        int err;
        struct real_poll_event e[4];
        do {
            unlock(&poll_->lock);
            sigset_t sigusr1, oldmask;
            sigemptyset(&sigusr1);
            sigaddset(&sigusr1, SIGUSR1);
            pthread_sigmask(SIG_BLOCK, &sigusr1, &oldmask);
            if (sigunwind_start()) {
                pthread_sigmask(SIG_SETMASK, &oldmask, NULL);
                errno = EINTR;
                err = -1;
            } else {
                lock(&current->sighand->lock, 0);
                bool signal_pending = !!(current->pending & ~current->blocked);
                unlock(&current->sighand->lock);
                if (signal_pending) {
                    sigunwind_end();
                    pthread_sigmask(SIG_SETMASK, &oldmask, NULL);
                    errno = EINTR;
                    err = -1;
                } else {
                    struct timespec remaining_timeout = {0};
                    struct timespec *wait_timeout = NULL;
                    struct timespec periodic_rescan_timeout = {
                        .tv_sec = 0,
                        .tv_nsec = 100 * 1000 * 1000L,
                    };
                    if (deadline != NULL) {
                        if (!poll_deadline_remaining(deadline, &remaining_timeout)) {
                            pthread_sigmask(SIG_SETMASK, &oldmask, NULL);
                            errno = 0;
                            err = 0;
                            sigunwind_end();
                            goto poll_wait_done;
                        }
                        wait_timeout = &remaining_timeout;
                    }
                    if (poll_needs_periodic_host_rescan(poll_)) {
                        if (wait_timeout == NULL ||
                                wait_timeout->tv_sec > periodic_rescan_timeout.tv_sec ||
                                (wait_timeout->tv_sec == periodic_rescan_timeout.tv_sec &&
                                 wait_timeout->tv_nsec > periodic_rescan_timeout.tv_nsec)) {
                            wait_timeout = &periodic_rescan_timeout;
                        }
                    }
                    pthread_sigmask(SIG_SETMASK, &oldmask, NULL);
                    if (poll_wait_trace_enabled()) {
                        fprintf(stderr, "ish-pollwait: sleep pid=%d comm=%s timeout=%lds.%09ld waiters=%d\n",
                               current->pid, current->comm,
                               wait_timeout != NULL ? wait_timeout->tv_sec : -1L,
                               wait_timeout != NULL ? wait_timeout->tv_nsec : -1L,
                               poll_->waiters);
                    }
                    err = real_poll_wait(&poll_->real, e, sizeof(e)/sizeof(e[0]), wait_timeout);
                    sigunwind_end();
                }
            }
poll_wait_done:
            lock(&poll_->lock, 0);
        } while (sockrestart_should_restart_listen_wait(1) && errno == EINTR);
        if (poll_wait_trace_enabled()) {
            fprintf(stderr, "ish-pollwait: wake pid=%d comm=%s err=%d errno=%d notify_pending=%d\n",
                   current->pid, current->comm, err, err < 0 ? errno : 0, poll_->notify_pending);
            if (err > 0) {
                for (int i = 0; i < err; i++) {
                    struct poll_fd *candidate = rpe_data(&e[i]);
                    struct poll_fd *triggered_poll_fd = poll_find_ptr(poll_, candidate);
#if HAVE_KQUEUE
                    if (triggered_poll_fd == NULL)
                        triggered_poll_fd = poll_find_real_fd(poll_, (int) e[i].real.ident);
#endif
                    if (triggered_poll_fd != NULL)
                        poll_wait_trace_fd(triggered_poll_fd, rpe_events(&e[i]), "host-event");
                    else {
                        poll_wait_trace_raw_event(poll_, &e[i], "raw-event");
                        poll_drop_unknown_event(poll_, &e[i]);
                    }
                }
            }
        }
        list_for_each_entry(&poll_->poll_fds, poll_fd, fds) {
            sockrestart_end_listen_wait(poll_fd->fd);
        }

        if (err < 0) {
            res = errno_map();
            break;
        }
        if (err == 0) {
            // Re-probe once before timing out. The host wait backend can miss
            // a transition even when a direct readiness probe already says the
            // fd is ready.
            res += poll_scan_ready_locked(poll_, callback, context);
            break;
        }

        // Deliver host readiness notifications directly. fd->ops->poll() is
        // still the preferred readiness source, but Darwin can report EOF/HUP
        // through kqueue when a follow-up zero-time probe returns no bits. If
        // we only rescan, that host event can wake us forever without ever
        // reaching the guest.
        for (int i = 0; i < err; i++) {
            struct poll_fd *candidate = rpe_data(&e[i]);
            struct poll_fd *triggered_poll_fd = poll_find_ptr(poll_, candidate);
#if HAVE_KQUEUE
            if (triggered_poll_fd == NULL)
                triggered_poll_fd = poll_find_real_fd(poll_, (int) e[i].real.ident);
#endif
            if (triggered_poll_fd == NULL || triggered_poll_fd->poll != poll_)
                continue;
            int host_events = rpe_events(&e[i]);
            if (triggered_poll_fd->types & POLL_EDGETRIGGERED)
                triggered_poll_fd->triggered_types &= ~host_events;
            int poll_types = host_events & (triggered_poll_fd->types | POLL_HUP | POLL_ERR | POLL_NVAL);
            res += poll_deliver_ready_locked(poll_, triggered_poll_fd, poll_types,
                                             callback, context, "host-callback");
        }

        while (poll_->notify_pipe[0] != -1) {
            char byte;
            ssize_t drained = read(poll_->notify_pipe[0], &byte, 1);
            if (drained > 0)
                continue;
            if (drained < 0 && errno != EAGAIN) {
                res = errno_map();
                break;
            }
            poll_->notify_pending = false;
            break;
        }
        if (res < 0)
            break;
        if (res > 0)
            break;
    }

    // release the pipe
    if (--poll_->waiters == 0) {
        close(poll_->notify_pipe[0]);
        close(poll_->notify_pipe[1]);
        poll_->notify_pipe[0] = -1;
        poll_->notify_pipe[1] = -1;
        poll_->notify_pending = false;
    }

    unlock(&poll_->lock);
    return res;
}

void poll_destroy(struct poll *poll) {
    struct poll_fd *poll_fd;
    struct poll_fd *tmp;

    list_for_each_entry_safe(&poll->poll_fds, poll_fd, tmp, fds) {
        lock(&poll_fd->fd->poll_lock, 0);
        list_remove(&poll_fd->polls);
        list_remove(&poll_fd->fds);
        unlock(&poll_fd->fd->poll_lock);
        free(poll_fd);
    }

    list_for_each_entry_safe(&poll->pollfd_freelist, poll_fd, tmp, fds) {
        list_remove(&poll_fd->fds);
        free(poll_fd);
    }

    real_poll_close(&poll->real);
    free(poll);
}

// Platform-specific real_poll implementations

#if HAVE_EPOLL

static int real_poll_init(struct real_poll *real) {
    real->fd = epoll_create1(0);
    if (real->fd < 0)
        return -1;
    return 0;
}

static int real_poll_wait(struct real_poll *real, struct real_poll_event *events, int max, struct timespec *timeout) {
    int timeout_millis = -1;
    if (timeout != NULL)
        timeout_millis = timeout->tv_sec * 1000 + timeout->tv_nsec / 1000000;
    return epoll_wait(real->fd, (struct epoll_event *) events, max, timeout_millis);
}

static int real_poll_update(struct real_poll *real, int fd, int types, void *data) {
    types &= ~EPOLLONESHOT;
    if (types == 0)
        return epoll_ctl(real->fd, EPOLL_CTL_DEL, fd, NULL);
    struct epoll_event epevent = {.events = types, .data.ptr = data};
    int err = epoll_ctl(real->fd, EPOLL_CTL_MOD, fd, &epevent);
    if (err < 0 && errno == ENOENT)
        err = epoll_ctl(real->fd, EPOLL_CTL_ADD, fd, &epevent);
    return err;
}

static void *rpe_data(struct real_poll_event *rpe) {
    return rpe->real.data.ptr;
}
static int rpe_events(struct real_poll_event *rpe) {
    return rpe->real.events;
}

#elif HAVE_KQUEUE

static int real_poll_init(struct real_poll *real) {
    real->fd = kqueue();
    if (real->fd < 0)
        return -1;
    return 0;
}

static int real_poll_check_receipts(struct kevent *events, int count) {
    for (int i = 0; i < count; i++) {
        if (!(events[i].flags & EV_ERROR))
            continue;
        if (events[i].data == 0)
            continue;
        // Deleting a filter that was never installed is harmless.
        if (events[i].data == ENOENT)
            continue;
        // EVFILT_EXCEPT is not supported for all Darwin fd types, including
        // regular files. Treat that as "filter unavailable" rather than
        // failing the whole poll registration.
        if (events[i].filter == EVFILT_EXCEPT &&
                (events[i].data == EINVAL || events[i].data == ENOTSUP || events[i].data == EPERM))
            continue;
        errno = (int) events[i].data;
        return -1;
    }
    return 0;
}

static int real_poll_update(struct real_poll *real, int fd, int types, void *data) {
    struct kevent e[3] = {
        {.filter = EVFILT_READ, .flags = types & (POLL_READ | POLL_HUP) ? EV_ADD : EV_DELETE},
        {.filter = EVFILT_WRITE, .flags = types & POLL_WRITE ? EV_ADD : EV_DELETE},
        {.filter = EVFILT_EXCEPT, .flags = types & POLL_ERR ? EV_ADD : EV_DELETE},
    };
    if (!(types & POLL_READ) && types & POLL_HUP) {
        // Set the low water mark really high so we'll only get woken up on a hangup
        e[0].fflags = NOTE_LOWAT;
        e[0].data = INT_MAX;
    }
    for (int i = 0; i < 3; i++) {
        e[i].ident = fd;
        e[i].udata = data;
        e[i].flags |= EV_RECEIPT;
        if (types & POLL_EDGETRIGGERED)
            e[i].flags |= EV_CLEAR;
    }

    int count = kevent(real->fd, e, 3, e, 3, NULL);
    if (count < 0)
        return -1;
    return real_poll_check_receipts(e, count);
}

static int real_poll_wait(struct real_poll *real, struct real_poll_event *events, int max, struct timespec *timeout) {
    return kevent(real->fd, NULL, 0, (struct kevent *) events, max, timeout);
}

static void *rpe_data(struct real_poll_event *rpe) {
    return rpe->real.udata;
}

static int rpe_events(struct real_poll_event *rpe) {
    if (rpe->real.flags & EV_ERROR) {
        int err = (int) rpe->real.data;
        if (err == 0)
            return POLL_ERR;
        if (err == EBADF || err == ENOENT)
            return POLL_NVAL;
        return POLL_ERR;
    }
    if (rpe->real.filter == EVFILT_READ) {
        int events = 0;
        if (rpe->real.data > 0)
            events |= POLL_READ;
        if (rpe->real.flags & EV_EOF)
            events |= POLL_HUP;
        return events;
    }
    if (rpe->real.filter == EVFILT_WRITE) return POLL_WRITE;
    if (rpe->real.filter == EVFILT_EXCEPT) return POLL_ERR;
    return 0;
}

#endif

static void real_poll_close(struct real_poll *real) {
    close(real->fd);
}
