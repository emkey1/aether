#include "debug.h"
#include <string.h>
#include <signal.h>
#include <sched.h>
#include "fs/poll.h"
#include "kernel/calls.h"
#include "kernel/signal.h"
#include "kernel/time.h"
#include "kernel/task.h"
#include "kernel/vdso.h"
#include "emu/interrupt.h"
#include "emu/memory.h"
#include "util/sync.h"

#if is_gcc(9)
#pragma GCC diagnostic ignored "-Waddress-of-packed-member"
#endif

int xsave_extra = 0;
int fxsave_extra = 0;
static void sigmask_set(sigset_t_ set);
static bool is_on_altstack(guest_addr_t sp, struct task *task);
static void restore_altstack(guest_addr_t sp, guest_addr_t stack, guest_addr_t size, dword_t flags);
static dword_t current_altstack_flags(struct task *task);
static void altstack_to_i386_user(struct task *task, struct stack_t_ *user_stack);
static void signalfd_wakeup_task(struct task *task, int sig);
static struct fd_ops signalfd_ops;
static void send_signal_with_sighand(struct task *task, struct sighand *sighand, int sig, struct siginfo_ info);

static bool should_trace_signal_task(struct task *UNUSED(task)) {
    return false;
}

struct sigaction_i386_marshaled {
    addr_t handler;
    dword_t flags;
    addr_t restorer;
    sigset_t_ mask;
} __attribute__((packed));

struct sigaction_amd64_marshaled {
    qword_t handler;
    qword_t flags;
    qword_t restorer;
    sigset_t_ mask;
} __attribute__((packed));

struct amd64_siginfo_ {
    int_t sig;
    int_t sig_errno;
    int_t code;
    int_t __pad0;
    union {
        struct {
            pid_t_ pid;
            uid_t_ uid;
        } kill;
        struct {
            pid_t_ pid;
            uid_t_ uid;
            union sigval_ value;
        } rt;
        struct {
            pid_t_ pid;
            uid_t_ uid;
            int_t status;
            int_t __pad0;
            qword_t utime;
            qword_t stime;
        } child;
        struct {
            guest_addr_t addr;
            word_t addr_lsb;
            byte_t __pad0[6];
            guest_addr_t lower;
            guest_addr_t upper;
        } fault;
        struct {
            int_t timer;
            int_t overrun;
            union sigval_ value;
            int_t _private;
            int_t __pad0;
        } timer;
        struct {
            guest_addr_t call_addr;
            int_t syscall;
            dword_t arch;
        } sigsys;
        byte_t __pad[112];
    };
} __attribute__((packed));

static_assert(sizeof(struct amd64_siginfo_) == 128, "amd64 siginfo layout mismatch");

enum amd64_greg_index {
    AMD64_GREG_R8 = 0,
    AMD64_GREG_R9,
    AMD64_GREG_R10,
    AMD64_GREG_R11,
    AMD64_GREG_R12,
    AMD64_GREG_R13,
    AMD64_GREG_R14,
    AMD64_GREG_R15,
    AMD64_GREG_RDI,
    AMD64_GREG_RSI,
    AMD64_GREG_RBP,
    AMD64_GREG_RBX,
    AMD64_GREG_RDX,
    AMD64_GREG_RAX,
    AMD64_GREG_RCX,
    AMD64_GREG_RSP,
    AMD64_GREG_RIP,
    AMD64_GREG_EFL,
    AMD64_GREG_CSGSFS,
    AMD64_GREG_ERR,
    AMD64_GREG_TRAPNO,
    AMD64_GREG_OLDMASK,
    AMD64_GREG_CR2,
    AMD64_GREG_COUNT,
};

enum {
    AMD64_USER_CS = 0x33,
    AMD64_UC_FP_XSTATE = 0x1,
    AMD64_UC_SIGCONTEXT_SS = 0x2,
    AMD64_UC_STRICT_RESTORE_SS = 0x4,
};

struct amd64_stack_t_marshaled {
    qword_t stack;
    dword_t flags;
    dword_t pad;
    qword_t size;
};

struct amd64_fpxreg_ {
    word_t significand[4];
    word_t exponent;
    word_t padding[3];
};

struct amd64_xmmreg_ {
    uint32_t element[4];
};

struct amd64_fpstate_ {
    word_t cwd;
    word_t swd;
    word_t twd;
    word_t fop;
    qword_t rip;
    qword_t rdp;
    dword_t mxcsr;
    dword_t mxcr_mask;
    struct amd64_fpxreg_ st[8];
    struct amd64_xmmreg_ xmm[16];
    dword_t reserved1[24];
};

static_assert(sizeof(struct amd64_fpstate_) == 512, "amd64 fpstate layout mismatch");

struct amd64_mcontext_ {
    qword_t gregs[AMD64_GREG_COUNT];
    guest_addr_t fpstate;
    qword_t reserved1[8];
};

struct amd64_ucontext_ {
    qword_t flags;
    guest_addr_t link;
    struct amd64_stack_t_marshaled stack;
    struct amd64_mcontext_ mcontext;
    sigset_t_ sigmask;
    struct amd64_fpstate_ fpregs_mem;
    qword_t ssp[4];
};

struct rt_sigframe_amd64 {
    guest_addr_t pretcode;
    struct amd64_ucontext_ uc;
    struct amd64_siginfo_ info;
    char retcode[8];
};

static int sigaction_from_user(struct task *task, guest_addr_t user_addr, struct sigaction_ *action) {
    if (task->abi == GUEST_ABI_AMD64) {
        struct sigaction_amd64_marshaled user_action;
        if (user_get(user_addr, user_action))
            return _EFAULT;
        *action = (struct sigaction_) {
            .handler = user_action.handler,
            .flags = user_action.flags,
            .restorer = user_action.restorer,
            .mask = user_action.mask,
        };
    } else {
        struct sigaction_i386_marshaled user_action;
        if (user_get(user_addr, user_action))
            return _EFAULT;
        *action = (struct sigaction_) {
            .handler = user_action.handler,
            .flags = user_action.flags,
            .restorer = user_action.restorer,
            .mask = user_action.mask,
        };
    }
    return 0;
}

static int sigaction_to_user(struct task *task, guest_addr_t user_addr, const struct sigaction_ *action) {
    if (task->abi == GUEST_ABI_AMD64) {
        struct sigaction_amd64_marshaled user_action = {
            .handler = action->handler,
            .flags = action->flags,
            .restorer = action->restorer,
            .mask = action->mask,
        };
        if (user_put(user_addr, user_action))
            return _EFAULT;
    } else {
        struct sigaction_i386_marshaled user_action = {
            .handler = (addr_t) action->handler,
            .flags = (dword_t) action->flags,
            .restorer = (addr_t) action->restorer,
            .mask = action->mask,
        };
        if (user_put(user_addr, user_action))
            return _EFAULT;
    }
    return 0;
}

static bool wake_waiting_task(struct task *task) {
    pthread_mutex_lock(&task->waiting_cond_lock.m);
    task->waiting_cond_lock.owner = pthread_self();

    cond_t *waiting_cond = task->waiting_cond;
    lock_t *waiting_lock = task->waiting_lock;
    bool *waiting_interrupt_flag = task->waiting_interrupt_flag;
    bool interrupted_wait = waiting_interrupt_flag != NULL ||
        (waiting_cond != NULL && waiting_lock != NULL);
    if (waiting_interrupt_flag != NULL)
        __atomic_store_n(waiting_interrupt_flag, true, __ATOMIC_RELEASE);
    if (waiting_cond != NULL && waiting_lock != NULL) {
        bool have_wait_lock = false;
        bool acquired_wait_lock = false;
        bool using_existing_wait_lock = false;
        int wait_lock_status = pthread_mutex_trylock(&waiting_lock->m);
        if (wait_lock_status == 0) {
            have_wait_lock = true;
            acquired_wait_lock = true;
        } else if (wait_lock_status == EBUSY &&
                   pthread_equal(waiting_lock->owner, pthread_self())) {
            // The signal sender may already hold the mutex associated with the
            // waiter (for example pids_lock during kill/wait interactions).
            // In that case it is safe to notify directly while keeping the
            // existing lock ownership.
            have_wait_lock = true;
            using_existing_wait_lock = true;
        } else if (wait_lock_status == EBUSY) {
            // Do not block on waiting_lock while holding waiting_cond_lock.
            // The waiter clears waiting_cond/waiting_lock after
            // pthread_cond_wait() returns, while still holding waiting_lock and
            // then taking waiting_cond_lock. Holding waiting_cond_lock here and
            // then blocking on waiting_lock deadlocks with that path.
            memset(&task->waiting_cond_lock.owner, 0, sizeof(task->waiting_cond_lock.owner));
            pthread_mutex_unlock(&task->waiting_cond_lock.m);

            // Wait until the waiter either reaches pthread_cond_wait() and
            // releases waiting_lock, or finishes the wait path entirely.
            pthread_mutex_lock(&waiting_lock->m);
            have_wait_lock = true;
            acquired_wait_lock = true;

            pthread_mutex_lock(&task->waiting_cond_lock.m);
            task->waiting_cond_lock.owner = pthread_self();
            if (task->waiting_cond != waiting_cond || task->waiting_lock != waiting_lock)
                have_wait_lock = false;
        }

        if (have_wait_lock) {
            notify(waiting_cond);
            if (!using_existing_wait_lock)
                pthread_mutex_unlock(&waiting_lock->m);
        } else if (acquired_wait_lock && !using_existing_wait_lock) {
            pthread_mutex_unlock(&waiting_lock->m);
        }
    }

    memset(&task->waiting_cond_lock.owner, 0, sizeof(task->waiting_cond_lock.owner));
    pthread_mutex_unlock(&task->waiting_cond_lock.m);
    return interrupted_wait;
}

static guest_addr_t current_user_sp(struct task *task) {
    if (task->abi == GUEST_ABI_AMD64)
        return task->cpu.amd64_regs[amd64_rsp];
    return task->cpu.esp;
}

struct signalfd_state {
    sigset_t_ mask;
};

struct signalfd_siginfo_ {
    dword_t signo;
    sdword_t sig_errno;
    sdword_t code;
    dword_t pid;
    dword_t uid;
    sdword_t fd;
    dword_t tid;
    dword_t band;
    dword_t overrun;
    dword_t trapno;
    sdword_t status;
    sdword_t sig_int;
    qword_t sig_ptr;
    qword_t utime;
    qword_t stime;
    qword_t addr;
    word_t addr_lsb;
    word_t __pad2;
    sdword_t syscall;
    qword_t call_addr;
    dword_t arch;
    byte_t __pad[28];
} __attribute__((packed));

static_assert(sizeof(struct signalfd_siginfo_) == 128, "signalfd siginfo layout mismatch");

