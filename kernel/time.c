#ifdef __linux__
#define _GNU_SOURCE
#include <sys/resource.h>
#endif
#include "debug.h"
#include <time.h>
#include <signal.h>
#include <sys/time.h>
#include <string.h>
#include "kernel/calls.h"
#include "kernel/errno.h"
#include "kernel/resource.h"
#include "kernel/time.h"
#include "fs/poll.h"
#include "util/timer.h"
#include <limits.h>
#include <sys/poll.h>

static int clockid_to_real(uint_t clock, clockid_t *real) {
    switch (clock) {
        case CLOCK_REALTIME_:
        case CLOCK_REALTIME_COARSE_:
            *real = CLOCK_REALTIME; break;
        case CLOCK_MONOTONIC_:
        case CLOCK_MONOTONIC_COARSE_:
            *real = CLOCK_MONOTONIC; break;
        case CLOCK_PROCESS_CPUTIME_ID_:
            *real = CLOCK_PROCESS_CPUTIME_ID; break;
        case CLOCK_THREAD_CPUTIME_ID_:
            *real = CLOCK_THREAD_CPUTIME_ID; break;
        case CLOCK_MONOTONIC_RAW_:
#ifdef CLOCK_MONOTONIC_RAW
            *real = CLOCK_MONOTONIC_RAW; break;
#else
            *real = CLOCK_MONOTONIC; break;
#endif
        case CLOCK_BOOTTIME_:
            // CLOCK_BOOTTIME (includes suspend time) is boot-relative like
            // MONOTONIC; use the host's when available, else MONOTONIC.
#ifdef CLOCK_BOOTTIME
            *real = CLOCK_BOOTTIME; break;
#else
            *real = CLOCK_MONOTONIC; break;
#endif
        case CLOCK_TAI_:
            // CLOCK_TAI is EPOCH-based (UTC + leap-second offset), NOT
            // boot-relative — mapping it to MONOTONIC (as before) made
            // clock_gettime(CLOCK_TAI) return boot-relative seconds, decades
            // off real Linux. Map to REALTIME (off by only the ~37s leap
            // offset); iOS/macOS lack a native CLOCK_TAI anyway.
            *real = CLOCK_REALTIME; break;
        default: return _EINVAL;
    }
    return 0;
}

static struct timer_spec timer_spec_to_real(struct itimerspec_ itspec) {
    struct timer_spec spec = {
        .value.tv_sec = itspec.value.sec,
        .value.tv_nsec = itspec.value.nsec,
        .interval.tv_sec = itspec.interval.sec,
        .interval.tv_nsec = itspec.interval.nsec,
    };
    return spec;
};

static struct itimerspec_ timer_spec_from_real(struct timer_spec spec) {
    struct itimerspec_ itspec = {
        .value.sec = (dword_t)spec.value.tv_sec,
        .value.nsec = (dword_t)spec.value.tv_nsec,
        .interval.sec = (dword_t)spec.interval.tv_sec,
        .interval.nsec = (dword_t)spec.interval.tv_nsec,
    };
    return itspec;
};

static struct timer_spec timer_spec_to_real64(struct itimerspec64_ itspec) {
    struct timer_spec spec = {
        .value.tv_sec = itspec.value.sec,
        .value.tv_nsec = itspec.value.nsec,
        .interval.tv_sec = itspec.interval.sec,
        .interval.tv_nsec = itspec.interval.nsec,
    };
    return spec;
};

static struct itimerspec64_ timer_spec_from_real64(struct timer_spec spec) {
    struct itimerspec64_ itspec = {
        .value.sec = spec.value.tv_sec,
        .value.nsec = spec.value.tv_nsec,
        .interval.sec = spec.interval.tv_sec,
        .interval.nsec = spec.interval.tv_nsec,
    };
    return itspec;
};

#define TIMER_ABSTIME_ (1 << 0)

static int timespec_is_valid(struct timespec ts) {
    return ts.tv_sec >= 0 && ts.tv_nsec >= 0 && ts.tv_nsec < 1000000000;
}

static struct timespec timespec_from_guest(struct timespec_ ts) {
    return (struct timespec) {
        .tv_sec = ts.sec,
        .tv_nsec = ts.nsec,
    };
}

static struct timespec timespec_from_guest64(struct timespec64_ ts) {
    return (struct timespec) {
        .tv_sec = ts.sec,
        .tv_nsec = ts.nsec,
    };
}

static struct timespec_ timespec_to_guest(struct timespec ts) {
    return (struct timespec_) {
        .sec = (dword_t) ts.tv_sec,
        .nsec = (dword_t) ts.tv_nsec,
    };
}

static struct timespec64_ timespec_to_guest64(struct timespec ts) {
    return (struct timespec64_) {
        .sec = ts.tv_sec,
        .nsec = ts.tv_nsec,
    };
}

size_t guest_timeval_size(enum guest_abi abi) {
    return abi == GUEST_ABI_AMD64 ? sizeof(struct amd64_timeval_) : sizeof(struct timeval_);
}

size_t guest_timespec_size(enum guest_abi abi) {
    return abi == GUEST_ABI_AMD64 ? sizeof(struct timespec64_) : sizeof(struct timespec_);
}

int read_guest_timeval_abi(enum guest_abi abi, guest_addr_t addr, struct timeval *out) {
    if (abi == GUEST_ABI_AMD64) {
        struct amd64_timeval_ guest;
        if (user_get(addr, guest))
            return _EFAULT;
        out->tv_sec = guest.sec;
        out->tv_usec = guest.usec;
    } else {
        struct timeval_ guest;
        if (user_get(addr, guest))
            return _EFAULT;
        out->tv_sec = guest.sec;
        out->tv_usec = guest.usec;
    }
    return 0;
}

int write_guest_timeval_abi(enum guest_abi abi, guest_addr_t addr, const struct timeval *in) {
    if (abi == GUEST_ABI_AMD64) {
        struct amd64_timeval_ guest = {
            .sec = in->tv_sec,
            .usec = in->tv_usec,
        };
        if (user_put(addr, guest))
            return _EFAULT;
    } else {
        struct timeval_ guest = {
            .sec = (dword_t) in->tv_sec,
            .usec = (dword_t) in->tv_usec,
        };
        if (user_put(addr, guest))
            return _EFAULT;
    }
    return 0;
}

int read_guest_timespec_abi(enum guest_abi abi, guest_addr_t addr, struct timespec *out) {
    if (abi == GUEST_ABI_AMD64) {
        struct timespec64_ guest;
        if (user_get(addr, guest))
            return _EFAULT;
        *out = timespec_from_guest64(guest);
    } else {
        struct timespec_ guest;
        if (user_get(addr, guest))
            return _EFAULT;
        *out = timespec_from_guest(guest);
    }
    return 0;
}

