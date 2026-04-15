#include "ptrace.h"
#include "kernel/calls.h"
#include "kernel/errno.h"
#include "kernel/signal.h"
#include "task.h"
#include <string.h>

static struct task *ptrace_tracer(struct task *task) {
    if (task->ptrace.tracer != NULL)
        return task->ptrace.tracer;
    return task->parent;
}

// Returns stopped tracee with the given pid, locked with the ptrace lock
static struct task *find_child(pid_t_ pid) {
    struct task *child = NULL;
    list_for_each_entry(&current->children, child, siblings) {
        if (child->pid != pid)
            continue;
        lock(&child->ptrace.lock, 0);
        if (child->ptrace.stopped)
            return child;
        unlock(&child->ptrace.lock);
    }
    list_for_each_entry(&current->ptracees, child, ptrace_siblings) {
        if (child->pid != pid)
            continue;
        lock(&child->ptrace.lock, 0);
        if (child->ptrace.stopped)
            return child;
        unlock(&child->ptrace.lock);
    }
    return NULL;
}

void ptrace_attach_fork_child(struct task *child, struct task *tracee) {
    struct task *tracer = ptrace_tracer(tracee);
    if (tracer == NULL)
        return;

    complex_lockt(&pids_lock, 0);
    child->ptrace.traced = true;
    child->ptrace.sysgood = tracee->ptrace.sysgood;
    child->ptrace.options = tracee->ptrace.options;
    child->ptrace.tracer = tracer;
    list_add(&tracer->ptracees, &child->ptrace_siblings);
    unlock(&pids_lock);
}