static int signal_is_blockable(int sig) {
    return sig != SIGKILL_ && sig != SIGSTOP_;
}

static bool signal_is_realtime(int sig) {
    return sig > SIGSYS_ && sig < NUM_SIGS;
}

#define UNBLOCKABLE_MASK (sig_mask(SIGKILL_) | sig_mask(SIGSTOP_))

static bool signal_is_synchronous_trap(int sig) {
    switch (sig) {
        case SIGILL_:
        case SIGTRAP_:
        case SIGBUS_:
        case SIGFPE_:
        case SIGSEGV_:
        case SIGSYS_:
            return true;
        default:
            return false;
    }
}

#define SIGNAL_IGNORE 0
#define SIGNAL_KILL 1
#define SIGNAL_CALL_HANDLER 2
#define SIGNAL_STOP 3

static int signal_action(struct sighand *sighand, int sig) {
    if (signal_is_blockable(sig)) {
        struct sigaction_ *action = &sighand->action[sig];
        if(sig > 63)
            return SIGNAL_IGNORE;
        
        if (action->handler == SIG_IGN_)
            return SIGNAL_IGNORE;
        if (action->handler != SIG_DFL_)
            return SIGNAL_CALL_HANDLER;
    }

    switch (sig) {
        // Linux defaults SIGURG to ignore; it arrives with out-of-band TCP
        // data, so defaulting it to kill terminates innocent network users.
        case SIGCONT_: case SIGCHLD_: case SIGURG_:
        case SIGIO_: case SIGWINCH_:
            return SIGNAL_IGNORE;

        case SIGSTOP_: case SIGTSTP_: case SIGTTIN_: case SIGTTOU_:
            return SIGNAL_STOP;

        default:
            return SIGNAL_KILL;
    }
}

static void deliver_signal_unlocked_locked(struct task *task, struct sighand *sighand, int sig, struct siginfo_ info) {
    if (!signal_is_realtime(sig) && sigset_has(task->pending, sig))
        return;

    if (task->exiting)
        return;

    sigset_add(&task->pending, sig);
    struct sigqueue *sigqueue = malloc(sizeof(struct sigqueue));
    sigqueue->info = info;
    sigqueue->info.sig = sig;
    list_add_tail(&task->queue, &sigqueue->queue);
    signalfd_wakeup_task(task, sig);

    // Synchronous fault signals must be delivered even when the task has them
    // masked. libc abort paths rely on this by blocking signals before
    // executing a crash instruction like `hlt`.
    if (sigset_has(task->blocked & ~task->waiting, sig) && signal_is_blockable(sig) &&
            !signal_is_synchronous_trap(sig))
        return;

    bool interrupted_wait = false;
    if (task != current) {
        int wake_err = pthread_kill(task->thread, SIGUSR1);
        if ((sig == SIGKILL_ || sig == SIGABRT_) &&
                (amd64_trace_is_lineage_tgid(task->tgid) ||
                 (current != NULL && amd64_trace_is_lineage_tgid(current->tgid)))) {
            printk("tracked signal wake: sender=%d sender_tgid=%d sig=%d target=%d target_tgid=%d abi=%d wake_err=%d exiting=%d io_block=%d pending=%#llx blocked=%#llx\n",
                   current != NULL ? current->pid : -1,
                   current != NULL ? current->tgid : -1,
                   sig, task->pid, task->tgid, task->abi, wake_err,
                   task->exiting, task->io_block,
                   (unsigned long long) task->pending,
                   (unsigned long long) task->blocked);
        }
        if (task->cpu.poked_ptr)
            cpu_poke(&task->cpu);

        // Wake pthread condition waiters without keeping sighand->lock held.
        // If the waiter is between publishing waiting_cond and entering
        // pthread_cond_wait(), retry briefly for its lock handoff so the wake
        // is not lost. This avoids global timed polling in wait_for().
        memset(&sighand->lock.owner, 0, sizeof(sighand->lock.owner));
        pthread_mutex_unlock(&sighand->lock.m);
        interrupted_wait = wake_waiting_task(task);
        pthread_mutex_lock(&sighand->lock.m);
        sighand->lock.owner = pthread_self();
    } else {
        interrupted_wait = wake_waiting_task(task);
    }
    if (interrupted_wait) {
        bool restart = signal_action(sighand, sig) == SIGNAL_CALL_HANDLER &&
            !!(sighand->action[sig].flags & SA_RESTART_);
        __atomic_store_n(&task->restart_interrupted_syscall, restart, __ATOMIC_RELEASE);
        __atomic_store_n(&task->wait_interrupted, true, __ATOMIC_RELEASE);
    }
}

void deliver_signal_with_sighand(struct task *task, struct sighand *sighand, int sig, struct siginfo_ info) {
    lock(&sighand->lock, 0);
    // deliver_signal is the forced path (faults, not kill()). Match Linux
    // force_sig semantics for synchronous traps: if the signal is ignored,
    // or blocked with the default handler, reset to SIG_DFL and unblock it
    // so the task dies. Without this the faulting instruction re-executes
    // forever: handle_interrupt only calls receive_signals for unblocked
    // pending signals, and receive_signals skips blocked ones anyway.
    // User-sent signals go through send_signal and are not affected.
    if (signal_is_synchronous_trap(sig) && signal_is_blockable(sig)) {
        struct sigaction_ *action = &sighand->action[sig];
        if (action->handler == SIG_IGN_ ||
                (action->handler == SIG_DFL_ && sigset_has(task->blocked, sig))) {
            *action = (struct sigaction_) {.handler = SIG_DFL_};
            sigset_del(&task->blocked, sig);
        }
    }
    deliver_signal_unlocked_locked(task, sighand, sig, info);
    unlock(&sighand->lock);
}

void deliver_signal(struct task *task, int sig, struct siginfo_ info) {
    struct sighand *sighand = task->sighand;
    if (sighand == NULL)
        return;
    deliver_signal_with_sighand(task, sighand, sig, info);
}

static bool signal_still_pending_locked(struct task *task, int sig) {
    struct sigqueue *sigqueue;
    list_for_each_entry(&task->queue, sigqueue, queue) {
        if (sigqueue->info.sig == sig)
            return true;
    }
    return false;
}

static bool signal_take_next_locked(struct task *task, sigset_t_ mask, struct siginfo_ *info_out) {
    // POSIX/signal(7): when several signals are pending, the lowest-numbered is
    // delivered first; multiple instances of the same (real-time) signal are
    // delivered FIFO. The queue is in FIFO insertion order, so scan for the
    // lowest signal number and, using a strict <, keep the first (oldest)
    // entry of that number. Matters for sigtimedwait/sigwaitinfo/signalfd.
    struct sigqueue *sigqueue;
    struct sigqueue *best = NULL;
    list_for_each_entry(&task->queue, sigqueue, queue) {
        if (!sigset_has(mask, sigqueue->info.sig))
            continue;
        if (best == NULL || sigqueue->info.sig < best->info.sig)
            best = sigqueue;
    }
    if (best == NULL)
        return false;
    *info_out = best->info;
    int sig = best->info.sig;
    list_remove(&best->queue);
    if (!signal_still_pending_locked(task, sig))
        sigset_del(&task->pending, sig);
    free(best);
    return true;
}

static void siginfo_to_i386_user(struct i386_siginfo_ *out, const struct siginfo_ *info) {
    memset(out, 0, sizeof(*out));
    out->sig = info->sig;
    out->sig_errno = info->sig_errno;
    out->code = info->code;
    switch (info->sig) {
        case SIGCHLD_:
            out->child.pid = info->child.pid;
            out->child.uid = info->child.uid;
            out->child.status = info->child.status;
            out->child.utime = info->child.utime;
            out->child.stime = info->child.stime;
            break;
        case SIGILL_:
        case SIGBUS_:
        case SIGFPE_:
        case SIGSEGV_:
            out->fault.addr = (addr_t) info->fault.addr;
            break;
        case SIGTRAP_:
            if (info->code == SIGTRAP_ || info->code == (SIGTRAP_ | 0x80)) {
                // Ptrace syscall-stop: Linux reports only si_signo/si_code.
            } else if ((info->code >> 8) != 0) {
                // Ptrace event-stop: Linux populates si_pid/si_uid, not si_addr.
                out->kill.pid = info->kill.pid;
                out->kill.uid = info->kill.uid;
            } else if (info->code <= 0 || info->code == SI_KERNEL_) {
                out->kill.pid = info->kill.pid;
                out->kill.uid = info->kill.uid;
            } else {
                out->fault.addr = (addr_t) info->fault.addr;
            }
            break;
        case SIGSYS_:
            out->sigsys.addr = (addr_t) info->sigsys.addr;
            out->sigsys.syscall = info->sigsys.syscall;
            break;
        default:
            if (info->code == SI_TIMER_) {
                out->timer.timer = info->timer.timer;
                out->timer.overrun = info->timer.overrun;
                memcpy(&out->timer.value, &info->timer.value, sizeof(out->timer.value));
                out->timer._private = info->timer._private;
            } else {
                if (info->code == SI_QUEUE_) {
                    out->rt.pid = info->rt.pid;
                    out->rt.uid = info->rt.uid;
                    memcpy(&out->rt.value, &info->rt.value, sizeof(out->rt.value));
                } else {
                    out->kill.pid = info->kill.pid;
                    out->kill.uid = info->kill.uid;
                }
            }
            break;
    }
}

static void siginfo_to_amd64_user(struct amd64_siginfo_ *out, const struct siginfo_ *info) {
    memset(out, 0, sizeof(*out));
    out->sig = info->sig;
    out->sig_errno = info->sig_errno;
    out->code = info->code;
    switch (info->sig) {
        case SIGCHLD_:
            out->child.pid = info->child.pid;
            out->child.uid = info->child.uid;
            out->child.status = info->child.status;
            out->child.utime = info->child.utime;
            out->child.stime = info->child.stime;
            break;
        case SIGILL_:
        case SIGBUS_:
        case SIGFPE_:
        case SIGSEGV_:
            out->fault.addr = info->fault.addr;
            break;
        case SIGTRAP_:
            if (info->code == SIGTRAP_ || info->code == (SIGTRAP_ | 0x80)) {
                // Ptrace syscall-stop: Linux reports only si_signo/si_code.
            } else if ((info->code >> 8) != 0) {
                out->kill.pid = info->kill.pid;
                out->kill.uid = info->kill.uid;
            } else if (info->code <= 0 || info->code == SI_KERNEL_) {
                out->kill.pid = info->kill.pid;
                out->kill.uid = info->kill.uid;
            } else {
                out->fault.addr = info->fault.addr;
            }
            break;
        case SIGSYS_:
            out->sigsys.call_addr = info->sigsys.addr;
            out->sigsys.syscall = info->sigsys.syscall;
            break;
        default:
            if (info->code == SI_TIMER_) {
                out->timer.timer = info->timer.timer;
                out->timer.overrun = info->timer.overrun;
                out->timer.value = info->timer.value;
                out->timer._private = info->timer._private;
            } else {
                if (info->code == SI_QUEUE_) {
                    out->rt.pid = info->rt.pid;
                    out->rt.uid = info->rt.uid;
                    out->rt.value = info->rt.value;
                } else {
                    out->kill.pid = info->kill.pid;
                    out->kill.uid = info->kill.uid;
                }
            }
            break;
    }
}