int write_guest_timespec_abi(enum guest_abi abi, guest_addr_t addr, const struct timespec *in) {
    if (abi == GUEST_ABI_AMD64) {
        struct timespec64_ guest = timespec_to_guest64(*in);
        if (user_put(addr, guest))
            return _EFAULT;
    } else {
        struct timespec_ guest = timespec_to_guest(*in);
        if (user_put(addr, guest))
            return _EFAULT;
    }
    return 0;
}

static dword_t clock_nanosleep_common(dword_t clock, int_t flags, struct timespec req,
        guest_addr_t rem_addr, bool rem_time64) {
    clockid_t clock_id;
    if (clockid_to_real(clock, &clock_id))
        return _EINVAL;
    if (flags & ~TIMER_ABSTIME_)
        return _EINVAL;
    if (!timespec_is_valid(req))
        return _EINVAL;

    struct timespec rem = {0};
    if (flags & TIMER_ABSTIME_) {
        req = timespec_subtract(req, timespec_now(clock_id));
        if (!timespec_positive(req))
            return 0;
    }

    bool trace_short_sleep = req.tv_sec >= 0 && req.tv_sec <= 2;
    if (trace_short_sleep) {
        printk("INFO: wait clock_nanosleep enter pid=%d comm=%s clock=%u flags=%#x req=%llds.%09ld rem=%#x\n",
               current != NULL ? current->pid : -1,
               current != NULL ? current->comm : "?",
               clock, flags, (long long) req.tv_sec, req.tv_nsec, rem_addr);
    }

    int res;
    TASK_MAY_BLOCK {
        res = nanosleep(&req, &rem);
    }
    if (trace_short_sleep) {
        printk("INFO: wait clock_nanosleep exit pid=%d comm=%s res=%d rem=%llds.%09ld\n",
               current != NULL ? current->pid : -1,
               current != NULL ? current->comm : "?",
               res, (long long) rem.tv_sec, rem.tv_nsec);
    }
    if (res < 0) {
        int err = errno_map();
        // POSIX: an interrupted *relative* sleep reports the time remaining so
        // the caller (or its libc restart logic) can resume. The host nanosleep
        // already populated `rem`; hand it back. Best effort — still report
        // EINTR even if the rem store faults. (iSH previously dropped rem here,
        // so amd64 nanosleep left the caller's buffer untouched on EINTR.)
        if (err == _EINTR && rem_addr != 0 && !(flags & TIMER_ABSTIME_)) {
            if (rem_time64) {
                struct timespec64_ rem_ts = timespec_to_guest64(rem);
                (void) user_put(rem_addr, rem_ts);
            } else {
                struct timespec_ rem_ts = timespec_to_guest(rem);
                (void) user_put(rem_addr, rem_ts);
            }
        }
        return err;
    }

    if (rem_addr != 0 && !(flags & TIMER_ABSTIME_)) {
        if (rem_time64) {
            struct timespec64_ rem_ts = timespec_to_guest64(rem);
            if (user_put(rem_addr, rem_ts))
                return _EFAULT;
        } else {
            struct timespec_ rem_ts = timespec_to_guest(rem);
            if (user_put(rem_addr, rem_ts))
                return _EFAULT;
        }
    }
    return 0;
}

dword_t sys_clock_nanosleep(dword_t clock_id, int_t flags, addr_t req_addr, addr_t rem_addr) {
    return sys_clock_nanosleep_guest(clock_id, flags, req_addr, rem_addr);
}

static dword_t sys_clock_nanosleep_guest_abi(dword_t clock_id, int_t flags, guest_addr_t req_addr,
        guest_addr_t rem_addr, enum guest_abi abi) {
    struct timespec req_ts;
    if (read_guest_timespec_abi(abi, req_addr, &req_ts))
        return _EFAULT;
    STRACE("clock_nanosleep(%d, %#x, {%lld, %ld}, %#x)", clock_id, flags,
            (long long) req_ts.tv_sec, req_ts.tv_nsec, rem_addr);
    return clock_nanosleep_common(clock_id, flags, req_ts, rem_addr, abi == GUEST_ABI_AMD64);
}

dword_t sys_clock_nanosleep_guest(dword_t clock_id, int_t flags, guest_addr_t req_addr, guest_addr_t rem_addr) {
    return sys_clock_nanosleep_guest_abi(clock_id, flags, req_addr, rem_addr, GUEST_ABI_I386);
}

dword_t sys_clock_nanosleep_amd64(dword_t clock_id, int_t flags, addr_t req_addr, addr_t rem_addr) {
    return sys_clock_nanosleep_guest_abi(clock_id, flags, req_addr, rem_addr, GUEST_ABI_AMD64);
}

dword_t sys_clock_nanosleep_amd64_guest(dword_t clock_id, int_t flags, guest_addr_t req_addr, guest_addr_t rem_addr) {
    return sys_clock_nanosleep_guest_abi(clock_id, flags, req_addr, rem_addr, GUEST_ABI_AMD64);
}

dword_t sys_clock_nanosleep_time64(dword_t clock_id, int_t flags, addr_t req_addr, addr_t rem_addr) {
    return sys_clock_nanosleep_time64_guest(clock_id, flags, req_addr, rem_addr);
}

dword_t sys_clock_nanosleep_time64_guest(dword_t clock_id, int_t flags, guest_addr_t req_addr, guest_addr_t rem_addr) {
    struct timespec64_ req_ts;
    if (user_get(req_addr, req_ts))
        return _EFAULT;
    STRACE("clock_nanosleep_time64(%d, %#x, {%lld, %lld}, %#x)", clock_id, flags,
            (long long) req_ts.sec, (long long) req_ts.nsec, rem_addr);
    return clock_nanosleep_common(clock_id, flags, timespec_from_guest64(req_ts), rem_addr, true);
}

dword_t sys_ppoll_time64(addr_t fds, dword_t nfds, addr_t timeout_addr, addr_t sigmask_addr, dword_t sigsetsize) {
    struct timespec timeout_ts = {};
    const struct timespec *timeout_ptr = NULL;
    int timeout_ms = -1;
    if (timeout_addr != 0) {
        struct timespec64_ timeout_timespec;
        if (user_get(timeout_addr, timeout_timespec))
            return _EFAULT;
        if (timeout_timespec.sec < 0 || timeout_timespec.nsec < 0 || timeout_timespec.nsec >= 1000000000)
            return _EINVAL;

        int64_t timeout_ms64 = timeout_timespec.sec * 1000 + timeout_timespec.nsec / 1000000;
        timeout_ms = timeout_ms64 > INT_MAX ? INT_MAX : (int) timeout_ms64;
        timeout_ts.tv_sec = timeout_timespec.sec;
        timeout_ts.tv_nsec = timeout_timespec.nsec;
        timeout_ptr = &timeout_ts;
    }

    sigset_t_ mask;
    if (sigmask_addr != 0) {
        if (sigsetsize != sizeof(sigset_t_))
            return _EINVAL;
        if (user_get(sigmask_addr, mask))
            return _EFAULT;
        sigmask_set_temp(mask);
    }

    return sys_poll_common(fds, nfds, timeout_ptr, timeout_ms);
}

