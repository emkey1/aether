#ifndef KERNEL_PTRACE_H
#define KERNEL_PTRACE_H

#include "misc.h"

struct cpu_state;
struct siginfo_;
struct task;

#define PTRACE_TRACEME_ 0
#define PTRACE_PEEKTEXT_ 1
#define PTRACE_PEEKDATA_ 2
#define PTRACE_PEEKUSER_ 3
#define PTRACE_POKETEXT_ 4
#define PTRACE_POKEDATA_ 5
#define PTRACE_CONT_ 7
#define PTRACE_KILL_ 8
#define PTRACE_SINGLESTEP_ 9
#define PTRACE_GETREGS_ 12
#define PTRACE_SETREGS_ 13
#define PTRACE_GETFPREGS_ 14
#define PTRACE_SETFPREGS_ 15
#define PTRACE_SYSCALL_ 24
#define PTRACE_SETOPTIONS_ 0x4200
#define PTRACE_GETEVENTMSG_ 0x4201
#define PTRACE_GETSIGINFO_ 0x4202

#define PTRACE_EVENT_FORK_ 1
#define PTRACE_EVENT_VFORK_ 2
#define PTRACE_EVENT_CLONE_ 3
#define PTRACE_EVENT_EXEC_ 4
#define PTRACE_EVENT_EXIT_ 6

#define PTRACE_O_TRACESYSGOOD_ 1
#define PTRACE_O_TRACEFORK_ 2
#define PTRACE_O_TRACEVFORK_ 4
#define PTRACE_O_TRACECLONE_ 8
#define PTRACE_O_TRACEEXEC_ 0x10
#define PTRACE_O_TRACEEXIT_ 0x40

struct user_regs_struct_ {
    dword_t ebx;
    dword_t ecx;
    dword_t edx;
    dword_t esi;
    dword_t edi;
    dword_t ebp;
    dword_t eax;
    dword_t xds;
    dword_t xes;
    dword_t xfs;
    dword_t xgs;
    dword_t orig_eax;
    dword_t eip;
    dword_t xcs;
    dword_t eflags;
    dword_t esp;
    dword_t xss;
};

struct user_fpregs_struct_ {
    dword_t cwd;
    dword_t swd;
    dword_t twd;
    dword_t fip;
    dword_t fcs;
    dword_t foo;
    dword_t fos;
    dword_t st_space[20];
};

struct user_ {
    struct user_regs_struct_ user_regs;
    char padding[286 - sizeof(struct user_regs_struct_)];
};

dword_t sys_ptrace(dword_t request, dword_t pid, addr_t addr, dword_t data);
void ptrace_signal_stop(int sig, struct siginfo_ *info);
void ptrace_syscall_stop(struct cpu_state *cpu);
void ptrace_event_stop(int sig, struct siginfo_ *info, int event, dword_t eventmsg);
void ptrace_attach_fork_child(struct task *child, struct task *tracee);

#endif /* KERNEL_PTRACE_H */