static int siginfo_from_user(struct task *task, guest_addr_t user_addr, struct siginfo_ *info) {
    memset(info, 0, sizeof(*info));
    if (task->abi == GUEST_ABI_AMD64) {
        struct amd64_siginfo_ user_info;
        if (user_get(user_addr, user_info))
            return _EFAULT;
        info->sig = user_info.sig;
        info->sig_errno = user_info.sig_errno;
        info->code = user_info.code;
        if (info->code == SI_TIMER_) {
            info->timer.timer = user_info.timer.timer;
            info->timer.overrun = user_info.timer.overrun;
            info->timer.value = user_info.timer.value;
            info->timer._private = user_info.timer._private;
        } else if (info->code == SI_QUEUE_) {
            info->rt.pid = user_info.rt.pid;
            info->rt.uid = user_info.rt.uid;
            info->rt.value = user_info.rt.value;
        } else {
            info->kill.pid = user_info.kill.pid;
            info->kill.uid = user_info.kill.uid;
        }
    } else {
        struct i386_siginfo_ user_info;
        if (user_get(user_addr, user_info))
            return _EFAULT;
        info->sig = user_info.sig;
        info->sig_errno = user_info.sig_errno;
        info->code = user_info.code;
        if (info->code == SI_TIMER_) {
            info->timer.timer = user_info.timer.timer;
            info->timer.overrun = user_info.timer.overrun;
            memcpy(&info->timer.value, &user_info.timer.value, sizeof(info->timer.value));
            info->timer._private = user_info.timer._private;
        } else if (info->code == SI_QUEUE_) {
            info->rt.pid = user_info.rt.pid;
            info->rt.uid = user_info.rt.uid;
            memcpy(&info->rt.value, &user_info.rt.value, sizeof(info->rt.value));
        } else {
            info->kill.pid = user_info.kill.pid;
            info->kill.uid = user_info.kill.uid;
        }
    }
    return 0;
}

int siginfo_to_user(struct task *task, guest_addr_t user_addr, const struct siginfo_ *info) {
    if (task->abi == GUEST_ABI_AMD64) {
        struct amd64_siginfo_ user_info;
        siginfo_to_amd64_user(&user_info, info);
        if (user_put(user_addr, user_info))
            return _EFAULT;
    } else {
        struct i386_siginfo_ user_info;
        siginfo_to_i386_user(&user_info, info);
        if (user_put(user_addr, user_info))
            return _EFAULT;
    }
    return 0;
}

static void signalfd_info_from_siginfo(struct signalfd_siginfo_ *out, struct siginfo_ *info) {
    memset(out, 0, sizeof(*out));
    out->signo = info->sig;
    out->sig_errno = info->sig_errno;
    out->code = info->code;
    out->pid = info->code == SI_QUEUE_ ? info->rt.pid : info->kill.pid;
    out->uid = info->code == SI_QUEUE_ ? info->rt.uid : info->kill.uid;
    out->status = info->child.status;
    out->sig_int = info->code == SI_QUEUE_ ? info->rt.value.sv_int : info->timer.value.sv_int;
    out->sig_ptr = info->code == SI_QUEUE_ ? info->rt.value.sv_ptr : info->timer.value.sv_ptr;
    out->utime = info->child.utime;
    out->stime = info->child.stime;
    out->addr = info->fault.addr;
    out->overrun = info->timer.overrun;
    out->tid = info->timer.timer;
    out->fd = -1;
    out->syscall = info->sigsys.syscall;
    out->call_addr = info->sigsys.addr;
}

static struct fdtable *signalfd_task_files_retain(struct task *task) {
    struct fdtable *files = NULL;
    lock(&task->general_lock, 0);
    if (!task->exiting && task->files != NULL)
        files = fdtable_retain(task->files);
    unlock(&task->general_lock);
    return files;
}

static void signalfd_wakeup_task(struct task *task, int sig) {
    if (task == NULL)
        return;

    struct fdtable *files = signalfd_task_files_retain(task);
    if (files == NULL)
        return;

    // Use trylock to avoid a deadlock: this function is called while
    // sighand->lock (and often pids_lock) is held.  f_close holds
    // files->lock during fdtable_close and may transitively need sighand or
    // pids.  If the files table is currently locked, skip the wakeup — the
    // signal is already pending in task->pending, so the task will find it
    // when it next checks for signals.
    if (trylock(&files->lock) != 0) {
        fdtable_release(files);
        return;
    }
    for (fd_t fd_no = 0; (unsigned) fd_no < files->size; fd_no++) {
        struct fd *fd = fdtable_get(files, fd_no);
        if (fd == NULL || fd->ops != &signalfd_ops || fd->data == NULL)
            continue;
        struct signalfd_state *state = fd->data;
        if (!sigset_has(state->mask, sig))
            continue;
        notify(&fd->cond);
        poll_wakeup(fd, POLL_READ);
    }
    unlock(&files->lock);
    fdtable_release(files);
}

static int signalfd_poll(struct fd *fd) {
    struct signalfd_state *state = fd->data;
    if (state == NULL)
        return POLL_ERR;

    lock(&current->sighand->lock, 0);
    struct sigqueue *sigqueue;
    list_for_each_entry(&current->queue, sigqueue, queue) {
        if (sigset_has(state->mask, sigqueue->info.sig)) {
            unlock(&current->sighand->lock);
            return POLL_READ;
        }
    }
    unlock(&current->sighand->lock);
    return 0;
}

static ssize_t signalfd_read(struct fd *fd, void *buf, size_t bufsize) {
    struct signalfd_state *state = fd->data;
    if (state == NULL)
        return _EINVAL;
    if (bufsize < sizeof(struct signalfd_siginfo_))
        return _EINVAL;

    size_t max_infos = bufsize / sizeof(struct signalfd_siginfo_);
    size_t count = 0;
    lock(&fd->lock, 0);
    while (count == 0) {
        lock(&current->sighand->lock, 0);
        while (count < max_infos) {
            struct siginfo_ info;
            if (!signal_take_next_locked(current, state->mask, &info))
                break;
            signalfd_info_from_siginfo(&((struct signalfd_siginfo_ *) buf)[count], &info);
            count++;
        }
        unlock(&current->sighand->lock);
        if (count != 0)
            break;
        if (fd->flags & O_NONBLOCK_) {
            unlock(&fd->lock);
            return _EAGAIN;
        }
        int err = wait_for(&fd->cond, &fd->lock, NULL);
        if (err != 0) {
            unlock(&fd->lock);
            return err;
        }
    }
    unlock(&fd->lock);
    return count * sizeof(struct signalfd_siginfo_);
}

static int signalfd_close(struct fd *fd) {
    free(fd->data);
    fd->data = NULL;
    return 0;
}

static struct fd_ops signalfd_ops = {
    .read = signalfd_read,
    .poll = signalfd_poll,
    .close = signalfd_close,
};

int_t sys_signalfd4(int_t fd_no, addr_t mask_addr, dword_t sigsetsize, int_t flags) {
    return sys_signalfd4_guest(fd_no, mask_addr, sigsetsize, flags);
}

int_t sys_signalfd4_guest(int_t fd_no, guest_addr_t mask_addr, dword_t sigsetsize, int_t flags) {
    if (sigsetsize != sizeof(sigset_t_))
        return _EINVAL;
    if (flags & ~(O_CLOEXEC_ | O_NONBLOCK_))
        return _EINVAL;

    sigset_t_ mask;
    if (user_get(mask_addr, mask))
        return _EFAULT;
    mask &= ~UNBLOCKABLE_MASK;

    if (fd_no != -1) {
        struct fd *fd = f_get(fd_no);
        if (fd == NULL || fd->ops != &signalfd_ops || fd->data == NULL)
            return _EINVAL;
        ((struct signalfd_state *) fd->data)->mask = mask;
        return fd_no;
    }

    struct fd *fd = adhoc_fd_create(&signalfd_ops);
    if (fd == NULL)
        return _ENOMEM;
    struct signalfd_state *state = malloc(sizeof(*state));
    if (state == NULL) {
        fd_close(fd);
        return _ENOMEM;
    }
    *state = (struct signalfd_state) {.mask = mask};
    fd->data = state;
    return f_install(fd, flags);
}

int_t sys_signalfd(int_t fd, addr_t mask_addr, dword_t sigsetsize) {
    return sys_signalfd4_guest(fd, mask_addr, sigsetsize, 0);
}

int_t sys_signalfd_guest(int_t fd, guest_addr_t mask_addr, dword_t sigsetsize) {
    return sys_signalfd4_guest(fd, mask_addr, sigsetsize, 0);
}

void send_signal(struct task *task, int sig, struct siginfo_ info) {
    struct sighand *sighand = task->sighand;
    if (sighand == NULL)
        return;
    send_signal_with_sighand(task, sighand, sig, info);
}

static void send_signal_with_sighand(struct task *task, struct sighand *sighand, int sig, struct siginfo_ info) {
    // signal zero is for testing whether a process exists
    if (sig == 0)
        return;
    if (task->zombie || task->exiting)
        return;
    lock(&sighand->lock, 0);
    bool ignored = signal_action(sighand, sig) == SIGNAL_IGNORE;
    bool synchronously_consumed = sigset_has(task->blocked | task->waiting, sig);
    if (should_trace_signal_task(task)) {
        printk("tracked signal send: target=%d tgid=%d comm=%s sig=%d ignored=%d sync=%d blocked=%#x waiting=%#x pending=%#x sender=%d/%s\n",
               task->pid, task->tgid, task->comm, sig, ignored, synchronously_consumed,
               task->blocked, task->waiting, task->pending,
               current != NULL ? current->pid : 0, current != NULL ? current->comm : "?");
    }
    if ((!ignored || synchronously_consumed) && (task->pid <= MAX_PID)) {
        deliver_signal_unlocked_locked(task, sighand, sig, info);
    }
    unlock(&sighand->lock);

    if (sig == SIGCONT_ || sig == SIGKILL_) {
        lock(&task->group->lock, 0);
        task->group->stopped = false;
        notify(&task->group->stopped_cond);
        unlock(&task->group->lock);
    }
}