dword_t sys_time(addr_t time_out) {
    qword_t now = sys_time_guest(time_out);
    if ((dword_t) now != now)
        return _EOVERFLOW;
    return (dword_t) now;
}

qword_t sys_time_guest(guest_addr_t time_out) {
    qword_t now = (qword_t) time(NULL);
    if (time_out != 0) {
        dword_t now32 = (dword_t) now;
        if (user_put(time_out, now32))
            return (qword_t) (sqword_t) _EFAULT;
    }
    return now;
}

qword_t sys_time_amd64_guest(guest_addr_t time_out) {
    qword_t now = (qword_t) time(NULL);
    if (time_out != 0) {
        if (user_put(time_out, now))
            return (qword_t) (sqword_t) _EFAULT;
    }
    return now;
}

qword_t sys_time_amd64(addr_t time_out) {
    return sys_time_amd64_guest(time_out);
}

dword_t sys_stime(addr_t UNUSED(time)) {
    return _EPERM;
}

dword_t sys_clock_gettime(dword_t clock, addr_t tp) {
    return sys_clock_gettime_guest(clock, tp);
}

static dword_t sys_clock_gettime_guest_abi(dword_t clock, guest_addr_t tp, enum guest_abi abi) {
    STRACE("clock_gettime(%d, 0x%x)", clock, tp);

    struct timespec ts;
    if (clock == CLOCK_PROCESS_CPUTIME_ID_) {
        // FIXME this is thread usage, not process usage
        struct rusage_ rusage = rusage_get_current();
        ts.tv_sec = rusage.utime.sec;
        ts.tv_nsec = rusage.utime.usec * 1000;
    } else {
        clockid_t clock_id;
        if (clockid_to_real(clock, &clock_id)) return _EINVAL;
        int err = clock_gettime(clock_id, &ts);
        if (err < 0)
            return errno_map();
    }
    if (write_guest_timespec_abi(abi, tp, &ts))
        return _EFAULT;
    return 0;
}

dword_t sys_clock_gettime_guest(dword_t clock, guest_addr_t tp) {
    return sys_clock_gettime_guest_abi(clock, tp, GUEST_ABI_I386);
}

dword_t sys_clock_gettime_amd64(dword_t clock, addr_t tp) {
    return sys_clock_gettime_guest_abi(clock, tp, GUEST_ABI_AMD64);
}

dword_t sys_clock_gettime_amd64_guest(dword_t clock, guest_addr_t tp) {
    return sys_clock_gettime_guest_abi(clock, tp, GUEST_ABI_AMD64);
}

dword_t sys_clock_gettime64(dword_t clock, addr_t tp) {
    return sys_clock_gettime64_guest(clock, tp);
}

dword_t sys_clock_gettime64_guest(dword_t clock, guest_addr_t tp) {
    STRACE("clock_gettime64(%d, 0x%x)", clock, tp);

    struct timespec ts;
    if (clock == CLOCK_PROCESS_CPUTIME_ID_) {
        // FIXME this is thread usage, not process usage
        struct rusage_ rusage = rusage_get_current();
        ts.tv_sec = rusage.utime.sec;
        ts.tv_nsec = rusage.utime.usec * 1000;
    } else {
        clockid_t clock_id;
        if (clockid_to_real(clock, &clock_id)) return _EINVAL;
        int err = clock_gettime(clock_id, &ts);
        if (err < 0)
            return errno_map();
    }
    struct timespec64_ t = timespec_to_guest64(ts);
    
    if (user_put(tp, t))
        return _EFAULT;
    STRACE(" {%ld s %ld ns}", (long)t.sec, (long)t.nsec);
    return 0;
}


dword_t sys_clock_getres(dword_t clock, addr_t res_addr) {
    return sys_clock_getres_guest(clock, res_addr);
}

static dword_t sys_clock_getres_guest_abi(dword_t clock, guest_addr_t res_addr, enum guest_abi abi) {
    STRACE("clock_getres(%d, %#x)", clock, res_addr);
    clockid_t clock_id;
    if (clockid_to_real(clock, &clock_id)) return _EINVAL;

    struct timespec res;
    int err = clock_getres(clock_id, &res);
    if (err < 0)
        return errno_map();
    if (write_guest_timespec_abi(abi, res_addr, &res))
        return _EFAULT;
    return 0;
}

dword_t sys_clock_getres_guest(dword_t clock, guest_addr_t res_addr) {
    return sys_clock_getres_guest_abi(clock, res_addr, GUEST_ABI_I386);
}

dword_t sys_clock_getres_amd64(dword_t clock, addr_t res_addr) {
    return sys_clock_getres_guest_abi(clock, res_addr, GUEST_ABI_AMD64);
}

dword_t sys_clock_getres_amd64_guest(dword_t clock, guest_addr_t res_addr) {
    return sys_clock_getres_guest_abi(clock, res_addr, GUEST_ABI_AMD64);
}

dword_t sys_clock_getres_time64(dword_t clock, addr_t res_addr) {
    return sys_clock_getres_time64_guest(clock, res_addr);
}

dword_t sys_clock_getres_time64_guest(dword_t clock, guest_addr_t res_addr) {
    STRACE("clock_getres_time64(%d, %#x)", clock, res_addr);
    clockid_t clock_id;
    if (clockid_to_real(clock, &clock_id))
        return _EINVAL;

    struct timespec res;
    int err = clock_getres(clock_id, &res);
    if (err < 0)
        return errno_map();
    struct timespec64_ t = timespec_to_guest64(res);
    if (user_put(res_addr, t))
        return _EFAULT;
    return 0;
}

// iSH never has the privilege to change the host wall clock, so settable
// clocks return EPERM. But Linux distinguishes by clock id: a clock that can
// never be set (MONOTONIC, BOOTTIME, the CPU-time and COARSE clocks, ...)
// returns EINVAL regardless of privilege, and an unknown clock id also returns
// EINVAL. Only CLOCK_REALTIME is settable -> EPERM here. (Previously every
// clock returned EPERM, so clock_settime(CLOCK_MONOTONIC) was EPERM not EINVAL.)
static dword_t clock_settime_errno(dword_t clock) {
    return clock == CLOCK_REALTIME_ ? _EPERM : _EINVAL;
}

dword_t sys_clock_settime(dword_t clock, addr_t UNUSED(tp)) {
    return clock_settime_errno(clock);
}

dword_t sys_clock_settime64(dword_t clock, addr_t UNUSED(tp)) {
    return clock_settime_errno(clock);
}

static bool time_warning_trace_enabled(void) {
    return false;
}