static void sync_i386_shadows_from_amd64_ptrace(struct cpu_state *cpu) {
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

static void get_user_regs_amd64(struct task *task, struct user_regs_struct_amd64_ *user_regs_) {
    struct cpu_state *cpu = &task->cpu;
    memset(user_regs_, 0, sizeof(*user_regs_));
    user_regs_->r15 = cpu->amd64_regs[amd64_r15];
    user_regs_->r14 = cpu->amd64_regs[amd64_r14];
    user_regs_->r13 = cpu->amd64_regs[amd64_r13];
    user_regs_->r12 = cpu->amd64_regs[amd64_r12];
    user_regs_->rbp = cpu->amd64_regs[amd64_rbp];
    user_regs_->rbx = cpu->amd64_regs[amd64_rbx];
    user_regs_->r11 = cpu->amd64_regs[amd64_r11];
    user_regs_->r10 = cpu->amd64_regs[amd64_r10];
    user_regs_->r9 = cpu->amd64_regs[amd64_r9];
    user_regs_->r8 = cpu->amd64_regs[amd64_r8];
    user_regs_->rax = cpu->amd64_regs[amd64_rax];
    user_regs_->rcx = cpu->amd64_regs[amd64_rcx];
    user_regs_->rdx = cpu->amd64_regs[amd64_rdx];
    user_regs_->rsi = cpu->amd64_regs[amd64_rsi];
    user_regs_->rdi = cpu->amd64_regs[amd64_rdi];
    user_regs_->orig_rax = task->ptrace.syscall;
    user_regs_->rip = cpu->amd64_rip;
    user_regs_->cs = 0x33;
    user_regs_->eflags = cpu->eflags;
    user_regs_->rsp = cpu->amd64_regs[amd64_rsp];
    user_regs_->ss = 0x2b;
    user_regs_->fs_base = cpu->tls_ptr;
}

static void set_user_regs_amd64(struct cpu_state *cpu, const struct user_regs_struct_amd64_ *user_regs_) {
    cpu->amd64_regs[amd64_r15] = user_regs_->r15;
    cpu->amd64_regs[amd64_r14] = user_regs_->r14;
    cpu->amd64_regs[amd64_r13] = user_regs_->r13;
    cpu->amd64_regs[amd64_r12] = user_regs_->r12;
    cpu->amd64_regs[amd64_rbp] = user_regs_->rbp;
    cpu->amd64_regs[amd64_rbx] = user_regs_->rbx;
    cpu->amd64_regs[amd64_r11] = user_regs_->r11;
    cpu->amd64_regs[amd64_r10] = user_regs_->r10;
    cpu->amd64_regs[amd64_r9] = user_regs_->r9;
    cpu->amd64_regs[amd64_r8] = user_regs_->r8;
    cpu->amd64_regs[amd64_rax] = user_regs_->rax;
    cpu->amd64_regs[amd64_rcx] = user_regs_->rcx;
    cpu->amd64_regs[amd64_rdx] = user_regs_->rdx;
    cpu->amd64_regs[amd64_rsi] = user_regs_->rsi;
    cpu->amd64_regs[amd64_rdi] = user_regs_->rdi;
    cpu->amd64_rip = user_regs_->rip;
    cpu->eflags = (dword_t) user_regs_->eflags;
    expand_flags(cpu);
    cpu->df_offset = cpu->df ? -1 : 1;
    cpu->amd64_regs[amd64_rsp] = user_regs_->rsp;
    cpu->tls_ptr = user_regs_->fs_base;
    sync_i386_shadows_from_amd64_ptrace(cpu);
}

static size_t ptrace_word_size(const struct task *task) {
    return task->abi == GUEST_ABI_AMD64 ? sizeof(qword_t) : sizeof(dword_t);
}

// Ensure stopped, ptrace locked, etc. before calling this
static void get_user_regs(struct cpu_state *cpu, struct user_regs_struct_ *user_regs_) {
    user_regs_->ebx = cpu->ebx;
    user_regs_->ecx = cpu->ecx;
    user_regs_->edx = cpu->edx;
    user_regs_->esi = cpu->esi;
    user_regs_->edi = cpu->edi;
    user_regs_->ebp = cpu->ebp;
    user_regs_->eax = cpu->eax;
//  user_regs_->xds = cpu->xds;
//  user_regs_->xes = cpu->xes;
//  user_regs_->xfs = cpu->xfs;
//  user_regs_->xgs = cpu->xgs;
    user_regs_->orig_eax = cpu->eax;
    user_regs_->eip = cpu->eip;
//  user_regs_->xcs = cpu->xcs;
    user_regs_->eflags = cpu->eflags;
    user_regs_->esp = cpu->esp;
//  user_regs_->xss = cpu->xss;
}

// Ensure stopped, ptrace locked, etc. before calling this
static void get_user_regs_and_syscall(struct task *task, struct user_regs_struct_ *user_regs_) {
    get_user_regs(&task->cpu, user_regs_);
    user_regs_->orig_eax = task->ptrace.syscall;
}

// Ensure stopped, ptrace locked, etc. before calling this
static void set_user_regs(struct cpu_state *cpu, struct user_regs_struct_ *user_regs_) {
    cpu->ebx = user_regs_->ebx;
    cpu->ecx = user_regs_->ecx;
    cpu->edx = user_regs_->edx;
    cpu->esi = user_regs_->esi;
    cpu->edi = user_regs_->edi;
    cpu->ebp = user_regs_->ebp;
    cpu->eax = user_regs_->eax;
//  cpu->xds = user_regs_->xds;
//  cpu->xes = user_regs_->xes;
//  cpu->xfs = user_regs_->xfs;
//  cpu->xgs = user_regs_->xgs;
//  cpu->eax = user_regs_->orig_eax;
    cpu->eip = user_regs_->eip;
//  cpu->xcs = user_regs_->xcs;
    cpu->eflags = user_regs_->eflags;
    expand_flags(cpu);
    cpu->df_offset = cpu->df ? -1 : 1;
    cpu->esp = user_regs_->esp;
//  cpu->xss = user_regs_->xss;
}

static void ptrace_stop_common(int sig, const struct siginfo_ *info, bool syscall_stop) {
    lock(&current->ptrace.lock, 0);
    current->ptrace.stopped = true;
    current->ptrace.signal = sig;
    if (syscall_stop && current->ptrace.sysgood)
        current->ptrace.signal |= 0x80;
    current->ptrace.info = *info;
    unlock(&current->ptrace.lock);

    struct task *tracer = ptrace_tracer(current);
    if (tracer != NULL) {
        notify(&tracer->group->child_exit);
        send_signal(tracer, current->group->leader->exit_signal, SIGINFO_NIL);
    }

    lock(&current->ptrace.lock, 0);
    TASK_MAY_BLOCK {
        while (current->ptrace.stopped) {
            wait_for_ignore_signals(&current->ptrace.cond, &current->ptrace.lock, NULL);
            lock(&current->sighand->lock, 0);
            bool got_sigkill = sigset_has(current->pending, SIGKILL_);
            unlock(&current->sighand->lock);
            if (got_sigkill) {
                STRACE("%d received a SIGKILL in ptrace stop\n", current->pid);
                unlock(&current->ptrace.lock);
                do_exit_group(SIGKILL_);
            }
        }
    }
    unlock(&current->ptrace.lock);
}

void ptrace_signal_stop(int sig, struct siginfo_ *info) {
    ptrace_stop_common(sig, info, false);
}

void ptrace_event_stop(int sig, struct siginfo_ *info, int event, dword_t eventmsg) {
    lock(&current->ptrace.lock, 0);
    current->ptrace.trap_event = event;
    current->ptrace.eventmsg = eventmsg;
    unlock(&current->ptrace.lock);
    ptrace_stop_common(sig, info, false);
}

void ptrace_syscall_stop(struct cpu_state *cpu) {
    struct siginfo_ info = {
        .sig = SIGTRAP_,
        .code = SIGTRAP_,
    };

    lock(&current->ptrace.lock, 0);
    if (!current->ptrace.syscall_stopped)
        current->ptrace.syscall = current->abi == GUEST_ABI_AMD64 ?
            (int) cpu->amd64_regs[amd64_rax] : (int) cpu->eax;
    current->ptrace.syscall_stopped = !current->ptrace.syscall_stopped;
    unlock(&current->ptrace.lock);

    ptrace_stop_common(SIGTRAP_, &info, true);
}

dword_t sys_ptrace(dword_t request, dword_t pid, addr_t addr, dword_t data) {
    return sys_ptrace_guest(request, pid, addr, data);
}

dword_t sys_ptrace_guest(dword_t request, dword_t pid, guest_addr_t addr, guest_addr_t data) {
    switch (request) {
        case PTRACE_TRACEME_:
            STRACE("ptrace(PTRACE_TRACEME, %d, %#llx, %#llx)", pid,
                    (unsigned long long) addr, (unsigned long long) data);
            current->ptrace.traced = true;
            current->ptrace.tracer = current->parent;
            return 0;

        case PTRACE_PEEKTEXT_:
        case PTRACE_PEEKDATA_: {
            STRACE("ptrace(PTRACE_PEEKDATA, %d, %#llx, %#llx)", pid,
                    (unsigned long long) addr, (unsigned long long) data);
            struct task *child = find_child(pid);
            if (!child) return _EPERM;

            if (child->abi == GUEST_ABI_AMD64) {
                qword_t peek;
                if (user_get_task(child, addr, peek) || user_put(data, peek)) {
                    unlock(&child->ptrace.lock);
                    return _EFAULT;
                }
            } else {
                dword_t peek;
                if (user_get_task(child, addr, peek) || user_put(data, peek)) {
                    unlock(&child->ptrace.lock);
                    return _EFAULT;
                }
            }
            unlock(&child->ptrace.lock);

            return 0;
        }

        case PTRACE_PEEKUSER_: {
            STRACE("ptrace(PTRACE_PEEKUSER, %d, %#llx, %#llx)", pid,
                    (unsigned long long) addr, (unsigned long long) data);
            struct task *child = find_child(pid);
            if (!child) return _EPERM;

            if (child->abi == GUEST_ABI_AMD64) {
                qword_t peek;
                struct user_regs_struct_amd64_ user_regs_amd64 = {};
                get_user_regs_amd64(child, &user_regs_amd64);

                if (addr & (sizeof(peek) - 1) || addr >= sizeof(user_regs_amd64)) {
                    unlock(&child->ptrace.lock);
                    return _EIO;
                }

                memcpy(&peek, (char *) &user_regs_amd64 + addr, sizeof(peek));
                if (user_put(data, peek)) {
                    unlock(&child->ptrace.lock);
                    return _EFAULT;
                }
            } else {
                dword_t peek;
                struct user_ user_ = {};
                get_user_regs_and_syscall(child, &user_.user_regs);

                if (addr & (sizeof(peek) - 1) || addr >= sizeof(struct user_)) {
                    unlock(&child->ptrace.lock);
                    return _EIO;
                }

                memcpy(&peek, (char *) &user_ + addr, sizeof(peek));
                if (user_put(data, peek)) {
                    unlock(&child->ptrace.lock);
                    return _EFAULT;
                }
            }
            unlock(&child->ptrace.lock);

            return 0;
        }

        case PTRACE_POKETEXT_:
        case PTRACE_POKEDATA_: {
            STRACE("ptrace(PTRACE_POKEDATA, %d, %#llx, %#llx)", pid,
                    (unsigned long long) addr, (unsigned long long) data);
            struct task *child = find_child(pid);
            if (!child) return _EPERM;

            if (user_write_task_ptrace(child, addr, &data, ptrace_word_size(child))) {
                unlock(&child->ptrace.lock);
                return _EFAULT;
            }
            unlock(&child->ptrace.lock);

            return 0;
        }

        case PTRACE_CONT_: {
            STRACE("ptrace(PTRACE_CONT, %d, %#llx, %#llx)", pid,
                    (unsigned long long) addr, (unsigned long long) data);
            struct task *child = find_child(pid);
            if (!child) return _EPERM;

            child->cpu.tf = false;
            child->ptrace.stop_at_syscall = false;
            child->ptrace.syscall_stopped = false;
            child->ptrace.stopped = false;
            notify(&child->ptrace.cond);
            unlock(&child->ptrace.lock);

            return 0;
        }

        case PTRACE_KILL_: {
            STRACE("ptrace(PTRACE_KILL, %d, %#llx, %#llx)", pid,
                    (unsigned long long) addr, (unsigned long long) data);
            struct task *child = find_child(pid);
            if (!child) return _EPERM;

            child->ptrace.stopped = false;
            send_signal(child, SIGKILL_, SIGINFO_NIL);
            unlock(&child->ptrace.lock);

            return 0;
        }

        case PTRACE_SINGLESTEP_: {
            STRACE("ptrace(PTRACE_SINGLESTEP, %d, %#llx, %#llx)", pid,
                    (unsigned long long) addr, (unsigned long long) data);
            struct task *child = find_child(pid);
            if (!child) return _EPERM;

            child->cpu.tf = true;
            child->ptrace.stop_at_syscall = false;
            child->ptrace.syscall_stopped = false;
            child->ptrace.stopped = false;
            notify(&child->ptrace.cond);
            unlock(&child->ptrace.lock);

            return 0;
        }

        case PTRACE_GETREGS_: {
            STRACE("ptrace(PTRACE_GETREGS, %d, %#llx, %#llx)", pid,
                    (unsigned long long) addr, (unsigned long long) data);
            struct task *child = find_child(pid);
            if (!child) return _EPERM;

            if (child->abi == GUEST_ABI_AMD64) {
                struct user_regs_struct_amd64_ user_regs_amd64 = {};
                get_user_regs_amd64(child, &user_regs_amd64);
                if (user_put(data, user_regs_amd64)) {
                    unlock(&child->ptrace.lock);
                    return _EFAULT;
                }
            } else {
                struct user_regs_struct_ user_regs_ = {};
                get_user_regs_and_syscall(child, &user_regs_);
                user_regs_.orig_eax = child->ptrace.syscall;
                if (user_put(data, user_regs_)) {
                    unlock(&child->ptrace.lock);
                    return _EFAULT;
                }
            }
            unlock(&child->ptrace.lock);

            return 0;
        }

        case PTRACE_SETREGS_: {
            STRACE("ptrace(PTRACE_SETREGS, %d, %#llx, %#llx)", pid,
                    (unsigned long long) addr, (unsigned long long) data);
            struct task *child = find_child(pid);
            if (!child) return _EPERM;

            if (child->abi == GUEST_ABI_AMD64) {
                struct user_regs_struct_amd64_ user_regs_amd64;
                if (user_get(data, user_regs_amd64)) {
                    unlock(&child->ptrace.lock);
                    return _EFAULT;
                }
                set_user_regs_amd64(&child->cpu, &user_regs_amd64);
            } else {
                struct user_regs_struct_ user_regs_;
                if (user_get(data, user_regs_)) {
                    unlock(&child->ptrace.lock);
                    return _EFAULT;
                }
                set_user_regs(&child->cpu, &user_regs_);
            }
            unlock(&child->ptrace.lock);

            return 0;
        }

        // GDB needs the fpregs functions to exist if you want to evaluate things
        case PTRACE_GETFPREGS_: {
            STRACE("ptrace(PTRACE_GETFPREGS, %d, %#llx, %#llx)", pid,
                    (unsigned long long) addr, (unsigned long long) data);
            struct task *child = find_child(pid);
            if (!child) return _EPERM;

            struct user_fpregs_struct_ user_fpregs_ = {};
            if (user_put(data, user_fpregs_)) {
                unlock(&child->ptrace.lock);
                return _EFAULT;
            }
            // TODO get float point registers
            unlock(&child->ptrace.lock);

            return 0;
        }

        case PTRACE_SETFPREGS_: {
            STRACE("ptrace(PTRACE_SETFPREGS, %d, %#llx, %#llx)", pid,
                    (unsigned long long) addr, (unsigned long long) data);
            struct task *child = find_child(pid);
            if (!child) return _EPERM;

            struct user_fpregs_struct_ user_fpregs_;
            if (user_get(data, user_fpregs_)) {
                return _EFAULT;
            } else {
                // TODO set floating point registers
            }
            unlock(&child->ptrace.lock);

            return 0;
        }

        case PTRACE_SYSCALL_: {
            STRACE("ptrace(PTRACE_SYSCALL, %d, %#llx, %#llx)", pid,
                    (unsigned long long) addr, (unsigned long long) data);
            struct task *child = find_child(pid);
            if (!child) return _EPERM;

            child->cpu.tf = false;
            child->ptrace.stopped = false;
            child->ptrace.stop_at_syscall = true;
            notify(&child->ptrace.cond);
            unlock(&child->ptrace.lock);

            return 0;
        }

        case PTRACE_SETOPTIONS_: {
            STRACE("ptrace(PTRACE_SETOPTIONS, %d, %#llx, %#llx)", pid,
                    (unsigned long long) addr, (unsigned long long) data);
            struct task *child = find_child(pid);
            if (!child) return _EPERM;
            // Ideally we would have this condition, but strace annonyingly
            // uses PTRACE_O_SYSGOOD | PTRACE_O_TRACEEXEC | PTRACE_O_TRACEEXIT
            // (we don't support the other two). Since this isn't a big deal we
            // will just pretend like we do support it to make that check pass.
            // if (data == PTRACE_O_TRACESYSGOOD_ || !data) {
            if (true) {
                child->ptrace.sysgood = !!(data & PTRACE_O_TRACESYSGOOD_);
                child->ptrace.options = data;
                unlock(&child->ptrace.lock);
                return 0;
            } else {
                unlock(&child->ptrace.lock);
                return _EINVAL;
            }
        }

        case PTRACE_GETSIGINFO_: {
            STRACE("ptrace(PTRACE_GETSIGINFO, %d, %#llx, %#llx)", pid,
                    (unsigned long long) addr, (unsigned long long) data);
            struct task *child = find_child(pid);
            if (!child) return _EPERM;

            if (data && siginfo_to_user(current, data, &child->ptrace.info)) {
                return _EFAULT;
            }
            unlock(&child->ptrace.lock);

            return 0;
        }

        case PTRACE_GETEVENTMSG_: {
            STRACE("ptrace(PTRACE_GETEVENTMSG, %d, %#llx, %#llx)", pid,
                    (unsigned long long) addr, (unsigned long long) data);
            struct task *child = find_child(pid);
            if (!child) return _EPERM;

            dword_t eventmsg = child->ptrace.eventmsg;
            if (data && user_put(data, eventmsg)) {
                unlock(&child->ptrace.lock);
                return _EFAULT;
            }
            unlock(&child->ptrace.lock);
            return 0;
        }

        default:
            STRACE("ptrace(%d, %d, %#llx, %#llx)", request, pid,
                    (unsigned long long) addr, (unsigned long long) data);
            return _EPERM;
    }
}