bool signal_should_restart_syscall(void) {
    if (current == NULL)
        return false;

    if (__atomic_exchange_n(&current->restart_interrupted_syscall, false, __ATOMIC_ACQ_REL))
        return true;

    struct sighand *sighand = current->sighand;
    lock(&sighand->lock, 0);
    struct sigqueue *sigqueue;
    list_for_each_entry(&current->queue, sigqueue, queue) {
        int sig = sigqueue->info.sig;
        if (sigset_has(current->blocked, sig))
            continue;
        if (signal_action(sighand, sig) != SIGNAL_CALL_HANDLER) {
            unlock(&sighand->lock);
            return false;
        }
        bool restart = !!(sighand->action[sig].flags & SA_RESTART_);
        unlock(&sighand->lock);
        return restart;
    }
    unlock(&sighand->lock);
    return false;
}

bool try_self_signal(int sig) {
    assert(sig == SIGTTIN_ || sig == SIGTTOU_);

    struct sighand *sighand = current->sighand;
    lock(&sighand->lock, 0);
    bool can_send = signal_action(sighand, sig) != SIGNAL_IGNORE &&
        !sigset_has(current->blocked, sig);
    if (can_send)
        deliver_signal_unlocked_locked(current, current->sighand, sig, SIGINFO_NIL);
    unlock(&sighand->lock);
    return can_send;
}

int send_group_signal(dword_t pgid, int sig, struct siginfo_ info) {
    struct group_signal_target {
        struct task *task;
        struct sighand *sighand;
    };
    struct group_signal_target stack_targets[32];
    struct group_signal_target *targets = stack_targets;
    size_t target_cap = sizeof(stack_targets) / sizeof(stack_targets[0]);
    size_t target_count = 0;

    complex_lockt(&pids_lock, 0);
    struct pid *pid = pid_get(pgid);
    if (pid == NULL) {
        unlock(&pids_lock);
        return _ESRCH;
    }

    size_t needed = 0;
    struct tgroup *tgroup;
    list_for_each_entry(&pid->pgroup, tgroup, pgroup)
        needed++;
    if (needed > target_cap) {
        unlock(&pids_lock);
        targets = malloc(sizeof(*targets) * needed);
        if (targets == NULL)
            return _ENOMEM;
        target_cap = needed;

        complex_lockt(&pids_lock, 0);
        pid = pid_get(pgid);
        if (pid == NULL) {
            unlock(&pids_lock);
            free(targets);
            return _ESRCH;
        }
    }

    list_for_each_entry(&pid->pgroup, tgroup, pgroup) {
        struct task *task = tgroup->leader;
        if (task == NULL || task->zombie || task->exiting || task->sighand == NULL)
            continue;
        task_ref_cnt_mod(task, 1);
        sighand_retain(task->sighand);
        targets[target_count++] = (struct group_signal_target) {
            .task = task,
            .sighand = task->sighand,
        };
    }
    unlock(&pids_lock);

    for (size_t i = 0; i < target_count; i++) {
        send_signal_with_sighand(targets[i].task, targets[i].sighand, sig, info);
        sighand_release(targets[i].sighand);
        task_ref_cnt_mod(targets[i].task, -1);
    }
    if (targets != stack_targets)
        free(targets);
    return 0;
}

static guest_addr_t sigreturn_trampoline(const char *name) {
    addr_t sigreturn_addr = vdso_symbol(name);
    if (sigreturn_addr == 0) {
        die("sigreturn not found in vdso, this should never happen");
    }
    return current->mm->vdso + sigreturn_addr;
}

static guest_addr_t signal_restorer(const struct sigaction_ *action, bool rt) {
    if (current->abi == GUEST_ABI_AMD64)
        return action->restorer;
    return sigreturn_trampoline(rt ? "__kernel_rt_sigreturn" : "__kernel_sigreturn");
}

static bool signal_should_capture_trap_state(int sig) {
    return signal_is_synchronous_trap(sig);
}

static qword_t signal_trap_error(struct cpu_state *cpu) {
    switch (cpu->trapno) {
        case INT_PF: {
            qword_t err = 0x4; // user-mode fault
            if (cpu->segfault_was_write)
                err |= 0x2;
            mem_read_lock_quiesce_aware(current->mem);
            if (mem_segv_reason(current->mem, cpu->segfault_addr) == SEGV_ACCERR_)
                err |= 0x1;
            mem_read_unlock_quiesce_aware(current->mem);
            return err;
        }
        default:
            return 0;
    }
}

static void setup_sigcontext(struct sigcontext_ *sc, struct cpu_state *cpu, int sig) {
    sc->ax = cpu->eax;
    sc->bx = cpu->ebx;
    sc->cx = cpu->ecx;
    sc->dx = cpu->edx;
    sc->di = cpu->edi;
    sc->si = cpu->esi;
    sc->bp = cpu->ebp;
    sc->sp = sc->sp_at_signal = cpu->esp;
    sc->ip = cpu->eip;
    collapse_flags(cpu);
    sc->flags = cpu->eflags;
    sc->trapno = signal_should_capture_trap_state(sig) ? cpu->trapno : 0;
    sc->err = signal_should_capture_trap_state(sig) ? (dword_t) signal_trap_error(cpu) : 0;
    if (sc->trapno == INT_PF)
        sc->cr2 = cpu->segfault_addr;
    else
        sc->cr2 = 0;
    // TODO more shit
    sc->oldmask = current->blocked & 0xffffffff;
}

static void setup_sigframe(struct siginfo_ *info, struct sigframe_ *frame) {
    frame->restorer = (addr_t) signal_restorer(&current->sighand->action[info->sig], false);
    frame->sig = info->sig;
    setup_sigcontext(&frame->sc, &current->cpu, info->sig);
    frame->extramask = current->blocked >> 32;

    static const struct {
        uint16_t popmov;
        uint32_t nr_sigreturn;
        uint16_t int80;
    } __attribute__((packed)) retcode = {
        .popmov = 0xb858,
        .nr_sigreturn = 113,
        .int80 = 0x80cd,
    };
    memcpy(frame->retcode, &retcode, sizeof(retcode));
}

static void setup_rt_sigframe(struct siginfo_ *info, struct rt_sigframe_ *frame) {
    frame->restorer = (addr_t) signal_restorer(&current->sighand->action[info->sig], true);
    frame->sig = info->sig;
    siginfo_to_i386_user(&frame->info, info);
    frame->uc.flags = 0;
    frame->uc.link = 0;
    altstack_to_i386_user(current, &frame->uc.stack);
    setup_sigcontext(&frame->uc.mcontext, &current->cpu, info->sig);
    frame->uc.sigmask = current->blocked;

    static const struct {
        uint8_t mov;
        uint32_t nr_rt_sigreturn;
        uint16_t int80;
        uint8_t pad;
    } __attribute__((packed)) rt_retcode = {
        .mov = 0xb8,
        .nr_rt_sigreturn = 173,
        .int80 = 0x80cd,
    };
    memcpy(frame->retcode, &rt_retcode, sizeof(rt_retcode));
}

static void setup_amd64_mcontext(struct amd64_mcontext_ *mcontext, struct cpu_state *cpu) {
    memset(mcontext, 0, sizeof(*mcontext));
    mcontext->gregs[AMD64_GREG_R8] = cpu->amd64_regs[amd64_r8];
    mcontext->gregs[AMD64_GREG_R9] = cpu->amd64_regs[amd64_r9];
    mcontext->gregs[AMD64_GREG_R10] = cpu->amd64_regs[amd64_r10];
    mcontext->gregs[AMD64_GREG_R11] = cpu->amd64_regs[amd64_r11];
    mcontext->gregs[AMD64_GREG_R12] = cpu->amd64_regs[amd64_r12];
    mcontext->gregs[AMD64_GREG_R13] = cpu->amd64_regs[amd64_r13];
    mcontext->gregs[AMD64_GREG_R14] = cpu->amd64_regs[amd64_r14];
    mcontext->gregs[AMD64_GREG_R15] = cpu->amd64_regs[amd64_r15];
    mcontext->gregs[AMD64_GREG_RDI] = cpu->amd64_regs[amd64_rdi];
    mcontext->gregs[AMD64_GREG_RSI] = cpu->amd64_regs[amd64_rsi];
    mcontext->gregs[AMD64_GREG_RBP] = cpu->amd64_regs[amd64_rbp];
    mcontext->gregs[AMD64_GREG_RBX] = cpu->amd64_regs[amd64_rbx];
    mcontext->gregs[AMD64_GREG_RDX] = cpu->amd64_regs[amd64_rdx];
    mcontext->gregs[AMD64_GREG_RAX] = cpu->amd64_regs[amd64_rax];
    mcontext->gregs[AMD64_GREG_RCX] = cpu->amd64_regs[amd64_rcx];
    mcontext->gregs[AMD64_GREG_RSP] = cpu->amd64_regs[amd64_rsp];
    mcontext->gregs[AMD64_GREG_RIP] = cpu->amd64_rip;
    collapse_flags(cpu);
    mcontext->gregs[AMD64_GREG_EFL] = cpu->eflags;
    // Linux x86_64 REG_CSGSFS packs CS, GS, FS, and a zero pad word.
    mcontext->gregs[AMD64_GREG_CSGSFS] =
        AMD64_USER_CS |
        ((qword_t) cpu->gs << 16);
    mcontext->gregs[AMD64_GREG_ERR] = signal_trap_error(cpu);
    mcontext->gregs[AMD64_GREG_TRAPNO] = 0;
    mcontext->gregs[AMD64_GREG_OLDMASK] = current->blocked;
}

static void setup_amd64_fpstate(struct amd64_fpstate_ *fpstate, struct cpu_state *cpu) {
    memset(fpstate, 0, sizeof(*fpstate));
    fpstate->cwd = cpu->fcw;
    fpstate->swd = cpu->fsw;
    fpstate->mxcsr = 0x1f80;

    for (int i = 0; i < 8; i++) {
        const float80 value = cpu->fp[i];
        for (int j = 0; j < 4; j++)
            fpstate->st[i].significand[j] = (word_t) (value.signif >> (j * 16));
        fpstate->st[i].exponent = value.signExp;
    }

    for (int i = 0; i < 16; i++)
        for (int j = 0; j < 4; j++)
            fpstate->xmm[i].element[j] = cpu->xmm[i].u32[j];
}