static void itimer_notify(struct task *task) {
    struct siginfo_ info = {
        .code = SI_TIMER_,
    };
    if (time_warning_trace_enabled())
        printk("WARNING: itimer_notify pid=%d tgid=%d comm=%s sig=%d pending=%#llx blocked=%#llx\n",
               task->pid, task->tgid, task->comm, SIGALRM_,
               (unsigned long long) task->pending,
               (unsigned long long) task->blocked);
    send_signal(task, SIGALRM_, info);
}

static long itimer_set(struct tgroup *group, int which, struct timer_spec spec, struct timer_spec *old_spec) {
    if (which != ITIMER_REAL_) {
        FIXME("unimplemented setitimer %d", which);
        return _EINVAL;
    }

    if (!group->itimer) {
        struct timer *timer = timer_new(CLOCK_REALTIME, (timer_callback_t) itimer_notify, current);
        if (IS_ERR(timer))
            return PTR_ERR(timer);
        group->itimer = timer;
        if (time_warning_trace_enabled())
            printk("WARNING: itimer_create pid=%d tgid=%d comm=%s timer=%p which=%d\n",
                   current->pid, current->tgid, current->comm, (void *) timer, which);
    }

    if (time_warning_trace_enabled())
        printk("WARNING: itimer_set pid=%d tgid=%d comm=%s which=%d value=%lds.%09ld interval=%lds.%09ld timer=%p\n",
               current->pid, current->tgid, current->comm, which,
               (long) spec.value.tv_sec, spec.value.tv_nsec,
               (long) spec.interval.tv_sec, spec.interval.tv_nsec,
               (void *) group->itimer);

    return timer_set(group->itimer, spec, old_spec);
}

struct amd64_itimerval_ {
    struct amd64_timeval_ interval;
    struct amd64_timeval_ value;
};

long sys_setitimer(int_t which, addr_t new_val_addr, addr_t old_val_addr) {
    return sys_setitimer_guest(which, new_val_addr, old_val_addr);
}

static long sys_setitimer_guest_abi(int_t which, guest_addr_t new_val_addr, guest_addr_t old_val_addr,
        enum guest_abi abi) {
    struct timer_spec spec = {};
    if (abi == GUEST_ABI_AMD64) {
        struct amd64_itimerval_ val;
        if (user_get(new_val_addr, val))
            return _EFAULT;
        STRACE("setitimer(%d, {%llds %lldus, %llds %lldus}, %#llx)", which,
                (long long) val.value.sec, (long long) val.value.usec,
                (long long) val.interval.sec, (long long) val.interval.usec,
                (unsigned long long) old_val_addr);
        spec = (struct timer_spec) {
            .interval.tv_sec = val.interval.sec,
            .interval.tv_nsec = val.interval.usec * 1000,
            .value.tv_sec = val.value.sec,
            .value.tv_nsec = val.value.usec * 1000,
        };
    } else {
        struct itimerval_ val;
        if (user_get(new_val_addr, val))
            return _EFAULT;
        STRACE("setitimer(%d, {%ds %dus, %ds %dus}, %#llx)", which,
                val.value.sec, val.value.usec, val.interval.sec, val.interval.usec,
                (unsigned long long) old_val_addr);
        spec = (struct timer_spec) {
            .interval.tv_sec = val.interval.sec,
            .interval.tv_nsec = val.interval.usec * 1000,
            .value.tv_sec = val.value.sec,
            .value.tv_nsec = val.value.usec * 1000,
        };
    }
    struct timer_spec old_spec;

    struct tgroup *group = current->group;
    lock(&group->lock, 0);
    long err = itimer_set(group, which, spec, &old_spec);
    unlock(&group->lock);
    if (err < 0)
        return err;

    if (old_val_addr != 0) {
        if (abi == GUEST_ABI_AMD64) {
            struct amd64_itimerval_ old_val = {
                .interval.sec = old_spec.interval.tv_sec,
                .interval.usec = old_spec.interval.tv_nsec / 1000,
                .value.sec = old_spec.value.tv_sec,
                .value.usec = old_spec.value.tv_nsec / 1000,
            };
            if (user_put(old_val_addr, old_val))
                return _EFAULT;
        } else {
            struct itimerval_ old_val = {
                .interval.sec = (dword_t) old_spec.interval.tv_sec,
                .interval.usec = (dword_t) old_spec.interval.tv_nsec / 1000,
                .value.sec = (dword_t) old_spec.value.tv_sec,
                .value.usec = (dword_t) old_spec.value.tv_nsec / 1000,
            };
            if (user_put(old_val_addr, old_val))
                return _EFAULT;
        }
    }

    return 0;
}

long sys_setitimer_guest(int_t which, guest_addr_t new_val_addr, guest_addr_t old_val_addr) {
    return sys_setitimer_guest_abi(which, new_val_addr, old_val_addr, GUEST_ABI_I386);
}

long sys_setitimer_amd64(int_t which, addr_t new_val_addr, addr_t old_val_addr) {
    return sys_setitimer_guest_abi(which, new_val_addr, old_val_addr, GUEST_ABI_AMD64);
}

long sys_setitimer_amd64_guest(int_t which, guest_addr_t new_val_addr, guest_addr_t old_val_addr) {
    return sys_setitimer_guest_abi(which, new_val_addr, old_val_addr, GUEST_ABI_AMD64);
}

// getitimer: report the interval timer currently armed for `which`. iSH only
// implements ITIMER_REAL (the wall-clock SIGALRM timer); ITIMER_VIRTUAL/PROF
// can never have been armed (setitimer rejects them) so they read back as
// {0,0} — exactly what Linux reports for an unset timer. An invalid `which`
// is EINVAL. (getitimer was entirely unwired before: i386 #105 / amd64 #36
// raised SIGSYS, so any program polling its interval timer crashed.)
static long sys_getitimer_guest_abi(int_t which, guest_addr_t old_val_addr, enum guest_abi abi) {
    STRACE("getitimer(%d, %#llx)", which, (unsigned long long) old_val_addr);
    if (which != ITIMER_REAL_ && which != ITIMER_VIRTUAL_ && which != ITIMER_PROF_)
        return _EINVAL;

    struct timer_spec spec = {};
    struct tgroup *group = current->group;
    lock(&group->lock, 0);
    if (which == ITIMER_REAL_ && group->itimer != NULL) {
        struct timer *timer = group->itimer;
        lock(&timer->lock, 0);
        spec.interval = timer->interval;
        if (timer->active) {
            struct timespec remaining = timespec_subtract(timer->end,
                timespec_now(timer->clockid));
            if (timespec_positive(remaining))
                spec.value = remaining;
        }
        unlock(&timer->lock);
    }
    unlock(&group->lock);

    if (old_val_addr != 0) {
        if (abi == GUEST_ABI_AMD64) {
            struct amd64_itimerval_ val = {
                .interval.sec = spec.interval.tv_sec,
                .interval.usec = spec.interval.tv_nsec / 1000,
                .value.sec = spec.value.tv_sec,
                .value.usec = spec.value.tv_nsec / 1000,
            };
            if (user_put(old_val_addr, val))
                return _EFAULT;
        } else {
            struct itimerval_ val = {
                .interval.sec = (dword_t) spec.interval.tv_sec,
                .interval.usec = (dword_t) (spec.interval.tv_nsec / 1000),
                .value.sec = (dword_t) spec.value.tv_sec,
                .value.usec = (dword_t) (spec.value.tv_nsec / 1000),
            };
            if (user_put(old_val_addr, val))
                return _EFAULT;
        }
    }
    return 0;
}

long sys_getitimer(int_t which, addr_t old_val_addr) {
    return sys_getitimer_guest_abi(which, old_val_addr, GUEST_ABI_I386);
}

long sys_getitimer_guest(int_t which, guest_addr_t old_val_addr) {
    return sys_getitimer_guest_abi(which, old_val_addr, GUEST_ABI_I386);
}

long sys_getitimer_amd64(int_t which, addr_t old_val_addr) {
    return sys_getitimer_guest_abi(which, old_val_addr, GUEST_ABI_AMD64);
}

long sys_getitimer_amd64_guest(int_t which, guest_addr_t old_val_addr) {
    return sys_getitimer_guest_abi(which, old_val_addr, GUEST_ABI_AMD64);
}

long sys_alarm(uint_t seconds) {
    STRACE("alarm(%d)", seconds);
    if (time_warning_trace_enabled())
        printk("WARNING: alarm pid=%d tgid=%d comm=%s seconds=%u\n",
               current->pid, current->tgid, current->comm, seconds);
    struct timer_spec spec = {
        .value.tv_sec = seconds,
    };
    struct timer_spec old_spec;

    struct tgroup *group = current->group;
    lock(&group->lock, 0);
    long err = itimer_set(group, ITIMER_REAL_, spec, &old_spec);
    unlock(&group->lock);
    if (err < 0)
        return err;

    // Round up, and make sure to not return 0 if old_spec is > 0
    seconds = (dword_t)old_spec.value.tv_sec;
    if (old_spec.value.tv_nsec >= 500000000)
        seconds++;
    if (seconds == 0 && !timespec_is_zero(old_spec.value))
        seconds = 1;
    return seconds;
}

dword_t sys_nanosleep(addr_t req_addr, addr_t rem_addr) {
    return sys_nanosleep_guest(req_addr, rem_addr);
}

static dword_t sys_nanosleep_guest_abi(guest_addr_t req_addr, guest_addr_t rem_addr, enum guest_abi abi) {
    struct timespec req_ts;
    if (read_guest_timespec_abi(abi, req_addr, &req_ts))
        return _EFAULT;
    STRACE("nanosleep({%lld, %ld}, 0x%x", (long long) req_ts.tv_sec, req_ts.tv_nsec, rem_addr);
    bool trace_short_sleep = req_ts.tv_sec >= 0 && req_ts.tv_sec <= 2;
    if (trace_short_sleep) {
        printk("INFO: wait nanosleep enter pid=%d comm=%s req=%llds.%09ld rem=%#x\n",
               current != NULL ? current->pid : -1,
               current != NULL ? current->comm : "?",
               (long long) req_ts.tv_sec, req_ts.tv_nsec, rem_addr);
    }
    struct timespec rem;
   // rem.tv_sec = 0;
    //rem.tv_nsec = 0;
    int res = 0;
    TASK_MAY_BLOCK {
        res = nanosleep(&req_ts, &rem);
    }
    if (trace_short_sleep) {
        printk("INFO: wait nanosleep exit pid=%d comm=%s res=%d rem=%llds.%09ld\n",
               current != NULL ? current->pid : -1,
               current != NULL ? current->comm : "?",
               res, (long long) rem.tv_sec, rem.tv_nsec);
    }
    if (res < 0) {
        int err = errno_map();
        // On EINTR report the remaining time (Linux does); best effort.
        if (err == _EINTR && rem_addr != 0)
            (void) write_guest_timespec_abi(abi, rem_addr, &rem);
        return err;
    }
    if (rem_addr != 0 && write_guest_timespec_abi(abi, rem_addr, &rem))
        return _EFAULT;
    return 0;
}

dword_t sys_nanosleep_guest(guest_addr_t req_addr, guest_addr_t rem_addr) {
    return sys_nanosleep_guest_abi(req_addr, rem_addr, GUEST_ABI_I386);
}

dword_t sys_nanosleep_amd64(addr_t req_addr, addr_t rem_addr) {
    return sys_nanosleep_guest_abi(req_addr, rem_addr, GUEST_ABI_AMD64);
}

dword_t sys_nanosleep_amd64_guest(guest_addr_t req_addr, guest_addr_t rem_addr) {
    return sys_nanosleep_guest_abi(req_addr, rem_addr, GUEST_ABI_AMD64);
}

dword_t sys_times_guest(guest_addr_t tbuf) {
    STRACE("times(0x%x)", tbuf);
    if (tbuf) {
        struct rusage_ rusage = rusage_get_current();
        clock_t_ utime = clock_from_timeval(rusage.utime);
        clock_t_ stime = clock_from_timeval(rusage.stime);
        if (current->abi == GUEST_ABI_AMD64) {
            // amd64 struct tms has 64-bit (long) fields, vs the 32-bit i386 layout.
            struct amd64_tms { qword_t utime, stime, cutime, cstime; } tmp = {
                .utime = utime, .stime = stime, .cutime = utime, .cstime = stime,
            };
            if (user_put(tbuf, tmp))
                return _EFAULT;
        } else {
            struct tms_ tmp;
            tmp.tms_utime = utime;
            tmp.tms_stime = stime;
            tmp.tms_cutime = utime;
            tmp.tms_cstime = stime;
            if (user_put(tbuf, tmp))
                return _EFAULT;
        }
    }
    return 0;
}

dword_t sys_times(addr_t tbuf) {
    return sys_times_guest(tbuf);
}

dword_t sys_gettimeofday(addr_t tv, addr_t tz) {
    STRACE("gettimeofday(0x%x, 0x%x)", tv, tz);
    struct timeval timeval;
    struct timezone timezone;
    if (gettimeofday(&timeval, &timezone) < 0) {
        return errno_map();
    }
    struct timezone_ tz_;
    tz_.minuteswest = timezone.tz_minuteswest;
    tz_.dsttime = timezone.tz_dsttime;
    if ((tv && write_guest_timeval_abi(GUEST_ABI_I386, tv, &timeval)) || (tz && user_put(tz, tz_))) {
        return _EFAULT;
    }
    return 0;
}

dword_t sys_gettimeofday_guest(guest_addr_t tv, guest_addr_t tz) {
    return sys_gettimeofday(tv, tz);
}