static void setup_rt_sigframe_amd64(struct siginfo_ *info, struct rt_sigframe_amd64 *frame) {
    memset(frame, 0, sizeof(*frame));
    frame->uc.flags = AMD64_UC_FP_XSTATE;
    frame->uc.link = 0;
    frame->uc.stack = (struct amd64_stack_t_marshaled) {
        .stack = current->altstack,
        .flags = current_altstack_flags(current),
        .size = current->altstack_size,
    };
    setup_amd64_mcontext(&frame->uc.mcontext, &current->cpu);
    setup_amd64_fpstate(&frame->uc.fpregs_mem, &current->cpu);
    if (signal_should_capture_trap_state(info->sig)) {
        frame->uc.mcontext.gregs[AMD64_GREG_TRAPNO] = current->cpu.trapno;
        if (current->cpu.trapno == INT_PF)
            frame->uc.mcontext.gregs[AMD64_GREG_CR2] = current->cpu.segfault_addr;
    }
    frame->uc.sigmask = current->blocked;
    siginfo_to_amd64_user(&frame->info, info);

    static const struct {
        uint8_t mov_rax_imm32;
        uint32_t nr_rt_sigreturn;
        uint16_t syscall;
    } __attribute__((packed)) rt_retcode = {
        .mov_rax_imm32 = 0xb8,
        .nr_rt_sigreturn = 15,
        .syscall = 0x050f,
    };
    memcpy(frame->retcode, &rt_retcode, sizeof(rt_retcode));
}

static void receive_signal(struct sighand *sighand, struct siginfo_ *info) {
    int sig = info->sig;
    STRACE("%d receiving signal %d\n", current->pid, sig);
    if (should_trace_signal_task(current)) {
        printk("tracked signal receive: pid=%d tgid=%d comm=%s sig=%d action=%d blocked=%#x pending=%#x waiting=%#x\n",
               current->pid, current->tgid, current->comm, sig,
               signal_action(sighand, sig), current->blocked, current->pending, current->waiting);
    }

    switch (signal_action(sighand, sig)) {
        case SIGNAL_IGNORE:
            return;

        case SIGNAL_STOP:
            lock(&current->group->lock,0);
            current->group->stopped = true;
            current->group->group_exit_code = sig << 8 | 0x7f;
            unlock(&current->group->lock);
            return;

        case SIGNAL_KILL:
            unlock(&sighand->lock); // do_exit must be called without this lock
            do_exit_group(sig);
    }

    struct sigaction_ *action = &sighand->action[info->sig];
    bool need_siginfo = action->flags & SA_SIGINFO_;

    guest_addr_t sp = current_user_sp(current);
    if (current->abi == GUEST_ABI_AMD64) {
        if ((action->flags & SA_ONSTACK_) && current->altstack && !is_on_altstack(sp, current))
            sp = current->altstack + current->altstack_size;
    } else {
        // Preserve longstanding i386 behavior. Existing 32-bit userspace in
        // this tree has historically run all handlers on the altstack when
        // one is configured, regardless of SA_ONSTACK.
        if (current->altstack && !is_on_altstack(sp, current))
            sp = current->altstack + current->altstack_size;
    }

    if (current->abi == GUEST_ABI_AMD64) {
        struct rt_sigframe_amd64 frame;
        size_t frame_size = sizeof(frame);
        setup_rt_sigframe_amd64(info, &frame);

        if (sp > 128)
            sp -= 128;
        if (xsave_extra) {
            sp -= xsave_extra;
            sp &= ~0x3full;
            sp -= fxsave_extra;
        }
        sp -= frame_size;
        sp = (sp & ~0xfull) - 8;

        guest_addr_t restorer = action->restorer;
        if (restorer == 0)
            restorer = sp + offsetof(struct rt_sigframe_amd64, retcode);
        frame.pretcode = restorer;
        frame.uc.mcontext.fpstate = sp + offsetof(struct rt_sigframe_amd64, uc.fpregs_mem);

        current->cpu.amd64_regs[amd64_rsp] = sp;
        current->cpu.esp = (dword_t) sp;
        current->cpu.amd64_rip = action->handler;
        current->cpu.eip = (dword_t) action->handler;
        current->cpu.amd64_regs[amd64_rdi] = info->sig;
        current->cpu.amd64_regs[amd64_rsi] = need_siginfo ? sp + offsetof(struct rt_sigframe_amd64, info) : 0;
        current->cpu.amd64_regs[amd64_rdx] = need_siginfo ? sp + offsetof(struct rt_sigframe_amd64, uc) : 0;
        current->cpu.edi = (dword_t) current->cpu.amd64_regs[amd64_rdi];
        current->cpu.esi = (dword_t) current->cpu.amd64_regs[amd64_rsi];
        current->cpu.edx = (dword_t) current->cpu.amd64_regs[amd64_rdx];

        if (!(action->flags & SA_NODEFER_))
            sigset_add(&current->blocked, info->sig);
        current->blocked |= action->mask;

        if (user_write(sp, &frame, frame_size)) {
            // The handler can't run (the stack is unwritable or gone). Linux
            // force_sigsegv kills with SIG_DFL here. Calling deliver_signal
            // would self-deadlock: receive_signals already holds
            // sighand->lock and deliver_signal takes it again.
            printk("WARNING: failed to install amd64 frame for %d at %#llx, killing\n",
                   info->sig, (unsigned long long) sp);
            unlock(&sighand->lock);
            do_exit_group(SIGSEGV_);
        }

        if (action->flags & SA_RESETHAND_)
            *action = (struct sigaction_) {.handler = SIG_DFL_};
        return;
    }

    // setup the frame
    union {
        struct sigframe_ sigframe;
        struct rt_sigframe_ rt_sigframe;
    } frame = {};
    size_t frame_size;
    if (need_siginfo) {
        setup_rt_sigframe(info, &frame.rt_sigframe);
        frame_size = sizeof(frame.rt_sigframe);
    } else {
        setup_sigframe(info, &frame.sigframe);
        frame_size = sizeof(frame.sigframe);
    }

    // set up registers for signal handler
    current->cpu.eax = info->sig;
    current->cpu.eip = action->handler;

    if (xsave_extra) {
        // do as the kernel does
        // this is superhypermega condensed version of fpu__alloc_mathframe in
        // arch/x86/kernel/fpu/signal.c
        sp -= xsave_extra;
        sp &=~ 0x3f;
        sp -= fxsave_extra;
    }
    sp -= frame_size;
    // align sp + 4 on a 16-byte boundary because that's what the abi says
    sp = ((sp + 4) & ~0xf) - 4;
    current->cpu.esp = sp;

    // Update the mask. By default the signal will be blocked while in the
    // handler, but sigaction is allowed to customize this.
    if (!(action->flags & SA_NODEFER_))
        sigset_add(&current->blocked, info->sig);
    current->blocked |= action->mask;

    // these have to be filled in after the location of the frame is known
    if (need_siginfo) {
        frame.rt_sigframe.pinfo = sp + offsetof(struct rt_sigframe_, info);
        frame.rt_sigframe.puc = sp + offsetof(struct rt_sigframe_, uc);
        current->cpu.edx = frame.rt_sigframe.pinfo;
        current->cpu.ecx = frame.rt_sigframe.puc;
    }

    // install frame
    if (user_write(sp, &frame, frame_size)) {
        // See the amd64 path above: kill like Linux force_sigsegv instead of
        // re-taking sighand->lock via deliver_signal and self-deadlocking.
        printk("WARNING: failed to install frame for %d at %#x, killing\n", info->sig, sp);
        unlock(&sighand->lock);
        do_exit_group(SIGSEGV_);
    }

    if (action->flags & SA_RESETHAND_)
        *action = (struct sigaction_) {.handler = SIG_DFL_};
}

void signal_delivery_stop(int sig, struct siginfo_ *info) {
    unlock(&current->sighand->lock);
    ptrace_signal_stop(sig, info);
    lock(&current->sighand->lock, 0);
}

void receive_signals(void) {  
    lock(&current->group->lock, 0);
    bool was_stopped = current->group->stopped;
    unlock(&current->group->lock);

    struct sighand *sighand = current->sighand;
    lock(&sighand->lock, 0);

    // A saved mask means that the last system call was a call like sigsuspend
    // that changes the mask during the call. Only ignore a signal right now if
    // it was both blocked during the call and should still be blocked after
    // the call.
    sigset_t_ blocked = current->blocked;
    if (current->has_saved_mask) {
        blocked &= current->saved_mask;
        current->has_saved_mask = false;
        current->blocked = current->saved_mask;
    }

    // Deliver pending unblocked signals LOWEST-NUMBERED-FIRST, matching Linux's
    // dequeue order (next_signal). When several are deliverable at once (e.g. a
    // sigprocmask that unblocks a whole set), each receive_signal stacks a frame
    // on top of the previous one, so the handlers RUN highest-first (LIFO) and
    // the per-frame saved mask is captured incrementally — bit-for-bit what real
    // Linux does. Previously this drained the queue in FIFO insertion order, so
    // a scrambled-order send ran the handlers in the wrong order.
    for (;;) {
        struct sigqueue *best = NULL;
        struct sigqueue *sigqueue;
        list_for_each_entry(&current->queue, sigqueue, queue) {
            if (sigset_has(blocked, sigqueue->info.sig))
                continue;
            if (best == NULL || sigqueue->info.sig < best->info.sig)
                best = sigqueue;
        }
        if (best == NULL)
            break;

        int sig = best->info.sig;
        struct siginfo_ info = best->info;
        list_remove(&best->queue);
        if (!signal_still_pending_locked(current, sig))
            sigset_del(&current->pending, sig);
        free(best);

        if (current->ptrace.traced && sig != SIGKILL_ &&
                sig != current->ptrace.deliver_sig) {
            // This notifies the parent, goes to sleep, and waits for the
            // parent to tell it to continue.
            // Any signals received while waiting are left on the queue, except
            // for SIGKILL_, which causes an immediate exit.
            signal_delivery_stop(sig, &info);
        } else {
            // A signal the tracer asked us to deliver (PTRACE_CONT with a
            // signal) is consumed and actually delivered exactly once, rather
            // than re-trapped through signal_delivery_stop (which would make the
            // tracer re-inject it forever).
            if (sig == current->ptrace.deliver_sig)
                current->ptrace.deliver_sig = 0;
            receive_signal(sighand, &info);
        }
    }

    unlock(&sighand->lock);

    // this got moved out of the switch case in receive_signal to fix locking problems
    if (!was_stopped) {
        lock(&current->group->lock, 0);
        bool now_stopped = current->group->stopped;
        // group_exit_code is (stop_sig << 8 | 0x7f) while stopped; recover the
        // bare stop signal for the SIGCHLD si_status.
        int stop_sig = (current->group->group_exit_code >> 8) & 0xff;
        unlock(&current->group->lock);
        if (now_stopped) {
            struct task *parent = NULL;
            int signal_no = 0;
            complex_lockt(&pids_lock, 0);
            parent = current->parent;
            if (parent != NULL) {
                task_ref_cnt_mod(parent, 1);
                signal_no = current->group->leader->exit_signal;
                notify(&parent->group->child_exit);
            }
            unlock(&pids_lock);
            if (parent != NULL) {
                // The stop SIGCHLD must carry CLD_STOPPED + the stop signal and
                // the child's pid/uid, not SIGINFO_NIL (which a SA_SIGINFO
                // handler / sigwaitinfo would read as SI_KERNEL with no child).
                struct siginfo_ info = {
                    .code = CLD_STOPPED_,
                    .child.pid = current->pid,
                    .child.uid = current->uid,
                    .child.status = stop_sig,
                };
                if (signal_no != 0)
                    send_signal(parent, signal_no, info);
                task_ref_cnt_mod(parent, -1);
            }
        }
    }
}

static void restore_sigcontext(struct sigcontext_ *context, struct cpu_state *cpu) {
    cpu->eax = context->ax;
    cpu->ebx = context->bx;
    cpu->ecx = context->cx;
    cpu->edx = context->dx;
    cpu->edi = context->di;
    cpu->esi = context->si;
    cpu->ebp = context->bp;
    cpu->esp = context->sp;
    cpu->eip = context->ip;
    collapse_flags(cpu);

    // Use AC, RF, OF, DF, TF, SF, ZF, AF, PF, CF
#define USE_FLAGS 0b1010000110111010101
    cpu->eflags = (context->flags & USE_FLAGS) | (cpu->eflags & ~USE_FLAGS);
    expand_flags(cpu);
    cpu->df_offset = cpu->df ? -1 : 1;
}

static void sync_i386_shadows_from_amd64(struct cpu_state *cpu) {
    cpu->eax = (dword_t) cpu->amd64_regs[amd64_rax];
    cpu->ebx = (dword_t) cpu->amd64_regs[amd64_rbx];
    cpu->ecx = (dword_t) cpu->amd64_regs[amd64_rcx];
    cpu->edx = (dword_t) cpu->amd64_regs[amd64_rdx];
    cpu->esi = (dword_t) cpu->amd64_regs[amd64_rsi];
    cpu->edi = (dword_t) cpu->amd64_regs[amd64_rdi];
    cpu->ebp = (dword_t) cpu->amd64_regs[amd64_rbp];
    cpu->esp = (dword_t) cpu->amd64_regs[amd64_rsp];
    cpu->eip = (dword_t) cpu->amd64_rip;
}

static void restore_amd64_fpstate(struct amd64_fpstate_ *fpstate, struct cpu_state *cpu) {
    cpu->fcw = fpstate->cwd;
    cpu->fsw = fpstate->swd;

    for (int i = 0; i < 8; i++) {
        uint64_t significand = 0;
        for (int j = 0; j < 4; j++)
            significand |= (uint64_t) fpstate->st[i].significand[j] << (j * 16);
        cpu->fp[i] = (float80) {
            .signif = significand,
            .signExp = fpstate->st[i].exponent,
        };
    }

    for (int i = 0; i < 16; i++)
        for (int j = 0; j < 4; j++)
            cpu->xmm[i].u32[j] = fpstate->xmm[i].element[j];
}

static void restore_amd64_mcontext(struct amd64_mcontext_ *mcontext, struct cpu_state *cpu) {
    cpu->amd64_regs[amd64_r8] = mcontext->gregs[AMD64_GREG_R8];
    cpu->amd64_regs[amd64_r9] = mcontext->gregs[AMD64_GREG_R9];
    cpu->amd64_regs[amd64_r10] = mcontext->gregs[AMD64_GREG_R10];
    cpu->amd64_regs[amd64_r11] = mcontext->gregs[AMD64_GREG_R11];
    cpu->amd64_regs[amd64_r12] = mcontext->gregs[AMD64_GREG_R12];
    cpu->amd64_regs[amd64_r13] = mcontext->gregs[AMD64_GREG_R13];
    cpu->amd64_regs[amd64_r14] = mcontext->gregs[AMD64_GREG_R14];
    cpu->amd64_regs[amd64_r15] = mcontext->gregs[AMD64_GREG_R15];
    cpu->amd64_regs[amd64_rdi] = mcontext->gregs[AMD64_GREG_RDI];
    cpu->amd64_regs[amd64_rsi] = mcontext->gregs[AMD64_GREG_RSI];
    cpu->amd64_regs[amd64_rbp] = mcontext->gregs[AMD64_GREG_RBP];
    cpu->amd64_regs[amd64_rbx] = mcontext->gregs[AMD64_GREG_RBX];
    cpu->amd64_regs[amd64_rdx] = mcontext->gregs[AMD64_GREG_RDX];
    cpu->amd64_regs[amd64_rax] = mcontext->gregs[AMD64_GREG_RAX];
    cpu->amd64_regs[amd64_rcx] = mcontext->gregs[AMD64_GREG_RCX];
    cpu->amd64_regs[amd64_rsp] = mcontext->gregs[AMD64_GREG_RSP];
    cpu->amd64_rip = mcontext->gregs[AMD64_GREG_RIP];
    cpu->trapno = (dword_t) mcontext->gregs[AMD64_GREG_TRAPNO];
    cpu->segfault_addr = mcontext->gregs[AMD64_GREG_CR2];

    cpu->eflags = (dword_t) mcontext->gregs[AMD64_GREG_EFL];
    expand_flags(cpu);
    cpu->df_offset = cpu->df ? -1 : 1;

    sync_i386_shadows_from_amd64(cpu);
}

dword_t sys_rt_sigreturn(void) {
    struct cpu_state *cpu = &current->cpu;
    if (current->abi == GUEST_ABI_AMD64)
        return (dword_t) sys_rt_sigreturn_amd64();

    struct rt_sigframe_ frame;
    // esp points past the first field of the frame
    if (user_get(cpu->esp - offsetof(struct rt_sigframe_, sig), frame)) {
        deliver_signal(current, SIGSEGV_, SIGINFO_NIL);
        return _EFAULT;
    }
    restore_sigcontext(&frame.uc.mcontext, cpu);

    lock(&current->sighand->lock, 0);
    restore_altstack(cpu->esp, frame.uc.stack.stack, frame.uc.stack.size, frame.uc.stack.flags);
    sigmask_set(frame.uc.sigmask);
    unlock(&current->sighand->lock);
    return cpu->eax;
}

qword_t sys_rt_sigreturn_amd64(void) {
    struct cpu_state *cpu = &current->cpu;
    struct rt_sigframe_amd64 frame;
    struct amd64_fpstate_ fpstate;
    guest_addr_t frame_addr = cpu->amd64_regs[amd64_rsp] - offsetof(struct rt_sigframe_amd64, uc);
    if (user_get(frame_addr, frame)) {
        deliver_signal(current, SIGSEGV_, SIGINFO_NIL);
        return _EFAULT;
    }

    restore_amd64_mcontext(&frame.uc.mcontext, cpu);
    if (frame.uc.mcontext.fpstate != 0) {
        if (user_get(frame.uc.mcontext.fpstate, fpstate)) {
            deliver_signal(current, SIGSEGV_, SIGINFO_NIL);
            return _EFAULT;
        }
        restore_amd64_fpstate(&fpstate, cpu);
    }

    lock(&current->sighand->lock, 0);
    restore_altstack(cpu->amd64_regs[amd64_rsp], frame.uc.stack.stack,
            frame.uc.stack.size, frame.uc.stack.flags);
    sigmask_set(frame.uc.sigmask);
    unlock(&current->sighand->lock);
    return cpu->amd64_regs[amd64_rax];
}

dword_t sys_sigreturn(void) {
    struct cpu_state *cpu = &current->cpu;
    struct sigframe_ frame;
    // esp points past the first two fields of the frame
    if (user_get(cpu->esp - offsetof(struct sigframe_, sc), frame)) {
        deliver_signal(current, SIGSEGV_, SIGINFO_NIL);
        return _EFAULT;
    }
    restore_sigcontext(&frame.sc, cpu);

    lock(&current->sighand->lock, 0);
    sigset_t_ oldmask = ((sigset_t_) frame.extramask << 32) | frame.sc.oldmask;
    sigmask_set(oldmask);
    unlock(&current->sighand->lock);
    return cpu->eax;
}

struct sighand *sighand_new(void) {
    struct sighand *sighand = malloc(sizeof(struct sighand));
    if (sighand == NULL)
        return NULL;
    memset(sighand, 0, sizeof(struct sighand));
    sighand->refcount = 1;
    lock_init(&sighand->lock, "sighand_new\0");
    return sighand;
}

struct sighand *sighand_copy(struct sighand *sighand) {
    struct sighand *new_sighand = sighand_new();
    if (new_sighand == NULL)
        return NULL;
    memcpy(new_sighand->action, sighand->action, sizeof(new_sighand->action));
    return new_sighand;
}

void sighand_retain(struct sighand *sighand) {
    atomic_fetch_add_explicit(&sighand->refcount, 1, memory_order_relaxed);
}

void sighand_release(struct sighand *sighand) {
    if (atomic_fetch_sub_explicit(&sighand->refcount, 1, memory_order_acq_rel) == 1) {
        free(sighand);
    }
}

static int do_sigaction(int sig, const struct sigaction_ *action, struct sigaction_ *oldaction) {
    if (sig >= NUM_SIGS)
        return _EINVAL;
    if (!signal_is_blockable(sig))
        return _EINVAL;

    struct sighand *sighand = current->sighand;
    lock(&sighand->lock, 0);
    struct sigaction_ prev_action = sighand->action[sig];
    if (oldaction)
        *oldaction = prev_action;
    if (action)
        sighand->action[sig] = *action;
    unlock(&sighand->lock);
    return 0;
}

dword_t sys_rt_sigaction(dword_t signum, addr_t action_addr, addr_t oldaction_addr, dword_t sigset_size) {
    return sys_rt_sigaction_guest(signum, action_addr, oldaction_addr, sigset_size);
}

dword_t sys_rt_sigaction_guest(dword_t signum, guest_addr_t action_addr, guest_addr_t oldaction_addr, dword_t sigset_size) {
    if (sigset_size != sizeof(sigset_t_))
        return _EINVAL;
    struct sigaction_ action = {};
    struct sigaction_ oldaction = {};
    if (action_addr != 0) {
        int err = sigaction_from_user(current, action_addr, &action);
        if (err < 0)
            return err;
    }
    STRACE("rt_sigaction(%d, %#llx {handler=%#llx, flags=%#llx, restorer=%#llx, mask=%#llx}, %#llx, %d)", signum,
            (unsigned long long) action_addr,
            (unsigned long long) action.handler,
            (unsigned long long) action.flags,
            (unsigned long long) action.restorer,
            (unsigned long long) action.mask,
            (unsigned long long) oldaction_addr, sigset_size);

    int err = do_sigaction(signum,
            action_addr ? &action : NULL,
            oldaction_addr ? &oldaction : NULL);
    if (err < 0)
        return err;

    if (oldaction_addr != 0) {
        err = sigaction_to_user(current, oldaction_addr, &oldaction);
        if (err < 0)
            return err;
    }
    return err;
}

dword_t sys_sigaction(dword_t signum, addr_t action_addr, addr_t oldaction_addr) {
    return sys_rt_sigaction(signum, action_addr, oldaction_addr, 1);
}