dword_t sys_gettimeofday_amd64_guest(guest_addr_t tv, guest_addr_t tz) {
    STRACE("gettimeofday(0x%x, 0x%x)", tv, tz);
    struct timeval timeval;
    struct timezone timezone;
    if (gettimeofday(&timeval, &timezone) < 0) {
        return errno_map();
    }
    struct timezone_ tz_;
    tz_.minuteswest = timezone.tz_minuteswest;
    tz_.dsttime = timezone.tz_dsttime;
    if ((tv && write_guest_timeval_abi(GUEST_ABI_AMD64, tv, &timeval)) || (tz && user_put(tz, tz_))) {
        return _EFAULT;
    }
    return 0;
}

dword_t sys_gettimeofday_amd64(addr_t tv, addr_t tz) {
    return sys_gettimeofday_amd64_guest(tv, tz);
}

dword_t sys_settimeofday(addr_t UNUSED(tv), addr_t UNUSED(tz)) {
    return _EPERM;
}

static void posix_timer_callback(struct posix_timer *timer) {
    if (timer->tgroup == NULL)
        return;
    struct siginfo_ info = {
        .code = SI_TIMER_,
        .timer.timer = timer->timer_id,
        .timer.overrun = 0,
        .timer.value = timer->sig_value,
    };
    struct task *thread = NULL;
    if (timer->thread_pid != 0) {
        thread = pid_get_task_ref(timer->thread_pid);
    } else if (timer->tgroup->leader != NULL) {
        // SIGEV_SIGNAL targets the process, so fall back to the thread-group leader.
        thread = timer->tgroup->leader;
        task_ref_cnt_mod(thread, 1);
    }
    if (time_warning_trace_enabled())
        printk("WARNING: posix_timer_callback timer_id=%d signal=%d thread_pid=%d target_pid=%d found=%d\n",
               timer->timer_id, timer->signal, timer->thread_pid,
               thread != NULL ? thread->pid : 0, thread != NULL);
    // TODO: solve pid reuse. currently we have two ways of referring to a task: pid_t_ and struct task *. pids get reused. task struct pointers get freed on exit or reap. need a third option for cases like this, like a refcount layer.
    if (thread != NULL) {
        send_signal(thread, timer->signal, info);
        task_ref_cnt_mod(thread, -1);
    }
}

#define SIGEV_SIGNAL_ 0
#define SIGEV_NONE_ 1
#define SIGEV_THREAD_ID_ 4

struct i386_sigevent_marshaled {
    union i386_sigval_ value;
    int_t signo;
    int_t method;
    pid_t_ tid;
};

struct amd64_sigevent_marshaled {
    union sigval_ value;
    int_t signo;
    int_t method;
    pid_t_ tid;
    dword_t __pad;
};

int_t sys_timer_create(dword_t clock, addr_t sigevent_addr, addr_t timer_addr) {
    return sys_timer_create_guest(clock, sigevent_addr, timer_addr);
}

static int_t sys_timer_create_guest_abi(dword_t clock, guest_addr_t sigevent_addr, guest_addr_t timer_addr,
        enum guest_abi abi) {
    STRACE("timer_create(%d, %#x, %#x)", clock, sigevent_addr, timer_addr);
    if (time_warning_trace_enabled())
        printk("WARNING: timer_create pid=%d tgid=%d comm=%s clock=%u sigevent=%#x timer_addr=%#x\n",
               current->pid, current->tgid, current->comm, clock, sigevent_addr, timer_addr);
    clockid_t real_clockid;
    if (clockid_to_real(clock, &real_clockid))
        return _EINVAL;
    struct sigevent_ sigev = {};
    if (abi == GUEST_ABI_AMD64) {
        struct amd64_sigevent_marshaled user_sigev;
        if (user_get(sigevent_addr, user_sigev))
            return _EFAULT;
        sigev = (struct sigevent_) {
            .value = user_sigev.value,
            .signo = user_sigev.signo,
            .method = user_sigev.method,
            .tid = user_sigev.tid,
        };
    } else {
        struct i386_sigevent_marshaled user_sigev;
        if (user_get(sigevent_addr, user_sigev))
            return _EFAULT;
        dword_t raw_value = 0;
        memcpy(&raw_value, &user_sigev.value, sizeof(raw_value));
        sigev = (struct sigevent_) {
            .value.sv_ptr = raw_value,
            .signo = user_sigev.signo,
            .method = user_sigev.method,
            .tid = user_sigev.tid,
        };
    }
    if (sigev.method != SIGEV_SIGNAL_ && sigev.method != SIGEV_NONE_ && sigev.method != SIGEV_THREAD_ID_)
        return _EINVAL;

    if (sigev.method == SIGEV_THREAD_ID_) {
        struct task *target = pid_get_task_ref(sigev.tid);
        if (target == NULL) {
            return _EINVAL;
        }
        task_ref_cnt_mod(target, -1);
    }

    struct tgroup *group = current->group;
    lock(&group->lock, 0);
    unsigned timer_id;
    for (timer_id = 0; timer_id < TIMERS_MAX; timer_id++) {
        if (group->posix_timers[timer_id].timer == NULL)
            break;
    }
    if (timer_id >= TIMERS_MAX) {
        unlock(&group->lock);
        return _ENOMEM;
    }
    if (user_put(timer_addr, timer_id)) {
        unlock(&group->lock);
        return _EFAULT;
    }

    struct posix_timer *timer = &group->posix_timers[timer_id];
    timer->timer_id = timer_id;
    timer->timer = timer_new(real_clockid, (timer_callback_t) posix_timer_callback, timer);
    timer->signal = sigev.signo;
    timer->sig_value = sigev.value;
    timer->tgroup = NULL;
    if (sigev.method == SIGEV_SIGNAL_) {
        timer->tgroup = group;
        timer->thread_pid = 0;
    } else if (sigev.method == SIGEV_THREAD_ID_) {
        timer->tgroup = group;
        timer->thread_pid = sigev.tid;
    }
    unlock(&group->lock);
    return 0;
}

int_t sys_timer_create_guest(dword_t clock, guest_addr_t sigevent_addr, guest_addr_t timer_addr) {
    return sys_timer_create_guest_abi(clock, sigevent_addr, timer_addr, GUEST_ABI_I386);
}

int_t sys_timer_create_amd64(dword_t clock, addr_t sigevent_addr, addr_t timer_addr) {
    return sys_timer_create_guest_abi(clock, sigevent_addr, timer_addr, GUEST_ABI_AMD64);
}

int_t sys_timer_create_amd64_guest(dword_t clock, guest_addr_t sigevent_addr, guest_addr_t timer_addr) {
    return sys_timer_create_guest_abi(clock, sigevent_addr, timer_addr, GUEST_ABI_AMD64);
}