static void sigmask_set(sigset_t_ set) {
    current->blocked = (set & ~UNBLOCKABLE_MASK);
}

static void sigmask_set_temp_unlocked(sigset_t_ mask) {
    current->saved_mask = current->blocked;
    current->has_saved_mask = true;
    sigmask_set(mask);
}

void sigmask_set_temp(sigset_t_ mask) {
    lock(&current->sighand->lock, 0);
    sigmask_set_temp_unlocked(mask);
    unlock(&current->sighand->lock);
}

void sigmask_clear_temp(void) {
    lock(&current->sighand->lock, 0);
    if (current->has_saved_mask) {
        current->blocked = current->saved_mask;
        current->has_saved_mask = false;
    }
    unlock(&current->sighand->lock);
}

static int do_sigprocmask(dword_t how, sigset_t_ set) {
    if (how == SIG_BLOCK_)
        sigmask_set(current->blocked | set);
    else if (how == SIG_UNBLOCK_)
        sigmask_set(current->blocked & ~set);
    else if (how == SIG_SETMASK_)
        sigmask_set(set);
    else
        return _EINVAL;
    return 0;
}

dword_t sys_sigprocmask_guest(dword_t how, guest_addr_t set_addr, guest_addr_t oldset_addr) {
    dword_t set32 = 0;
    if (set_addr != 0)
        if (user_get(set_addr, set32))
            return _EFAULT;

    STRACE("sigprocmask(%s, %#llx, %#x)",
            how == SIG_BLOCK_ ? "SIG_BLOCK" :
            how == SIG_UNBLOCK_ ? "SIG_UNBLOCK" :
            how == SIG_SETMASK_ ? "SIG_SETMASK" : "??",
            set_addr != 0 ? (unsigned long long) set32 : ~0ull,
            oldset_addr);

    if (oldset_addr != 0) {
        dword_t oldset32 = (dword_t) current->blocked;
        if (user_put(oldset_addr, oldset32))
            return _EFAULT;
    }
    if (set_addr != 0) {
        struct sighand *sighand = current->sighand;
        lock(&sighand->lock, 0);
        int err = do_sigprocmask(how, (sigset_t_) set32);
        unlock(&sighand->lock);
        if (err < 0)
            return err;
    }
    return 0;
}

dword_t sys_sigprocmask(dword_t how, addr_t set_addr, addr_t oldset_addr) {
    return sys_sigprocmask_guest(how, set_addr, oldset_addr);
}

dword_t sys_rt_sigprocmask_guest(dword_t how, guest_addr_t set_addr, guest_addr_t oldset_addr, dword_t size) {
    if (size != sizeof(sigset_t_))
        return _EINVAL;

    sigset_t_ set = 0;
    if (set_addr != 0)
        if (user_get(set_addr, set))
            return _EFAULT;
    STRACE("rt_sigprocmask(%s, %#llx, %#x, %d)",
            how == SIG_BLOCK_ ? "SIG_BLOCK" :
            how == SIG_UNBLOCK_ ? "SIG_UNBLOCK" :
            how == SIG_SETMASK_ ? "SIG_SETMASK" : "??",
            set_addr != 0 ? (long long) set : -1, oldset_addr, size);

    if (oldset_addr != 0)
        if (user_put(oldset_addr, current->blocked))
            return _EFAULT;
    if (set_addr != 0) {
        struct sighand *sighand = current->sighand;
        lock(&sighand->lock, 0);
        int err = do_sigprocmask(how, set);
        unlock(&sighand->lock);
        if (err < 0)
            return err;
    }
    return 0;
}

dword_t sys_rt_sigprocmask(dword_t how, addr_t set_addr, addr_t oldset_addr, dword_t size) {
    return sys_rt_sigprocmask_guest(how, set_addr, oldset_addr, size);
}

int_t sys_rt_sigpending(addr_t set_addr) {
    return sys_rt_sigpending_guest(set_addr);
}

int_t sys_rt_sigpending_guest(guest_addr_t set_addr) {
    STRACE("rt_sigpending(%#llx)", (unsigned long long) set_addr);
    // as defined by the standard
    sigset_t_ pending = current->pending & current->blocked;
    if (user_put(set_addr, pending))
        return _EFAULT;
    return 0;
}

static bool is_on_altstack(guest_addr_t sp, struct task *task) {
    return sp > task->altstack && sp <= task->altstack + task->altstack_size;
}

static void restore_altstack(guest_addr_t sp, guest_addr_t stack, guest_addr_t size, dword_t flags) {
    if (is_on_altstack(sp, current))
        return;
    if (flags & SS_DISABLE_) {
        current->altstack = 0;
        current->altstack_size = 0;
        return;
    }
    if (size >= MINSIGSTKSZ_) {
        current->altstack = stack;
        current->altstack_size = size;
    }
}

static dword_t current_altstack_flags(struct task *task) {
    dword_t flags = 0;
    if (task->altstack == 0)
        flags |= SS_DISABLE_;
    if (is_on_altstack(current_user_sp(task), task))
        flags |= SS_ONSTACK_;
    return flags;
}

static void altstack_to_i386_user(struct task *task, struct stack_t_ *user_stack) {
    user_stack->stack = task->altstack;
    user_stack->flags = current_altstack_flags(task);
    user_stack->size = (dword_t) task->altstack_size;
}

static int altstack_to_user(struct task *task, guest_addr_t user_addr) {
    dword_t flags = current_altstack_flags(task);
    if (task->abi == GUEST_ABI_AMD64) {
        struct amd64_stack_t_marshaled user_stack = {
            .stack = task->altstack,
            .flags = flags,
            .size = task->altstack_size,
        };
        if (user_put(user_addr, user_stack))
            return _EFAULT;
    } else {
        struct stack_t_ user_stack = {
            .stack = task->altstack,
            .flags = flags,
            .size = (dword_t) task->altstack_size,
        };
        if (user_put(user_addr, user_stack))
            return _EFAULT;
    }
    return 0;
}

static int altstack_from_user(struct task *task, guest_addr_t user_addr, guest_addr_t *stack_out, guest_addr_t *size_out, dword_t *flags_out) {
    if (task->abi == GUEST_ABI_AMD64) {
        struct amd64_stack_t_marshaled user_stack;
        if (user_get(user_addr, user_stack))
            return _EFAULT;
        *stack_out = user_stack.stack;
        *size_out = user_stack.size;
        *flags_out = user_stack.flags;
    } else {
        struct stack_t_ user_stack;
        if (user_get(user_addr, user_stack))
            return _EFAULT;
        *stack_out = user_stack.stack;
        *size_out = user_stack.size;
        *flags_out = user_stack.flags;
    }
    return 0;
}

dword_t sys_sigaltstack(guest_addr_t ss_addr, guest_addr_t old_ss_addr) {
    STRACE("sigaltstack(%#llx, %#llx)", (unsigned long long) ss_addr, (unsigned long long) old_ss_addr);
    struct sighand *sighand = current->sighand;
    lock(&sighand->lock, 0);
    if (old_ss_addr != 0) {
        if (altstack_to_user(current, old_ss_addr)) {
            unlock(&sighand->lock);
            return _EFAULT;
        }
    }
    if (ss_addr != 0) {
        if (is_on_altstack(current_user_sp(current), current)) {
            unlock(&sighand->lock);
            return _EPERM;
        }
        guest_addr_t stack;
        guest_addr_t size;
        dword_t flags;
        int err = altstack_from_user(current, ss_addr, &stack, &size, &flags);
        if (err < 0) {
            unlock(&sighand->lock);
            return err;
        }
        if (flags & SS_DISABLE_) {
            current->altstack = 0;
            current->altstack_size = 0;
        } else {
            if (size < MINSIGSTKSZ_) {
                unlock(&sighand->lock);
                return _ENOMEM;
            }
            current->altstack = stack;
            current->altstack_size = size;
        }
    }
    unlock(&sighand->lock);
    return 0;
}

dword_t sys_sigaltstack_guest(guest_addr_t ss_addr, guest_addr_t old_ss_addr) {
    return sys_sigaltstack(ss_addr, old_ss_addr);
}

int_t sys_rt_sigsuspend(addr_t mask_addr, uint_t size) {
    return sys_rt_sigsuspend_guest(mask_addr, size);
}

int_t sys_rt_sigsuspend_guest(guest_addr_t mask_addr, uint_t size) {
    if (size != sizeof(sigset_t_))
        return _EINVAL;
    sigset_t_ mask;
    if (user_get(mask_addr, mask))
        return _EFAULT;
    STRACE("sigsuspend(0x%llx) = ...\n", (long long) mask);
    lock(&current->sighand->lock, 0);
    sigmask_set_temp_unlocked(mask);
    TASK_MAY_BLOCK {
        while (wait_for(&current->pause, &current->sighand->lock, NULL) != _EINTR)
            continue;
    }
    unlock(&current->sighand->lock);
    STRACE("%d done sigsuspend", current->pid);
    return _EINTR;
}

int_t sys_pause(void) {
    lock(&current->sighand->lock, 0);
    TASK_MAY_BLOCK {
        while (wait_for(&current->pause, &current->sighand->lock, NULL) != _EINTR)
            continue;
    }
    unlock(&current->sighand->lock);
    return _EINTR;
}

static int_t sys_rt_sigtimedwait_common(guest_addr_t set_addr, guest_addr_t info_addr, guest_addr_t timeout_addr, uint_t set_size,
        bool timeout_time64) {
    if (set_size != sizeof(sigset_t_))
        return _EINVAL;
    sigset_t_ set;
    if (user_get(set_addr, set))
        return _EFAULT;
    struct timespec timeout;
    if (timeout_addr != 0) {
        // The amd64 ABI's native struct timespec is 64-bit (== timespec64_).
        // Reading it as the 32-bit i386 struct timespec_ pulled tv_nsec out of
        // the high half of tv_sec — always 0 for any sub-second timeout — so
        // the EINVAL range check never fired and sub-second waits collapsed to
        // an immediate EAGAIN. amd64 must always read the 64-bit layout.
        bool read64 = timeout_time64 || current->abi == GUEST_ABI_AMD64;
        if (read64) {
            struct timespec64_ fake_timeout;
            if (user_get(timeout_addr, fake_timeout))
                return _EFAULT;
            timeout.tv_sec = fake_timeout.sec;
            timeout.tv_nsec = fake_timeout.nsec;
        } else {
            struct timespec_ fake_timeout;
            if (user_get(timeout_addr, fake_timeout))
                return _EFAULT;
            timeout.tv_sec = fake_timeout.sec;
            timeout.tv_nsec = fake_timeout.nsec;
        }
        if (timeout.tv_sec < 0 || timeout.tv_nsec < 0 || timeout.tv_nsec >= 1000000000)
            return _EINVAL;
    }
    STRACE("sigtimedwait(%#llx, %#x, %#x) = ...\n", (long long) set, info_addr, timeout_addr);

    lock(&current->sighand->lock, 0);
    struct siginfo_ info;
    if (signal_take_next_locked(current, set, &info)) {
        unlock(&current->sighand->lock);
        if (info_addr != 0)
            if (siginfo_to_user(current, info_addr, &info))
                return _EFAULT;
        STRACE("done sigtimedwait immediate = %d\n", info.sig);
        return info.sig;
    }
    assert(current->waiting == 0);
    current->waiting = set;
    int err = 0;
    TASK_MAY_BLOCK {
        do {
            err = wait_for(&current->pause, &current->sighand->lock, timeout_addr == 0 ? NULL : &timeout);
        } while (err == 0);
    }
    current->waiting = 0;
    if (err == _ETIMEDOUT) {
        unlock(&current->sighand->lock);
        STRACE("sigtimedwait timed out\n");
        return _EAGAIN;
    }

    bool found = signal_take_next_locked(current, set, &info);
    unlock(&current->sighand->lock);
    if (!found)
        return _EINTR;
    if (info_addr != 0)
        if (siginfo_to_user(current, info_addr, &info))
            return _EFAULT;
    STRACE("done sigtimedwait = %d\n", info.sig);
    return info.sig;
}

int_t sys_rt_sigtimedwait(addr_t set_addr, addr_t info_addr, addr_t timeout_addr, uint_t set_size) {
    return sys_rt_sigtimedwait_common(set_addr, info_addr, timeout_addr, set_size, false);
}

int_t sys_rt_sigtimedwait_guest(guest_addr_t set_addr, guest_addr_t info_addr, guest_addr_t timeout_addr, uint_t set_size) {
    return sys_rt_sigtimedwait_common(set_addr, info_addr, timeout_addr, set_size, false);
}

int_t sys_rt_sigtimedwait_time64(addr_t set_addr, addr_t info_addr, addr_t timeout_addr, uint_t set_size) {
    return sys_rt_sigtimedwait_common(set_addr, info_addr, timeout_addr, set_size, true);
}

int_t sys_rt_sigtimedwait_time64_guest(guest_addr_t set_addr, guest_addr_t info_addr, guest_addr_t timeout_addr, uint_t set_size) {
    return sys_rt_sigtimedwait_common(set_addr, info_addr, timeout_addr, set_size, true);
}

static int kill_task(struct task *task, dword_t sig, int si_code) {
    // FIXME: Need to check references to kernel here to be sure they are zero
    if (!superuser() &&
            current->uid != task->uid &&
            current->uid != task->suid &&
            current->euid != task->uid &&
            current->euid != task->suid) {
        return _EPERM;
    }
    // kill(2) reports SI_USER; tkill/tgkill(2) report SI_TKILL. A handler that
    // inspects si_code (or si_pid, which is meaningless for SI_TKILL) must see
    // the right one — glibc raise() routes through tgkill, so this is common.
    struct siginfo_ info = {
        .code = si_code,
        .kill.pid = current->pid,
        .kill.uid = current->uid,
    };

    send_signal(task, sig, info);
    return 0;
}

static int queue_signal_task(struct task *task, dword_t sig, struct siginfo_ info) {
    if (!superuser() &&
            current->uid != task->uid &&
            current->uid != task->suid &&
            current->euid != task->uid &&
            current->euid != task->suid) {
        return _EPERM;
    }

    send_signal(task, sig, info);
    return 0;
}

struct kill_target {
    struct task *task;
};

static int kill_group(pid_t_ pgid, dword_t sig, int si_code) {
    struct kill_target stack_targets[32];
    struct kill_target *targets = stack_targets;
    size_t target_cap = sizeof(stack_targets) / sizeof(stack_targets[0]);
    size_t target_count = 0;
    struct pid *pid = pid_get(pgid);
    if (pid == NULL) {
        unlock(&pids_lock);
        return _ESRCH;
    }
retry:
    target_count = 0;
    size_t needed = 0;
    struct tgroup *tgroup;
    list_for_each_entry(&pid->pgroup, tgroup, pgroup)
        needed++;
    if (needed > target_cap) {
        unlock(&pids_lock);
        if (targets != stack_targets)
            free(targets);
        targets = malloc(sizeof(*targets) * needed);
        if (targets == NULL)
            return _ENOMEM;
        target_cap = needed;

        complex_lockt(&pids_lock, 0);
        pid = pid_get(pgid);
        if (pid == NULL) {
            unlock(&pids_lock);
            free(targets);
            return _ESRCH;
        }
        goto retry;
    }

    list_for_each_entry(&pid->pgroup, tgroup, pgroup) {
        struct task *task = tgroup->leader;
        if (task == NULL || task->zombie || task->exiting)
            continue;
        task_ref_cnt_mod(task, 1);
        targets[target_count++] = (struct kill_target) {.task = task};
    }
    unlock(&pids_lock);

    int err = _EPERM;
    for (size_t i = 0; i < target_count; i++) {
        int kill_err = kill_task(targets[i].task, sig, si_code);
        task_ref_cnt_mod(targets[i].task, -1);
        if (err == _EPERM)
            err = kill_err;
    }
    if (targets != stack_targets)
        free(targets);
    return err;
}

static int kill_everything(dword_t sig, int si_code) {
    struct kill_target stack_targets[64];
    struct kill_target *targets = stack_targets;
    size_t target_cap = sizeof(stack_targets) / sizeof(stack_targets[0]);
    size_t target_count = 0;

retry:
    target_count = 0;
    if (targets == NULL)
        return _ENOMEM;

    for (int i = 2; i < MAX_PID; i++) {
        struct task *task = pid_get_task(i);
        if (task == NULL || task == current || !task_is_leader(task))
            continue;
        if (target_count == target_cap) {
            unlock(&pids_lock);
            size_t new_cap = target_cap * 2;
            struct kill_target *new_targets = targets == stack_targets
                ? malloc(sizeof(*new_targets) * new_cap)
                : realloc(targets, sizeof(*new_targets) * new_cap);
            if (new_targets == NULL) {
                if (targets != stack_targets)
                    free(targets);
                return _ENOMEM;
            }
            if (targets == stack_targets)
                memcpy(new_targets, stack_targets, sizeof(stack_targets));
            targets = new_targets;
            target_cap = new_cap;
            complex_lockt(&pids_lock, 0);
            goto retry;
        }
        task_ref_cnt_mod(task, 1);
        targets[target_count++] = (struct kill_target) {.task = task};
    }
    unlock(&pids_lock);

    int err = _EPERM;
    for (size_t i = 0; i < target_count; i++) {
        int kill_err = kill_task(targets[i].task, sig, si_code);
        task_ref_cnt_mod(targets[i].task, -1);
        if (err == _EPERM)
            err = kill_err;
    }
    if (targets != stack_targets)
        free(targets);
    return err;
}

// si_code distinguishes the sender: SI_USER for kill(2), SI_TKILL for
// tkill/tgkill(2). Linux forces this on the receiving side, so we thread it
// down from the syscall entry point rather than letting kill_task assume SI_USER.
static int do_kill(pid_t_ pid, dword_t sig, pid_t_ tgid, int si_code) {
    STRACE("kill(%d, %d)", pid, sig);
    if (sig >= NUM_SIGS)
        return _EINVAL;
    if (pid == 0) {
        lock(&current->group->lock, 0);
        pid = -current->group->pgid;
        unlock(&current->group->lock);
    }

    int err;
    if (pid == -1) {
        complex_lockt(&pids_lock, 0);
        err = kill_everything(sig, si_code);
    } else if (pid < 0) {
        complex_lockt(&pids_lock, 0);
        err = kill_group(-pid, sig, si_code);
    } else {
        struct task *task = pid_get_task_ref(pid);
        if (task == NULL) {
            return _ESRCH;
        }

        // If tgid is nonzero, it must be correct
        if (tgid != 0 && task->tgid != tgid) {
            task_ref_cnt_mod(task, -1);
            return _ESRCH;
        }

        err = kill_task(task, sig, si_code);
        task_ref_cnt_mod(task, -1);
    }
    return err;
}

dword_t sys_kill(pid_t_ pid, dword_t sig) {
    return do_kill(pid, sig, 0, SI_USER_);
}
dword_t sys_tgkill(pid_t_ tgid, pid_t_ tid, dword_t sig) {
    if (tid <= 0 || tgid <= 0)
        return _EINVAL;
    return do_kill(tid, sig, tgid, SI_TKILL_);
}
dword_t sys_tkill(pid_t_ tid, dword_t sig) {
    if (tid <= 0)
        return _EINVAL;
    return do_kill(tid, sig, 0, SI_TKILL_);
}

dword_t sys_rt_sigqueueinfo(pid_t_ pid, dword_t sig, addr_t uinfo_addr) {
    return sys_rt_sigqueueinfo_guest(pid, sig, uinfo_addr);
}

dword_t sys_rt_sigqueueinfo_guest(pid_t_ pid, dword_t sig, guest_addr_t uinfo_addr) {
    if (pid <= 0 || sig <= 0 || sig >= NUM_SIGS)
        return _EINVAL;

    struct siginfo_ info;
    int err = siginfo_from_user(current, uinfo_addr, &info);
    if (err < 0)
        return err;

    info.sig = sig;
    info.sig_errno = 0;
    info.code = SI_QUEUE_;
    info.rt.pid = current->pid;
    info.rt.uid = current->uid;

    struct task *task = pid_get_task_ref(pid);
    if (task == NULL) {
        return _ESRCH;
    }

    err = queue_signal_task(task, sig, info);
    task_ref_cnt_mod(task, -1);
    return err;
}

dword_t sys_rt_tgsigqueueinfo_guest(pid_t_ tgid, pid_t_ tid, dword_t sig, guest_addr_t uinfo_addr) {
    if (tgid <= 0 || tid <= 0 || sig <= 0 || sig >= NUM_SIGS)
        return _EINVAL;

    struct siginfo_ info;
    int err = siginfo_from_user(current, uinfo_addr, &info);
    if (err < 0)
        return err;

    info.sig = sig;
    info.sig_errno = 0;

    struct task *task = pid_get_task_ref(tid);
    if (task == NULL) {
        return _ESRCH;
    }
    if (task->tgid != tgid) {
        task_ref_cnt_mod(task, -1);
        return _ESRCH;
    }

    err = queue_signal_task(task, sig, info);
    task_ref_cnt_mod(task, -1);
    return err;
}

dword_t sys_rt_tgsigqueueinfo(pid_t_ tgid, pid_t_ tid, dword_t sig, addr_t uinfo_addr) {
    return sys_rt_tgsigqueueinfo_guest(tgid, tid, sig, uinfo_addr);
}