static int_t sys_timer_gettime_common(dword_t timer_id, guest_addr_t curr_value_addr, bool time64) {
    STRACE("timer_gettime(%d, %#x)", timer_id, curr_value_addr);
    if (timer_id >= TIMERS_MAX)
        return _EINVAL;

    lock(&current->group->lock, 0);
    struct posix_timer *timer = &current->group->posix_timers[timer_id];
    if (timer->timer == NULL) {
        unlock(&current->group->lock);
        return _EINVAL;
    }

    struct timer_spec spec = {};
    lock(&timer->timer->lock, 0);
    spec.interval = timer->timer->interval;
    if (timer->timer->active) {
        struct timespec remaining = timespec_subtract(timer->timer->end,
            timespec_now(timer->timer->clockid));
        if (timespec_positive(remaining))
            spec.value = remaining;
    }
    unlock(&timer->timer->lock);
    unlock(&current->group->lock);

    if (!time64) {
        struct itimerspec_ value = timer_spec_from_real(spec);
        if (user_put(curr_value_addr, value))
            return _EFAULT;
    } else {
        struct itimerspec64_ value = timer_spec_from_real64(spec);
        if (user_put(curr_value_addr, value))
            return _EFAULT;
    }
    return 0;
}

int_t sys_timer_gettime(dword_t timer_id, addr_t curr_value_addr) {
    return sys_timer_gettime_guest(timer_id, curr_value_addr);
}

int_t sys_timer_gettime_guest(dword_t timer_id, guest_addr_t curr_value_addr) {
    return sys_timer_gettime_common(timer_id, curr_value_addr, false);
}

int_t sys_timer_getoverrun(dword_t timer_id) {
    if (timer_id >= TIMERS_MAX)
        return _EINVAL;

    lock(&current->group->lock, 0);
    struct posix_timer *timer = &current->group->posix_timers[timer_id];
    bool valid = timer->timer != NULL;
    unlock(&current->group->lock);
    if (!valid)
        return _EINVAL;
    return 0;
}

static int_t sys_timer_settime_common(dword_t timer_id, int_t flags, guest_addr_t new_value_addr,
        guest_addr_t old_value_addr,
        bool time64) {
    STRACE("timer_settime(%d, %d, %#x, %#x)", timer_id, flags, new_value_addr, old_value_addr);
    if (time_warning_trace_enabled())
        printk("WARNING: timer_settime pid=%d tgid=%d comm=%s timer_id=%u flags=%d new=%#x old=%#x time64=%d\n",
               current->pid, current->tgid, current->comm, timer_id, flags, new_value_addr, old_value_addr, time64);
    if (timer_id >= TIMERS_MAX)
        return _EINVAL;

    struct timer_spec spec;
    if (!time64) {
        struct itimerspec_ value;
        if (user_get(new_value_addr, value))
            return _EFAULT;
        spec = timer_spec_to_real(value);
    } else {
        struct itimerspec64_ value;
        if (user_get(new_value_addr, value))
            return _EFAULT;
        spec = timer_spec_to_real64(value);
    }

    lock(&current->group->lock, 0);
    struct posix_timer *timer = &current->group->posix_timers[timer_id];
    if (timer->timer == NULL) {
        unlock(&current->group->lock);
        return _EINVAL;
    }
    struct timer_spec old_spec;
    if (flags & TIMER_ABSTIME_) {
        struct timespec now = timespec_now(timer->timer->clockid);
        spec.value = timespec_subtract(spec.value, now);
    }
    int err = timer_set(timer->timer, spec, &old_spec);
    unlock(&current->group->lock);
    if (err < 0)
        return err;

    if (old_value_addr) {
        if (!time64) {
            struct itimerspec_ old_value = timer_spec_from_real(old_spec);
            if (user_put(old_value_addr, old_value))
                return _EFAULT;
        } else {
            struct itimerspec64_ old_value = timer_spec_from_real64(old_spec);
            if (user_put(old_value_addr, old_value))
                return _EFAULT;
        }
    }
    return 0;
}

int_t sys_timer_settime(dword_t timer_id, int_t flags, addr_t new_value_addr, addr_t old_value_addr) {
    return sys_timer_settime_guest(timer_id, flags, new_value_addr, old_value_addr);
}

int_t sys_timer_settime_guest(dword_t timer_id, int_t flags, guest_addr_t new_value_addr, guest_addr_t old_value_addr) {
    return sys_timer_settime_common(timer_id, flags, new_value_addr, old_value_addr, false);
}

int_t sys_timer_settime64(dword_t timer_id, int_t flags, addr_t new_value_addr, addr_t old_value_addr) {
    return sys_timer_settime64_guest(timer_id, flags, new_value_addr, old_value_addr);
}

int_t sys_timer_settime64_guest(dword_t timer_id, int_t flags, guest_addr_t new_value_addr, guest_addr_t old_value_addr) {
    return sys_timer_settime_common(timer_id, flags, new_value_addr, old_value_addr, true);
}

int_t sys_timer_gettime64(dword_t timer_id, addr_t curr_value_addr) {
    return sys_timer_gettime64_guest(timer_id, curr_value_addr);
}

int_t sys_timer_gettime64_guest(dword_t timer_id, guest_addr_t curr_value_addr) {
    return sys_timer_gettime_common(timer_id, curr_value_addr, true);
}

int_t sys_timer_delete(dword_t timer_id) {
    STRACE("timer_delete(%d)\n", timer_id);
    lock(&current->group->lock, 0);
    struct posix_timer *timer = &current->group->posix_timers[timer_id];
    if (timer->timer == NULL) {
        unlock(&current->group->lock);
        return _EINVAL;
    }
    timer_free(timer->timer);
    timer->timer = NULL;
    unlock(&current->group->lock);
    return 0;
}

static struct fd_ops timerfd_ops;

static void timerfd_callback(struct fd *fd) {
    lock(&fd->lock, 0);
    fd->timerfd.expirations++;
    notify(&fd->cond);
    unlock(&fd->lock);
    poll_wakeup(fd, POLL_READ);
}

fd_t sys_timerfd_create(int_t clockid, int_t flags) {
    STRACE("timerfd_create(%d, %#x)", clockid, flags);
    if (time_warning_trace_enabled())
        printk("WARNING: timerfd_create pid=%d tgid=%d comm=%s clockid=%d flags=%#x\n",
               current->pid, current->tgid, current->comm, clockid, flags);
    // Linux timerfd only accepts the wall/uptime clocks; the CPU-time and
    // COARSE clocks (and unknown ids) are rejected with EINVAL even though
    // clockid_to_real would happily map some of them.
    if (clockid != CLOCK_REALTIME_ && clockid != CLOCK_MONOTONIC_ && clockid != CLOCK_BOOTTIME_)
        return _EINVAL;
    // Only TFD_NONBLOCK / TFD_CLOEXEC are valid (== O_NONBLOCK / O_CLOEXEC);
    // unknown flag bits -> EINVAL (was previously ignored).
    if (flags & ~(O_NONBLOCK_ | O_CLOEXEC_))
        return _EINVAL;
    clockid_t real_clockid;
    if (clockid_to_real(clockid, &real_clockid)) return _EINVAL;

    struct fd *fd = adhoc_fd_create(&timerfd_ops);
    if (fd == NULL)
        return _ENOMEM;

    fd->timerfd.timer = timer_new(real_clockid, (timer_callback_t) timerfd_callback, fd);
    return f_install(fd, flags);
}

static int timerfd_lookup(fd_t f, struct fd **fd_out) {
    struct fd *fd = f_get(f);
    if (fd == NULL)
        return _EBADF;
    if (fd->ops != &timerfd_ops)
        return _EINVAL;
    *fd_out = fd;
    return 0;
}

static struct timer_spec timerfd_current_spec(struct fd *fd) {
    struct timer_spec spec = {};
    lock(&fd->timerfd.timer->lock, 0);
    spec.interval = fd->timerfd.timer->interval;
    if (fd->timerfd.timer->active) {
        struct timespec remaining = timespec_subtract(fd->timerfd.timer->end,
            timespec_now(fd->timerfd.timer->clockid));
        if (timespec_positive(remaining))
            spec.value = remaining;
    }
    unlock(&fd->timerfd.timer->lock);
    return spec;
}

static int_t sys_timerfd_settime_common(fd_t f, int_t flags, guest_addr_t new_value_addr,
        guest_addr_t old_value_addr,
        bool time64) {
    STRACE("timerfd_settime(%d, %d, %#x, %#x)", f, flags, new_value_addr, old_value_addr);
    if (time_warning_trace_enabled())
        printk("WARNING: timerfd_settime pid=%d tgid=%d comm=%s fd=%d flags=%d new=%#x old=%#x time64=%d\n",
               current->pid, current->tgid, current->comm, f, flags, new_value_addr, old_value_addr, time64);
    if (flags & ~(TIMER_ABSTIME_))
        return _EINVAL;
    struct fd *fd;
    int err = timerfd_lookup(f, &fd);
    if (err < 0)
        return err;
    struct timer_spec spec;
    if (!time64) {
        struct itimerspec_ value;
        if (user_get(new_value_addr, value))
            return _EFAULT;
        spec = timer_spec_to_real(value);
    } else {
        struct itimerspec64_ value;
        if (user_get(new_value_addr, value))
            return _EFAULT;
        spec = timer_spec_to_real64(value);
    }
    struct timer_spec old_spec;
    if (flags & TIMER_ABSTIME_) {
        struct timespec now = timespec_now(fd->timerfd.timer->clockid);
        spec.value = timespec_subtract(spec.value, now);
    }

    lock(&fd->lock, 0);
    err = timer_set(fd->timerfd.timer, spec, &old_spec);
    unlock(&fd->lock);
    if (err < 0)
        return err;

    if (old_value_addr) {
        if (!time64) {
            struct itimerspec_ old_value = timer_spec_from_real(old_spec);
            if (user_put(old_value_addr, old_value))
                return _EFAULT;
        } else {
            struct itimerspec64_ old_value = timer_spec_from_real64(old_spec);
            if (user_put(old_value_addr, old_value))
                return _EFAULT;
        }
    }

    return 0;
}

int_t sys_timerfd_settime(fd_t f, int_t flags, addr_t new_value_addr, addr_t old_value_addr) {
    return sys_timerfd_settime_guest(f, flags, new_value_addr, old_value_addr);
}

int_t sys_timerfd_settime_guest(fd_t f, int_t flags, guest_addr_t new_value_addr, guest_addr_t old_value_addr) {
    return sys_timerfd_settime_common(f, flags, new_value_addr, old_value_addr, false);
}

int_t sys_timerfd_settime64(fd_t f, int_t flags, addr_t new_value_addr, addr_t old_value_addr) {
    return sys_timerfd_settime64_guest(f, flags, new_value_addr, old_value_addr);
}

int_t sys_timerfd_settime64_guest(fd_t f, int_t flags, guest_addr_t new_value_addr, guest_addr_t old_value_addr) {
    return sys_timerfd_settime_common(f, flags, new_value_addr, old_value_addr, true);
}

static int_t sys_timerfd_gettime_common(fd_t f, guest_addr_t curr_value_addr, bool time64) {
    STRACE("timerfd_gettime(%d, %#x)", f, curr_value_addr);
    struct fd *fd;
    int err = timerfd_lookup(f, &fd);
    if (err < 0)
        return err;
    struct timer_spec spec = timerfd_current_spec(fd);
    if (!time64) {
        struct itimerspec_ value = timer_spec_from_real(spec);
        if (user_put(curr_value_addr, value))
            return _EFAULT;
    } else {
        struct itimerspec64_ value = timer_spec_from_real64(spec);
        if (user_put(curr_value_addr, value))
            return _EFAULT;
    }
    return 0;
}

int_t sys_timerfd_gettime(fd_t f, addr_t curr_value_addr) {
    return sys_timerfd_gettime_guest(f, curr_value_addr);
}

int_t sys_timerfd_gettime_guest(fd_t f, guest_addr_t curr_value_addr) {
    return sys_timerfd_gettime_common(f, curr_value_addr, false);
}

int_t sys_timerfd_gettime64(fd_t f, addr_t curr_value_addr) {
    return sys_timerfd_gettime64_guest(f, curr_value_addr);
}

int_t sys_timerfd_gettime64_guest(fd_t f, guest_addr_t curr_value_addr) {
    return sys_timerfd_gettime_common(f, curr_value_addr, true);
}

static ssize_t timerfd_read(struct fd *fd, void *buf, size_t bufsize) {
    if (bufsize < sizeof(uint64_t))
        return _EINVAL;
    lock(&fd->lock, 0);
    while (fd->timerfd.expirations == 0) {
        if (fd->flags & O_NONBLOCK_) {
            unlock(&fd->lock);
            return _EAGAIN;
        }
        int err = wait_for(&fd->cond, &fd->lock, NULL);
        if (err < 0) {
            unlock(&fd->lock);
            return err;
        }
    }

    *(uint64_t *) buf = fd->timerfd.expirations;
    fd->timerfd.expirations = 0;
    unlock(&fd->lock);
    return sizeof(uint64_t);
}
static int timerfd_poll(struct fd *fd) {
    int res = 0;
    lock(&fd->lock, 0);
    if (fd->timerfd.expirations != 0)
        res |= POLL_READ;
    unlock(&fd->lock);
    return res;
}
static int timerfd_close(struct fd *fd) {
    timer_free(fd->timerfd.timer);
    return 0;
}

static struct fd_ops timerfd_ops = {
    .read = timerfd_read,
    .poll = timerfd_poll,
    .close = timerfd_close,
};
