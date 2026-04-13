#include <string.h>
#include "debug.h"
#include "kernel/calls.h"
#include "emu/interrupt.h"
#include "emu/memory.h"
#include "kernel/signal.h"
#include "kernel/task.h"
#include "util/sync.h"

extern bool isGlibC;

dword_t syscall_stub(void) {
    STRACE("syscall_stub()");
    return _ENOSYS;
}
dword_t syscall_stub_silent(void) {
    STRACE("syscall_stub_silent()");
    return _ENOSYS;
}
dword_t syscall_success_stub(void) {
    STRACE("syscall_stub_success()");
    return 0;
}
dword_t syscall_eopnotsupp_stub(void) {
    STRACE("syscall_stub_eopnotsupp()");
    return _EOPNOTSUPP;
}

static dword_t sys_pread_amd64(fd_t f, addr_t buf_addr, dword_t size,
        dword_t off_low, dword_t off_high, dword_t UNUSED(unused)) {
    off_t_ off = ((off_t_) off_high << 32) | off_low;
    return sys_pread(f, buf_addr, size, off);
}

static dword_t sys_pwrite_amd64(fd_t f, addr_t buf_addr, dword_t size,
        dword_t off_low, dword_t off_high, dword_t UNUSED(unused)) {
    off_t_ off = ((off_t_) off_high << 32) | off_low;
    return sys_pwrite(f, buf_addr, size, off);
}

static dword_t sys_truncate_amd64(addr_t path_addr, dword_t size_low, dword_t size_high,
        dword_t UNUSED(unused3), dword_t UNUSED(unused4), dword_t UNUSED(unused5)) {
    return sys_truncate64(path_addr, size_low, size_high);
}

static dword_t sys_ftruncate_amd64(fd_t f, dword_t size_low, dword_t size_high,
        dword_t UNUSED(unused3), dword_t UNUSED(unused4), dword_t UNUSED(unused5)) {
    return sys_ftruncate64(f, size_low, size_high);
}

static dword_t sys_fallocate_amd64(fd_t f, dword_t mode, dword_t offset_low, dword_t offset_high,
        dword_t len_low, dword_t len_high) {
    return sys_fallocate(f, mode, offset_low, offset_high, len_low, len_high);
}

#if is_gcc(8)
#pragma GCC diagnostic ignored "-Wcast-function-type"
#endif
static syscall_t i386_syscall_table[] = {
    [1]   = (syscall_t) sys_exit,
    [2]   = (syscall_t) sys_fork,
    [3]   = (syscall_t) sys_read,
    [4]   = (syscall_t) sys_write,
    [5]   = (syscall_t) sys_open,
    [6]   = (syscall_t) sys_close,
    [7]   = (syscall_t) sys_waitpid,
    [8]   = (syscall_t) sys_creat, // creat
    [9]   = (syscall_t) sys_link,
    [10]  = (syscall_t) sys_unlink,
    [11]  = (syscall_t) sys_execve,
    [12]  = (syscall_t) sys_chdir,
    [13]  = (syscall_t) sys_time,
    [14]  = (syscall_t) sys_mknod,
    [15]  = (syscall_t) sys_chmod,
    [19]  = (syscall_t) sys_lseek,
    [20]  = (syscall_t) sys_getpid,
    [21]  = (syscall_t) sys_mount,
    [23]  = (syscall_t) sys_setuid,
    [24]  = (syscall_t) sys_getuid,
    [25]  = (syscall_t) sys_stime,
    [26]  = (syscall_t) sys_ptrace,
    [27]  = (syscall_t) sys_alarm,
    [29]  = (syscall_t) sys_pause,
    [30]  = (syscall_t) sys_utime,
    [33]  = (syscall_t) sys_access,
    [36]  = (syscall_t) syscall_success_stub, // sync
    [37]  = (syscall_t) sys_kill,
    [38]  = (syscall_t) sys_rename,
    [39]  = (syscall_t) sys_mkdir,
    [40]  = (syscall_t) sys_rmdir,
    [41]  = (syscall_t) sys_dup,
    [42]  = (syscall_t) sys_pipe,
    [43]  = (syscall_t) sys_times,
    [45]  = (syscall_t) sys_brk,
    [46]  = (syscall_t) sys_setgid,
    [47]  = (syscall_t) sys_getgid,
    [49]  = (syscall_t) sys_geteuid,
    [50]  = (syscall_t) sys_getegid,
    [52]  = (syscall_t) sys_umount2,
    [54]  = (syscall_t) sys_ioctl,
    [55]  = (syscall_t) sys_fcntl32,
    [57]  = (syscall_t) sys_setpgid,
    [60]  = (syscall_t) sys_umask,
    [61]  = (syscall_t) sys_chroot,
    [63]  = (syscall_t) sys_dup2,
    [64]  = (syscall_t) sys_getppid,
    [65]  = (syscall_t) sys_getpgrp,
    [66]  = (syscall_t) sys_setsid,
    [67]  = (syscall_t) syscall_stub, // sigaction
    [74]  = (syscall_t) sys_sethostname,
    [75]  = (syscall_t) sys_setrlimit32,
    [76]  = (syscall_t) sys_old_getrlimit32,
    [77]  = (syscall_t) sys_getrusage,
    [78]  = (syscall_t) sys_gettimeofday,
    [79]  = (syscall_t) sys_settimeofday,
    [80]  = (syscall_t) sys_getgroups,
    [81]  = (syscall_t) sys_setgroups,
    [83]  = (syscall_t) sys_symlink,
    [85]  = (syscall_t) sys_readlink,
    [88]  = (syscall_t) sys_reboot,
    [90]  = (syscall_t) sys_mmap,
    [91]  = (syscall_t) sys_munmap,
    [93]  = (syscall_t) sys_ftruncate,
    [94]  = (syscall_t) sys_fchmod,
    [96]  = (syscall_t) sys_getpriority,
    [97]  = (syscall_t) sys_setpriority,
    [99]  = (syscall_t) sys_statfs,
    [100] = (syscall_t) sys_fstatfs,
    [102] = (syscall_t) sys_socketcall,
    [103] = (syscall_t) sys_syslog,
    [104] = (syscall_t) sys_setitimer,
    [106] = (syscall_t) sys_stat,
    [107] = (syscall_t) sys_lstat,
    [108] = (syscall_t) sys_fstat,
    [114] = (syscall_t) sys_wait4,
    [116] = (syscall_t) sys_sysinfo,
    [117] = (syscall_t) sys_ipc,
    [118] = (syscall_t) sys_fsync,
    [119] = (syscall_t) sys_sigreturn,
    [120] = (syscall_t) sys_clone,
    [122] = (syscall_t) sys_uname,
    [125] = (syscall_t) sys_mprotect,
    [132] = (syscall_t) sys_getpgid,
    [133] = (syscall_t) sys_fchdir,
    [136] = (syscall_t) sys_personality,
    [140] = (syscall_t) sys__llseek,
    [141] = (syscall_t) sys_getdents,
    [142] = (syscall_t) sys_select,
    [143] = (syscall_t) sys_flock,
    [144] = (syscall_t) sys_msync,
    [145] = (syscall_t) sys_readv,
    [146] = (syscall_t) sys_writev,
    [147] = (syscall_t) sys_getsid,
    [148] = (syscall_t) sys_fsync, // fdatasync
    [150] = (syscall_t) sys_mlock,
    [151] = (syscall_t) sys_munlock,
    [152] = (syscall_t) syscall_stub, // mlockall
    [155] = (syscall_t) sys_sched_getparam,
    [156] = (syscall_t) sys_sched_setscheduler,
    [157] = (syscall_t) sys_sched_getscheduler,
    [158] = (syscall_t) sys_sched_yield,
    [159] = (syscall_t) sys_sched_get_priority_max,
    [160] = (syscall_t) sys_sched_get_priority_min,
    [162] = (syscall_t) sys_nanosleep,
    [163] = (syscall_t) sys_mremap,
    [168] = (syscall_t) sys_poll,
    [172] = (syscall_t) sys_prctl,
    [173] = (syscall_t) sys_rt_sigreturn,
    [174] = (syscall_t) sys_rt_sigaction,
    [175] = (syscall_t) sys_rt_sigprocmask,
    [176] = (syscall_t) sys_rt_sigpending,
    [177] = (syscall_t) sys_rt_sigtimedwait,
    [179] = (syscall_t) sys_rt_sigsuspend,
    [180] = (syscall_t) sys_pread,
    [181] = (syscall_t) sys_pwrite,
    [183] = (syscall_t) sys_getcwd,
    [184] = (syscall_t) sys_capget,
    [185] = (syscall_t) sys_capset,
    [186] = (syscall_t) sys_sigaltstack,
    [187] = (syscall_t) sys_sendfile,
    [190] = (syscall_t) sys_vfork,
    [191] = (syscall_t) sys_getrlimit32,
    [192] = (syscall_t) sys_mmap2,
    [193] = (syscall_t) sys_truncate64,
    [194] = (syscall_t) sys_ftruncate64,
    [195] = (syscall_t) sys_stat64,
    [196] = (syscall_t) sys_lstat64,
    [197] = (syscall_t) sys_fstat64,
    [198] = (syscall_t) sys_lchown,
    [199] = (syscall_t) sys_getuid32,
    [200] = (syscall_t) sys_getgid32,
    [201] = (syscall_t) sys_geteuid32,
    [202] = (syscall_t) sys_getegid32,
    [203] = (syscall_t) sys_setreuid,
    [204] = (syscall_t) sys_setregid,
    [205] = (syscall_t) sys_getgroups,
    [206] = (syscall_t) sys_setgroups,
    [207] = (syscall_t) sys_fchown32,
    [208] = (syscall_t) sys_setresuid,
    [209] = (syscall_t) sys_getresuid,
    [210] = (syscall_t) sys_setresgid,
    [211] = (syscall_t) sys_getresgid,
    [212] = (syscall_t) sys_chown32,
    [213] = (syscall_t) sys_setuid,
    [214] = (syscall_t) sys_setgid,
    [215] = (syscall_t) sys_setfsuid,
    [216] = (syscall_t) sys_setfsgid,
    [219] = (syscall_t) sys_madvise,
    [220] = (syscall_t) sys_getdents64,
    [221] = (syscall_t) sys_fcntl,
    [224] = (syscall_t) sys_gettid,
    [225] = (syscall_t) syscall_success_stub, // readahead
    [226 ... 237] = (syscall_t) sys_xattr_stub,
    [238] = (syscall_t) sys_tkill,
    [239] = (syscall_t) sys_sendfile64,
    [240] = (syscall_t) sys_futex,
    [241] = (syscall_t) sys_sched_setaffinity,
    [242] = (syscall_t) sys_sched_getaffinity,
    [243] = (syscall_t) sys_set_thread_area,
    [245] = (syscall_t) syscall_stub, // io_setup
    [252] = (syscall_t) sys_exit_group,
    [254] = (syscall_t) sys_epoll_create0,
    [255] = (syscall_t) sys_epoll_ctl,
    [256] = (syscall_t) sys_epoll_wait,
    [258] = (syscall_t) sys_set_tid_address,
    [259] = (syscall_t) sys_timer_create,
    [260] = (syscall_t) sys_timer_settime,
    [261] = (syscall_t) sys_timer_gettime,
    [262] = (syscall_t) sys_timer_getoverrun,
    [263] = (syscall_t) sys_timer_delete,
    [264] = (syscall_t) sys_clock_settime,
    [265] = (syscall_t) sys_clock_gettime,
    [266] = (syscall_t) sys_clock_getres,
    [267] = (syscall_t) sys_clock_nanosleep,
    [268] = (syscall_t) sys_statfs64,
    [269] = (syscall_t) sys_fstatfs64,
    [270] = (syscall_t) sys_tgkill,
    [271] = (syscall_t) sys_utimes,
    [272] = (syscall_t) syscall_success_stub,
    [274] = (syscall_t) sys_mbind,
    [275] = (syscall_t) sys_get_mempolicy,
    [276] = (syscall_t) sys_set_mempolicy,
    [284] = (syscall_t) sys_waitid,
    [288] = (syscall_t) sys_keyctl,
    [289] = (syscall_t) sys_ioprio_set,
    [290] = (syscall_t) sys_ioprio_get,
    [291] = (syscall_t) sys_inotify_init,
    [292] = (syscall_t) sys_inotify_add_watch,
    [293] = (syscall_t) sys_inotify_rm_watch,
    [295] = (syscall_t) sys_openat,
    [296] = (syscall_t) sys_mkdirat,
    [297] = (syscall_t) sys_mknodat,
    [298] = (syscall_t) sys_fchownat,
    [300] = (syscall_t) sys_fstatat64,
    [301] = (syscall_t) sys_unlinkat,
    [302] = (syscall_t) sys_renameat,
    [303] = (syscall_t) sys_linkat,
    [304] = (syscall_t) sys_symlinkat,
    [305] = (syscall_t) sys_readlinkat,
    [306] = (syscall_t) sys_fchmodat,
    [307] = (syscall_t) sys_faccessat,
    [308] = (syscall_t) sys_pselect,
    [309] = (syscall_t) sys_ppoll,
    [310] = (syscall_t) sys_unshare,
    [311] = (syscall_t) sys_set_robust_list,
    [312] = (syscall_t) sys_get_robust_list,
    [313] = (syscall_t) sys_splice,
    [314] = (syscall_t) syscall_stub_silent, // sync_file_range
    [318] = (syscall_t) syscall_success_stub, // getcpu
    [319] = (syscall_t) sys_epoll_pwait,
    [320] = (syscall_t) sys_utimensat,
    [321] = (syscall_t) sys_signalfd,
    [322] = (syscall_t) sys_timerfd_create,
    [323] = (syscall_t) sys_eventfd,
    [324] = (syscall_t) sys_fallocate,
    [325] = (syscall_t) sys_timerfd_settime,
    [326] = (syscall_t) sys_timerfd_gettime,
    [327] = (syscall_t) sys_signalfd4,
    [328] = (syscall_t) sys_eventfd2,
    [329] = (syscall_t) sys_epoll_create,
    [330] = (syscall_t) sys_dup3,
    [331] = (syscall_t) sys_pipe2,
    [332] = (syscall_t) sys_inotify_init1,
    [336] = (syscall_t) syscall_stub, // perf_event_open
    [337] = (syscall_t) sys_recvmmsg,
    [340] = (syscall_t) sys_prlimit64,
    [341] = (syscall_t) syscall_eopnotsupp_stub, // name_to_handle_at
    [342] = (syscall_t) syscall_eopnotsupp_stub, // open_by_handle_at
    [345] = (syscall_t) sys_sendmmsg,
    [347] = (syscall_t) sys_process_vm_readv,
    [352] = (syscall_t) syscall_stub, // sched_getattr
    [353] = (syscall_t) sys_renameat2,
    [354] = (syscall_t) syscall_stub, //seccomp
    [355] = (syscall_t) sys_getrandom,
    [356] = (syscall_t) sys_memfd_create,
    [358] = (syscall_t) sys_execveat,
    [359] = (syscall_t) sys_socket,
    [360] = (syscall_t) sys_socketpair,
    [361] = (syscall_t) sys_bind,
    [362] = (syscall_t) sys_connect,
    [363] = (syscall_t) sys_listen,
    [364] = (syscall_t) sys_accept4,
    [365] = (syscall_t) sys_getsockopt,
    [366] = (syscall_t) sys_setsockopt,
    [367] = (syscall_t) sys_getsockname,
    [368] = (syscall_t) sys_getpeername,
    [369] = (syscall_t) sys_sendto,
    [370] = (syscall_t) sys_sendmsg,
    [371] = (syscall_t) sys_recvfrom,
    [372] = (syscall_t) sys_recvmsg,
    [373] = (syscall_t) sys_shutdown,
    [375] = (syscall_t) sys_membarrier, // membarrier
    [377] = (syscall_t) sys_copy_file_range,
    [383] = (syscall_t) sys_statx,
    [384] = (syscall_t) sys_arch_prctl,
    [386] = (syscall_t) sys_rseq,
    [395] = (syscall_t) sys_shmget,
    [396] = (syscall_t) sys_shmctl,
    [397] = (syscall_t) sys_shmat,
    [398] = (syscall_t) sys_shmdt,
    [403] = (syscall_t) sys_clock_gettime64, // clock_gettime64
    [404] = (syscall_t) sys_clock_settime64, // clock_settime64
    [406] = (syscall_t) sys_clock_getres_time64, // clock_getres_time64
    [407] = (syscall_t) sys_clock_nanosleep_time64, // clock_nanosleep_time64
    [408] = (syscall_t) sys_timer_gettime64, // timer_gettime64
    [409] = (syscall_t) sys_timer_settime64, // timer_settime64
    [410] = (syscall_t) sys_timerfd_gettime64, // timerfd_gettime64
    [411] = (syscall_t) sys_timerfd_settime64, // timerfd_settime64
    [412] = (syscall_t) sys_utimensat64, // utimensat_time64
    [413] = (syscall_t) sys_pselect_time64, // pselect6_time64
    [414] = (syscall_t) sys_ppoll_time64,
    [417] = (syscall_t) sys_recvmmsg_time64, // recvmmsg_time64
    [421] = (syscall_t) sys_rt_sigtimedwait_time64, // rt_sigtimedwait_time64
    [422] = (syscall_t) sys_futex_time64, // futex_time64
    [424] = (syscall_t) syscall_stub, // pidfd_send_signal?
    [435] = (syscall_t) sys_clone3, // clone3
    [436] = (syscall_t) sys_close_range,
    [437] = (syscall_t) sys_openat2,
    [439] = (syscall_t) sys_faccessat, // faccessat2
    [441] = (syscall_t) sys_epoll_pwait2, // epoll_pwait2
    [452] = (syscall_t) sys_fchmodat2,
};
/*
SYS_MSGRCV                       = 401
SYS_MSGCTL                       = 402
SYS_CLOCK_GETTIME64              = 403
SYS_CLOCK_SETTIME64              = 404
SYS_CLOCK_ADJTIME64              = 405
SYS_CLOCK_GETRES_TIME64          = 406
SYS_CLOCK_NANOSLEEP_TIME64       = 407
SYS_TIMER_GETTIME64              = 408
SYS_TIMER_SETTIME64              = 409
SYS_TIMERFD_GETTIME64            = 410
SYS_TIMERFD_SETTIME64            = 411
SYS_UTIMENSAT_TIME64             = 412
SYS_PSELECT6_TIME64              = 413
SYS_PPOLL_TIME64                 = 414
SYS_IO_PGETEVENTS_TIME64         = 416
SYS_RECVMMSG_TIME64              = 417
SYS_MQ_TIMEDSEND_TIME64          = 418
SYS_MQ_TIMEDRECEIVE_TIME64       = 419
SYS_SEMTIMEDOP_TIME64            = 420
SYS_RT_SIGTIMEDWAIT_TIME64       = 421
SYS_FUTEX_TIME64                 = 422
SYS_SCHED_RR_GET_INTERVAL_TIME64 = 423
SYS_PIDFD_SEND_SIGNAL            = 424
SYS_IO_URING_SETUP               = 425
SYS_IO_URING_ENTER               = 426
SYS_IO_URING_REGISTER            = 427
SYS_OPEN_TREE                    = 428
SYS_MOVE_MOUNT                   = 429
SYS_FSOPEN                       = 430
SYS_FSCONFIG                     = 431
SYS_FSMOUNT                      = 432
SYS_FSPICK                       = 433
SYS_PIDFD_OPEN                   = 434
SYS_CLONE3                       = 435
SYS_CLOSE_RANGE                  = 436
SYS_OPENAT2                      = 437
SYS_PIDFD_GETFD                  = 438
SYS_FACCESSAT2                   = 439
SYS_PROCESS_MADVISE              = 440
SYS_EPOLL_PWAIT2                 = 441
 */

static inline qword_t i386_syscall_number(const struct cpu_state *cpu);
static inline void i386_syscall_args(const struct cpu_state *cpu, qword_t args[6]);
static inline void i386_syscall_result(struct cpu_state *cpu, dword_t result);
static inline qword_t amd64_syscall_number(const struct cpu_state *cpu);
static inline void amd64_syscall_args(const struct cpu_state *cpu, qword_t args[6]);
static inline void amd64_syscall_result(struct cpu_state *cpu, dword_t result);
void handle_syscall_interrupt(struct cpu_state *cpu);

struct syscall_abi_dispatch {
    enum guest_abi abi;
    const char *name;
    const syscall_t *table;
    size_t num_syscalls;
    qword_t (*syscall_number)(const struct cpu_state *cpu);
    void (*syscall_args)(const struct cpu_state *cpu, qword_t args[6]);
    void (*syscall_result)(struct cpu_state *cpu, dword_t result);
};

static syscall_t amd64_syscall_table[453] = {
    // This table covers amd64 syscalls that either reuse an existing i386
    // implementation safely or have a small amd64 shim for 64-bit arg packing.
    [0] = (syscall_t) sys_read,
    [1] = (syscall_t) sys_write,
    [2] = (syscall_t) sys_open,
    [3] = (syscall_t) sys_close,
    [4] = (syscall_t) sys_stat_amd64,
    [5] = (syscall_t) sys_fstat_amd64,
    [6] = (syscall_t) sys_lstat_amd64,
    [7] = (syscall_t) sys_poll,
    [8] = (syscall_t) sys_lseek,
    [9] = (syscall_t) sys_mmap_amd64,
    [10] = (syscall_t) sys_mprotect,
    [11] = (syscall_t) sys_munmap,
    [12] = (syscall_t) sys_brk,
    [13] = (syscall_t) sys_rt_sigaction,
    [14] = (syscall_t) sys_rt_sigprocmask,
    [15] = (syscall_t) sys_rt_sigreturn,
    [17] = (syscall_t) sys_pread_amd64,
    [18] = (syscall_t) sys_pwrite_amd64,
    [16] = (syscall_t) sys_ioctl,
    [19] = (syscall_t) sys_readv,
    [20] = (syscall_t) sys_writev,
    [21] = (syscall_t) sys_access,
    [22] = (syscall_t) sys_pipe,
    [23] = (syscall_t) sys_select,
    [24] = (syscall_t) sys_sched_yield,
    [25] = (syscall_t) sys_mremap,
    [26] = (syscall_t) sys_msync,
    [28] = (syscall_t) sys_madvise,
    [29] = (syscall_t) sys_shmget,
    [30] = (syscall_t) sys_shmat,
    [31] = (syscall_t) sys_shmctl,
    [32] = (syscall_t) sys_dup,
    [33] = (syscall_t) sys_dup2,
    [34] = (syscall_t) sys_pause,
    [35] = (syscall_t) sys_nanosleep,
    [37] = (syscall_t) sys_alarm,
    [38] = (syscall_t) sys_setitimer,
    [39] = (syscall_t) sys_getpid,
    [40] = (syscall_t) sys_sendfile,
    [41] = (syscall_t) sys_socket,
    [42] = (syscall_t) sys_connect,
    [43] = (syscall_t) sys_accept,
    [44] = (syscall_t) sys_sendto,
    [45] = (syscall_t) sys_recvfrom,
    [46] = (syscall_t) sys_sendmsg,
    [47] = (syscall_t) sys_recvmsg,
    [48] = (syscall_t) sys_shutdown,
    [49] = (syscall_t) sys_bind,
    [50] = (syscall_t) sys_listen,
    [51] = (syscall_t) sys_getsockname,
    [52] = (syscall_t) sys_getpeername,
    [53] = (syscall_t) sys_socketpair,
    [54] = (syscall_t) sys_setsockopt,
    [55] = (syscall_t) sys_getsockopt,
    [56] = (syscall_t) sys_clone,
    [57] = (syscall_t) sys_fork,
    [58] = (syscall_t) sys_vfork,
    [59] = (syscall_t) sys_execve,
    [60] = (syscall_t) sys_exit,
    [61] = (syscall_t) sys_wait4,
    [62] = (syscall_t) sys_kill,
    [63] = (syscall_t) sys_uname,
    [67] = (syscall_t) sys_shmdt,
    [72] = (syscall_t) sys_fcntl,
    [73] = (syscall_t) sys_flock,
    [74] = (syscall_t) sys_fsync,
    [75] = (syscall_t) sys_fsync, // fdatasync
    [76] = (syscall_t) sys_truncate_amd64,
    [77] = (syscall_t) sys_ftruncate_amd64,
    [78] = (syscall_t) sys_getdents,
    [79] = (syscall_t) sys_getcwd,
    [80] = (syscall_t) sys_chdir,
    [81] = (syscall_t) sys_fchdir,
    [82] = (syscall_t) sys_rename,
    [83] = (syscall_t) sys_mkdir,
    [84] = (syscall_t) sys_rmdir,
    [85] = (syscall_t) sys_creat,
    [86] = (syscall_t) sys_link,
    [87] = (syscall_t) sys_unlink,
    [88] = (syscall_t) sys_symlink,
    [89] = (syscall_t) sys_readlink,
    [90] = (syscall_t) sys_chmod,
    [91] = (syscall_t) sys_fchmod,
    [92] = (syscall_t) sys_chown32,
    [93] = (syscall_t) sys_fchown32,
    [94] = (syscall_t) sys_lchown,
    [95] = (syscall_t) sys_umask,
    [96] = (syscall_t) sys_gettimeofday,
    [97] = (syscall_t) sys_getrlimit64,
    [98] = (syscall_t) sys_getrusage,
    [99] = (syscall_t) sys_sysinfo,
    [100] = (syscall_t) sys_times,
    [101] = (syscall_t) sys_ptrace,
    [102] = (syscall_t) sys_getuid,
    [103] = (syscall_t) sys_syslog,
    [104] = (syscall_t) sys_getgid,
    [105] = (syscall_t) sys_setuid,
    [106] = (syscall_t) sys_setgid,
    [107] = (syscall_t) sys_geteuid,
    [108] = (syscall_t) sys_getegid,
    [109] = (syscall_t) sys_setpgid,
    [110] = (syscall_t) sys_getppid,
    [111] = (syscall_t) sys_getpgrp,
    [112] = (syscall_t) sys_setsid,
    [113] = (syscall_t) sys_setreuid,
    [114] = (syscall_t) sys_setregid,
    [115] = (syscall_t) sys_getgroups,
    [116] = (syscall_t) sys_setgroups,
    [117] = (syscall_t) sys_setresuid,
    [118] = (syscall_t) sys_getresuid,
    [119] = (syscall_t) sys_setresgid,
    [120] = (syscall_t) sys_getresgid,
    [121] = (syscall_t) sys_getpgid,
    [122] = (syscall_t) sys_setfsuid,
    [123] = (syscall_t) sys_setfsgid,
    [124] = (syscall_t) sys_getsid,
    [125] = (syscall_t) sys_capget,
    [126] = (syscall_t) sys_capset,
    [127] = (syscall_t) sys_rt_sigpending,
    [128] = (syscall_t) sys_rt_sigtimedwait,
    [130] = (syscall_t) sys_rt_sigsuspend,
    [131] = (syscall_t) sys_sigaltstack,
    [132] = (syscall_t) sys_utime,
    [133] = (syscall_t) sys_mknod,
    [135] = (syscall_t) sys_personality,
    [137] = (syscall_t) sys_statfs_amd64,
    [138] = (syscall_t) sys_fstatfs_amd64,
    [140] = (syscall_t) sys_getpriority,
    [141] = (syscall_t) sys_setpriority,
    [143] = (syscall_t) sys_sched_getparam,
    [144] = (syscall_t) sys_sched_setscheduler,
    [145] = (syscall_t) sys_sched_getscheduler,
    [146] = (syscall_t) sys_sched_get_priority_max,
    [147] = (syscall_t) sys_sched_get_priority_min,
    [149] = (syscall_t) sys_mlock,
    [150] = (syscall_t) sys_munlock,
    [157] = (syscall_t) sys_prctl,
    [158] = (syscall_t) sys_arch_prctl,
    [160] = (syscall_t) sys_setrlimit64,
    [161] = (syscall_t) sys_chroot,
    [162] = (syscall_t) syscall_success_stub, // sync
    [164] = (syscall_t) sys_settimeofday,
    [165] = (syscall_t) sys_mount,
    [166] = (syscall_t) sys_umount2,
    [169] = (syscall_t) sys_reboot,
    [170] = (syscall_t) sys_sethostname,
    [186] = (syscall_t) sys_gettid,
    [187] = (syscall_t) syscall_success_stub, // readahead
    [188 ... 199] = (syscall_t) sys_xattr_stub,
    [200] = (syscall_t) sys_tkill,
    [201] = (syscall_t) sys_time,
    [202] = (syscall_t) sys_futex,
    [203] = (syscall_t) sys_sched_setaffinity,
    [204] = (syscall_t) sys_sched_getaffinity,
    [213] = (syscall_t) sys_epoll_create0,
    [217] = (syscall_t) sys_getdents64,
    [218] = (syscall_t) sys_set_tid_address,
    [222] = (syscall_t) sys_timer_create,
    [223] = (syscall_t) sys_timer_settime,
    [224] = (syscall_t) sys_timer_gettime,
    [225] = (syscall_t) sys_timer_getoverrun,
    [226] = (syscall_t) sys_timer_delete,
    [227] = (syscall_t) sys_clock_settime,
    [228] = (syscall_t) sys_clock_gettime,
    [229] = (syscall_t) sys_clock_getres,
    [230] = (syscall_t) sys_clock_nanosleep,
    [231] = (syscall_t) sys_exit_group,
    [232] = (syscall_t) sys_epoll_wait,
    [233] = (syscall_t) sys_epoll_ctl,
    [234] = (syscall_t) sys_tgkill,
    [235] = (syscall_t) sys_utimes,
    [237] = (syscall_t) sys_mbind,
    [238] = (syscall_t) sys_set_mempolicy,
    [239] = (syscall_t) sys_get_mempolicy,
    [247] = (syscall_t) sys_waitid,
    [250] = (syscall_t) sys_keyctl,
    [251] = (syscall_t) sys_ioprio_set,
    [252] = (syscall_t) sys_ioprio_get,
    [253] = (syscall_t) sys_inotify_init,
    [254] = (syscall_t) sys_inotify_add_watch,
    [255] = (syscall_t) sys_inotify_rm_watch,
    [257] = (syscall_t) sys_openat,
    [258] = (syscall_t) sys_mkdirat,
    [259] = (syscall_t) sys_mknodat,
    [260] = (syscall_t) sys_fchownat,
    [261] = (syscall_t) sys_futimesat,
    [262] = (syscall_t) sys_newfstatat_amd64,
    [263] = (syscall_t) sys_unlinkat,
    [264] = (syscall_t) sys_renameat,
    [265] = (syscall_t) sys_linkat,
    [266] = (syscall_t) sys_symlinkat,
    [267] = (syscall_t) sys_readlinkat,
    [268] = (syscall_t) sys_fchmodat,
    [269] = (syscall_t) sys_faccessat,
    [270] = (syscall_t) sys_pselect,
    [271] = (syscall_t) sys_ppoll,
    [272] = (syscall_t) sys_unshare,
    [273] = (syscall_t) sys_set_robust_list,
    [274] = (syscall_t) sys_get_robust_list,
    [275] = (syscall_t) sys_splice,
    [277] = (syscall_t) syscall_stub_silent, // sync_file_range
    [280] = (syscall_t) sys_utimensat,
    [281] = (syscall_t) sys_epoll_pwait,
    [282] = (syscall_t) sys_signalfd,
    [283] = (syscall_t) sys_timerfd_create,
    [284] = (syscall_t) sys_eventfd,
    [285] = (syscall_t) sys_fallocate_amd64,
    [286] = (syscall_t) sys_timerfd_settime,
    [287] = (syscall_t) sys_timerfd_gettime,
    [288] = (syscall_t) sys_accept4,
    [289] = (syscall_t) sys_signalfd4,
    [290] = (syscall_t) sys_eventfd2,
    [291] = (syscall_t) sys_epoll_create,
    [292] = (syscall_t) sys_dup3,
    [293] = (syscall_t) sys_pipe2,
    [294] = (syscall_t) sys_inotify_init1,
    [299] = (syscall_t) sys_recvmmsg,
    [302] = (syscall_t) sys_prlimit64,
    [307] = (syscall_t) sys_sendmmsg,
    [309] = (syscall_t) syscall_success_stub, // getcpu
    [310] = (syscall_t) sys_process_vm_readv,
    [316] = (syscall_t) sys_renameat2,
    [318] = (syscall_t) sys_getrandom,
    [319] = (syscall_t) sys_memfd_create,
    [322] = (syscall_t) sys_execveat,
    [324] = (syscall_t) sys_membarrier,
    [326] = (syscall_t) sys_copy_file_range,
    [332] = (syscall_t) sys_statx,
    [334] = (syscall_t) sys_rseq,
    [403] = (syscall_t) sys_clock_gettime64,
    [404] = (syscall_t) sys_clock_settime64,
    [406] = (syscall_t) sys_clock_getres_time64,
    [407] = (syscall_t) sys_clock_nanosleep_time64,
    [408] = (syscall_t) sys_timer_gettime64,
    [409] = (syscall_t) sys_timer_settime64,
    [410] = (syscall_t) sys_timerfd_gettime64,
    [411] = (syscall_t) sys_timerfd_settime64,
    [412] = (syscall_t) sys_utimensat64,
    [413] = (syscall_t) sys_pselect_time64,
    [414] = (syscall_t) sys_ppoll_time64,
    [417] = (syscall_t) sys_recvmmsg_time64,
    [421] = (syscall_t) sys_rt_sigtimedwait_time64,
    [422] = (syscall_t) sys_futex_time64,
    [435] = (syscall_t) sys_clone3,
    [436] = (syscall_t) sys_close_range,
    [437] = (syscall_t) sys_openat2,
    [439] = (syscall_t) sys_faccessat,
    [441] = (syscall_t) sys_epoll_pwait2,
    [452] = (syscall_t) sys_fchmodat2,
};

static const struct syscall_abi_dispatch i386_syscall_dispatch = {
    .abi = GUEST_ABI_I386,
    .name = "i386",
    .table = i386_syscall_table,
    .num_syscalls = sizeof(i386_syscall_table) / sizeof(i386_syscall_table[0]),
    .syscall_number = i386_syscall_number,
    .syscall_args = i386_syscall_args,
    .syscall_result = i386_syscall_result,
};

static const struct syscall_abi_dispatch amd64_syscall_dispatch = {
    .abi = GUEST_ABI_AMD64,
    .name = "amd64",
    .table = amd64_syscall_table,
    .num_syscalls = sizeof(amd64_syscall_table) / sizeof(amd64_syscall_table[0]),
    .syscall_number = amd64_syscall_number,
    .syscall_args = amd64_syscall_args,
    .syscall_result = amd64_syscall_result,
};

static const struct syscall_abi_dispatch *syscall_dispatch_for_abi(enum guest_abi abi) {
    switch (abi) {
    case GUEST_ABI_AMD64:
        return &amd64_syscall_dispatch;
    case GUEST_ABI_I386:
    default:
        return &i386_syscall_dispatch;
    }
}

void dump_stack(int lines);

static bool syscall_is_logged_stub(syscall_t syscall) {
    return syscall == (syscall_t) syscall_stub ||
        syscall == (syscall_t) syscall_stub_silent;
}

static bool syscall_result_is_errno(dword_t result) {
    sdword_t signed_result = (sdword_t) result;
    return signed_result < 0 && signed_result >= -4095;
}

static sdword_t syscall_result_errno(dword_t result) {
    return (sdword_t) result;
}

static inline qword_t i386_syscall_number(const struct cpu_state *cpu) {
    return cpu->eax;
}

static inline void i386_syscall_args(const struct cpu_state *cpu, qword_t args[6]) {
    args[0] = cpu->ebx;
    args[1] = cpu->ecx;
    args[2] = cpu->edx;
    args[3] = cpu->esi;
    args[4] = cpu->edi;
    args[5] = cpu->ebp;
}

static inline void i386_syscall_result(struct cpu_state *cpu, dword_t result) {
    cpu->eax = result;
}

static inline qword_t amd64_syscall_number(const struct cpu_state *cpu) {
    return cpu->amd64_regs[amd64_rax];
}

static inline void amd64_syscall_args(const struct cpu_state *cpu, qword_t args[6]) {
    args[0] = cpu->amd64_regs[amd64_rdi];
    args[1] = cpu->amd64_regs[amd64_rsi];
    args[2] = cpu->amd64_regs[amd64_rdx];
    args[3] = cpu->amd64_regs[amd64_r10];
    args[4] = cpu->amd64_regs[amd64_r8];
    args[5] = cpu->amd64_regs[amd64_r9];
}

static inline void amd64_syscall_result(struct cpu_state *cpu, dword_t result) {
    if (syscall_result_is_errno(result))
        cpu->amd64_regs[amd64_rax] = (qword_t) (sqword_t) syscall_result_errno(result);
    else
        cpu->amd64_regs[amd64_rax] = (qword_t) result;
    cpu->eax = result;
    cpu->amd64_regs[amd64_rcx] = cpu->amd64_rip;
    cpu->amd64_regs[amd64_r11] = cpu->eflags;
}

static inline void amd64_syscall_result_qword(struct cpu_state *cpu, qword_t result) {
    sqword_t signed_result = (sqword_t) result;
    if (signed_result < 0 && signed_result >= -4095)
        cpu->amd64_regs[amd64_rax] = (qword_t) signed_result;
    else
        cpu->amd64_regs[amd64_rax] = result;
    cpu->eax = (dword_t) cpu->amd64_regs[amd64_rax];
    cpu->amd64_regs[amd64_rcx] = cpu->amd64_rip;
    cpu->amd64_regs[amd64_r11] = cpu->eflags;
}

static bool syscall_arg_fits_legacy_dword(qword_t arg) {
    return arg <= UINT32_MAX || (arg >> 32) == UINT32_MAX;
}

static bool handle_amd64_native_memory_syscall(struct cpu_state *cpu, qword_t syscall_num,
        const qword_t raw_args[6]) {
    switch (syscall_num) {
    case 9:
        amd64_syscall_result_qword(cpu, sys_mmap_guest(raw_args[0], (dword_t) raw_args[1],
                (dword_t) raw_args[2], (dword_t) raw_args[3], (fd_t) raw_args[4],
                (dword_t) raw_args[5]));
        return true;
    case 10:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_mprotect_guest(raw_args[0],
                (uint_t) raw_args[1], (int_t) raw_args[2]));
        return true;
    case 11:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_munmap_guest(raw_args[0],
                (uint_t) raw_args[1]));
        return true;
    case 12:
        amd64_syscall_result_qword(cpu, sys_brk_guest(raw_args[0]));
        return true;
    case 25:
        amd64_syscall_result_qword(cpu, sys_mremap_guest(raw_args[0], (dword_t) raw_args[1],
                (dword_t) raw_args[2], (dword_t) raw_args[3]));
        return true;
    case 26:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_msync_guest(raw_args[0],
                (dword_t) raw_args[1], (int_t) raw_args[2]));
        return true;
    case 28:
        amd64_syscall_result_qword(cpu, sys_madvise_guest(raw_args[0], (dword_t) raw_args[1],
                (dword_t) raw_args[2]));
        return true;
    case 149:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_mlock_guest(raw_args[0],
                (dword_t) raw_args[1]));
        return true;
    case 150:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_munlock_guest(raw_args[0],
                (dword_t) raw_args[1]));
        return true;
    case 158:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_arch_prctl_guest(
                (int_t) raw_args[0], raw_args[1]));
        return true;
    case 218:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_set_tid_address_guest(
                raw_args[0]));
        return true;
    default:
        return false;
    }
}

static unsigned amd64_syscall_legacy_arg_count(qword_t syscall_num) {
    switch (syscall_num) {
    case 15:  // rt_sigreturn
    case 24:  // sched_yield
    case 34:  // pause
    case 39:  // getpid
    case 57:  // fork
    case 58:  // vfork
    case 102: // getuid
    case 104: // getgid
    case 107: // geteuid
    case 108: // getegid
    case 110: // getppid
    case 111: // getpgrp
    case 112: // setsid
    case 162: // sync
    case 186: // gettid
    case 187: // readahead stubbed
    case 253: // inotify_init
    case 309: // getcpu stubbed
        return 0;
    case 3:   // close
    case 12:  // brk
    case 22:  // pipe
    case 32:  // dup
    case 37:  // alarm
    case 60:  // exit
    case 63:  // uname
    case 74:  // fsync
    case 75:  // fdatasync
    case 80:  // chdir
    case 81:  // fchdir
    case 84:  // rmdir
    case 87:  // unlink
    case 95:  // umask
    case 97:  // getrlimit
    case 98:  // getrusage
    case 99:  // sysinfo
    case 100: // times
    case 105: // setuid
    case 106: // setgid
    case 121: // getpgid
    case 124: // getsid
    case 122: // setfsuid
    case 123: // setfsgid
    case 125: // capget
    case 126: // capset
    case 127: // rt_sigpending
    case 130: // rt_sigsuspend
    case 135: // personality
    case 149: // mlock
    case 150: // munlock
    case 161: // chroot
    case 164: // settimeofday
    case 166: // umount2
    case 170: // sethostname
    case 200: // tkill
    case 201: // time
    case 218: // set_tid_address
    case 231: // exit_group
    case 272: // unshare
    case 273: // set_robust_list
    case 277: // sync_file_range stubbed
    case 284: // eventfd
    case 291: // epoll_create
    case 294: // inotify_init1
    case 67:  // shmdt
    case 225: // timer_getoverrun
    case 226: // timer_delete
        return 1;
    case 4:   // stat
    case 5:   // fstat
    case 6:   // lstat
    case 132: // utime
    case 21:  // access
    case 33:  // dup2
    case 48:  // shutdown
    case 50:  // listen
    case 62:  // kill
    case 73:  // flock
    case 79:  // getcwd
    case 82:  // rename
    case 85:  // creat
    case 86:  // link
    case 88:  // symlink
    case 90:  // chmod
    case 109: // setpgid
    case 113: // setreuid
    case 114: // setregid
    case 115: // getgroups
    case 116: // setgroups
    case 35:  // nanosleep
    case 96:  // gettimeofday
    case 131: // sigaltstack
    case 140: // getpriority
    case 143: // sched_getparam
    case 144: // sched_setscheduler
    case 145: // sched_getscheduler
    case 146: // sched_get_priority_max
    case 147: // sched_get_priority_min
    case 160: // setrlimit
    case 158: // arch_prctl
    case 229: // clock_getres
    case 235: // utimes
    case 252: // ioprio_get
    case 255: // inotify_rm_watch
    case 283: // timerfd_create
    case 287: // timerfd_gettime
    case 228: // clock_gettime
    case 290: // eventfd2
    case 293: // pipe2
    case 403: // clock_gettime64
    case 404: // clock_settime64
    case 406: // clock_getres_time64
    case 408: // timer_gettime64
    case 410: // timerfd_gettime64
    case 435: // clone3
    case 319: // memfd_create
        return 2;
    case 0:   // read
    case 1:   // write
    case 2:   // open
    case 7:   // poll
    case 8:   // lseek
    case 10:  // mprotect
    case 16:  // ioctl
    case 19:  // readv
    case 20:  // writev
    case 26:  // msync
    case 28:  // madvise
    case 29:  // shmget
    case 30:  // shmat
    case 31:  // shmctl
    case 41:  // socket
    case 42:  // connect
    case 43:  // accept
    case 46:  // sendmsg
    case 47:  // recvmsg
    case 49:  // bind
    case 51:  // getsockname
    case 52:  // getpeername
    case 72:  // fcntl
    case 83:  // mkdir
    case 89:  // readlink
    case 91:  // fchmod
    case 92:  // chown
    case 93:  // fchown
    case 94:  // lchown
    case 101: // ptrace
    case 117: // setresuid
    case 118: // getresuid
    case 119: // setresgid
    case 120: // getresgid
    case 133: // mknod
    case 141: // setpriority
    case 165: // mount
    case 203: // sched_setaffinity
    case 204: // sched_getaffinity
    case 213: // epoll_create
    case 217: // getdents64
    case 227: // clock_settime
    case 230: // clock_nanosleep
    case 234: // tgkill
    case 251: // ioprio_set
    case 254: // inotify_add_watch
    case 258: // mkdirat
    case 261: // futimesat
    case 263: // unlinkat
    case 266: // symlinkat
    case 268: // fchmodat
    case 269: // faccessat
    case 103: // syslog
    case 38:  // setitimer
    case 59:  // execve
    case 169: // reboot
    case 292: // dup3
    case 318: // getrandom
    case 324: // membarrier
    case 274: // get_robust_list
    case 282: // signalfd
    case 334: // rseq
    case 222: // timer_create
    case 238: // set_mempolicy
        return 3;
    case 13:  // rt_sigaction
    case 14:  // rt_sigprocmask
    case 40:  // sendfile
    case 78:  // getdents
    case 128: // rt_sigtimedwait
    case 232: // epoll_wait
    case 233: // epoll_ctl
    case 53:  // socketpair
    case 54:  // setsockopt
    case 61:  // wait4
    case 188: // setxattr
    case 189: // lsetxattr
    case 190: // fsetxattr
    case 257: // openat
    case 259: // mknodat
    case 262: // newfstatat
    case 264: // renameat
    case 267: // readlinkat
    case 280: // utimensat
    case 288: // accept4
    case 289: // signalfd4
    case 302: // prlimit64
    case 307: // sendmmsg
    case 223: // timer_settime
    case 407: // clock_nanosleep_time64
    case 409: // timer_settime64
    case 411: // timerfd_settime64
    case 412: // utimensat64
    case 421: // rt_sigtimedwait_time64
    case 437: // openat2
    case 439: // faccessat2 wired to faccessat for now
    case 452: // fchmodat2
        return 4;
    case 56:  // clone
    case 25:  // mremap
    case 55:  // getsockopt
    case 157: // prctl
    case 23:  // select
    case 191: // getxattr
    case 192: // lgetxattr
    case 193: // fgetxattr
    case 194: // listxattr
    case 195: // llistxattr
    case 196: // flistxattr
    case 250: // keyctl
    case 260: // fchownat
    case 265: // linkat
    case 271: // ppoll
    case 286: // timerfd_settime
    case 299: // recvmmsg
    case 316: // renameat2
    case 332: // statx
    case 322: // execveat
    case 239: // get_mempolicy
    case 247: // waitid
    case 414: // ppoll_time64
    case 417: // recvmmsg_time64
        return 5;
    case 197: // removexattr
    case 198: // lremovexattr
    case 199: // fremovexattr
        return 2;
    case 9:   // mmap
    case 44:  // sendto
    case 45:  // recvfrom
    case 202: // futex
    case 237: // mbind
    case 275: // splice
    case 310: // process_vm_readv
    case 326: // copy_file_range
    case 413: // pselect_time64
    case 422: // futex_time64
    case 441: // epoll_pwait2
        return 6;
    default:
        return 6;
    }
}

static bool marshal_syscall_args_legacy(enum guest_abi abi, qword_t syscall_num,
        const qword_t raw_args[6], dword_t args[6]) {
    if (abi == GUEST_ABI_AMD64) {
        switch (syscall_num) {
        case 17: // pread64
        case 18: // pwrite64
            if (!syscall_arg_fits_legacy_dword(raw_args[0]) ||
                    !syscall_arg_fits_legacy_dword(raw_args[1]) ||
                    !syscall_arg_fits_legacy_dword(raw_args[2]))
                return false;
            args[0] = (dword_t) raw_args[0];
            args[1] = (dword_t) raw_args[1];
            args[2] = (dword_t) raw_args[2];
            args[3] = (dword_t) raw_args[3];
            args[4] = (dword_t) (raw_args[3] >> 32);
            args[5] = 0;
            return true;
        case 76: // truncate
            if (!syscall_arg_fits_legacy_dword(raw_args[0]))
                return false;
            args[0] = (dword_t) raw_args[0];
            args[1] = (dword_t) raw_args[1];
            args[2] = (dword_t) (raw_args[1] >> 32);
            args[3] = 0;
            args[4] = 0;
            args[5] = 0;
            return true;
        case 77: // ftruncate
            if (!syscall_arg_fits_legacy_dword(raw_args[0]))
                return false;
            args[0] = (dword_t) raw_args[0];
            args[1] = (dword_t) raw_args[1];
            args[2] = (dword_t) (raw_args[1] >> 32);
            args[3] = 0;
            args[4] = 0;
            args[5] = 0;
            return true;
        case 285: // fallocate
            if (!syscall_arg_fits_legacy_dword(raw_args[0]) ||
                    !syscall_arg_fits_legacy_dword(raw_args[1]))
                return false;
            args[0] = (dword_t) raw_args[0];
            args[1] = (dword_t) raw_args[1];
            args[2] = (dword_t) raw_args[2];
            args[3] = (dword_t) (raw_args[2] >> 32);
            args[4] = (dword_t) raw_args[3];
            args[5] = (dword_t) (raw_args[3] >> 32);
            return true;
        case 56: // clone
            if (!syscall_arg_fits_legacy_dword(raw_args[0]) ||
                    !syscall_arg_fits_legacy_dword(raw_args[1]) ||
                    !syscall_arg_fits_legacy_dword(raw_args[2]) ||
                    !syscall_arg_fits_legacy_dword(raw_args[3]) ||
                    !syscall_arg_fits_legacy_dword(raw_args[4]))
                return false;
            args[0] = (dword_t) raw_args[0];
            args[1] = (dword_t) raw_args[1];
            args[2] = (dword_t) raw_args[2];
            args[3] = (dword_t) raw_args[4];
            args[4] = (dword_t) raw_args[3];
            args[5] = 0;
            return true;
        case 202: // futex
        case 422: // futex_time64
            args[0] = (dword_t) raw_args[0];
            args[1] = (dword_t) raw_args[1];
            args[2] = (dword_t) raw_args[2];
            args[3] = (dword_t) raw_args[3];
            args[4] = (dword_t) raw_args[4];
            args[5] = (dword_t) raw_args[5];
            return true;
        }
    }

    unsigned arg_count = abi == GUEST_ABI_AMD64 ? amd64_syscall_legacy_arg_count(syscall_num) : 6;
    for (unsigned i = 0; i < arg_count; i++) {
        if (abi == GUEST_ABI_AMD64 && !syscall_arg_fits_legacy_dword(raw_args[i]))
            return false;
        args[i] = (dword_t) raw_args[i];
    }
    for (unsigned i = arg_count; i < 6; i++)
        args[i] = 0;
    return true;
}

static void log_stub_syscall(struct cpu_state *cpu, const struct syscall_abi_dispatch *dispatch,
        unsigned syscall_num, const char *kind) {
    enum { STUB_LOG_MAX = 512, STUB_LOG_BUDGET = 5 };
    static unsigned short stub_log_count[2][STUB_LOG_MAX];
    bool always_log = dispatch->abi == GUEST_ABI_AMD64 && strcmp(current->comm, "bash") == 0;

    unsigned short *count = NULL;
    if (!always_log && dispatch->abi < 2 && syscall_num < STUB_LOG_MAX)
        count = &stub_log_count[dispatch->abi][syscall_num];

    if (count != NULL) {
        if (*count >= STUB_LOG_BUDGET) {
            if (*count == STUB_LOG_BUDGET) {
                printk("INFO: suppressing further %s %s syscall logs for %u\n",
                       dispatch->name, kind, syscall_num);
                (*count)++;
            }
            return;
        }
        (*count)++;
    }

    if (dispatch->abi == GUEST_ABI_AMD64) {
        printk("ERROR: %d(%s) %s %s syscall %u rip=0x%x rax=0x%llx rdi=0x%llx rsi=0x%llx rdx=0x%llx\n",
               current->pid, current->comm, dispatch->name, kind, syscall_num, cpu->eip,
               (unsigned long long) cpu->amd64_syscall.rax,
               (unsigned long long) cpu->amd64_syscall.rdi,
               (unsigned long long) cpu->amd64_syscall.rsi,
               (unsigned long long) cpu->amd64_syscall.rdx);
        return;
    }

    printk("ERROR: %d(%s) %s %s syscall %u eip=0x%x eax=0x%x ebx=0x%x ecx=0x%x edx=0x%x\n",
           current->pid, current->comm, dispatch->name, kind, syscall_num, cpu->eip,
           cpu->eax, cpu->ebx, cpu->ecx, cpu->edx);
}

static void amd64_syscall_seed_legacy_regs(struct cpu_state *cpu) {
    cpu->amd64_syscall.rax = cpu->amd64_regs[amd64_rax];
    cpu->amd64_syscall.rdi = cpu->amd64_regs[amd64_rdi];
    cpu->amd64_syscall.rsi = cpu->amd64_regs[amd64_rsi];
    cpu->amd64_syscall.rdx = cpu->amd64_regs[amd64_rdx];
    cpu->amd64_syscall.r10 = cpu->amd64_regs[amd64_r10];
    cpu->amd64_syscall.r8 = cpu->amd64_regs[amd64_r8];
    cpu->amd64_syscall.r9 = cpu->amd64_regs[amd64_r9];
    cpu->amd64_syscall.rcx = cpu->amd64_rip;
    cpu->amd64_syscall.r11 = cpu->eflags;
}

static void handle_amd64_syscall_interrupt(struct cpu_state *cpu) {
    if (current->abi != GUEST_ABI_AMD64) {
        printk("ERROR: %d(%s) amd64 syscall instruction in non-amd64 task at 0x%x\n",
               current->pid, current->comm, cpu->eip);
        struct siginfo_ info = {
            .code = SI_KERNEL_,
            .fault.addr = cpu->eip,
        };
        deliver_signal(current, SIGILL_, info);
        return;
    }

    amd64_syscall_seed_legacy_regs(cpu);
    handle_syscall_interrupt(cpu);
}

void handle_syscall_interrupt(struct cpu_state *cpu) {
    const struct syscall_abi_dispatch *dispatch = syscall_dispatch_for_abi(current->abi);
    if (dispatch->table == NULL) {
        printk("ERROR: %d(%s) syscall dispatch for guest ABI %s is not implemented yet\n",
               current->pid, current->comm, dispatch->name);
        deliver_signal(current, SIGSYS_, SIGINFO_NIL);
        return;
    }

    qword_t raw_args[6];
    dword_t args[6];
    qword_t syscall_num = dispatch->syscall_number(cpu);
    if (syscall_num >= dispatch->num_syscalls) {
        printk("ERROR: %d(%s) missing %s syscall %d\n",
               current->pid, current->comm, dispatch->name, (int) syscall_num);
        deliver_signal(current, SIGSYS_, SIGINFO_NIL);
        return;
    }

    if (current->ptrace.traced && current->ptrace.stop_at_syscall)
        ptrace_syscall_stop(cpu);

    syscall_t syscall = dispatch->table[syscall_num];
    if (syscall == NULL) {
        log_stub_syscall(cpu, dispatch, (unsigned) syscall_num, "missing");
        dispatch->syscall_result(cpu, syscall_stub());
        return;
    }
    if (syscall_is_logged_stub(syscall))
        log_stub_syscall(cpu, dispatch, (unsigned) syscall_num, "stub");

    dispatch->syscall_args(cpu, raw_args);
    if (dispatch->abi == GUEST_ABI_AMD64 &&
            handle_amd64_native_memory_syscall(cpu, syscall_num, raw_args)) {
        if (current->ptrace.traced && current->ptrace.stop_at_syscall)
            ptrace_syscall_stop(cpu);
        return;
    }
    if (!marshal_syscall_args_legacy(dispatch->abi, syscall_num, raw_args, args)) {
        printk("ERROR: %d(%s) %s syscall %llu needs full-width args before it can be emulated\n",
               current->pid, current->comm, dispatch->name, syscall_num);
        deliver_signal(current, SIGSYS_, SIGINFO_NIL);
        return;
    }

    STRACE("%d(%s) %d:%d %s call %-3llu ", current->pid, current->comm,
           current->reference.count, current->locks_held.count, dispatch->name, syscall_num);
    dword_t result = syscall(args[0], args[1], args[2], args[3], args[4], args[5]);
    dispatch->syscall_result(cpu, result);
    if (current->ptrace.traced && current->ptrace.stop_at_syscall)
        ptrace_syscall_stop(cpu);
    STRACE(" = 0x%x\n", result);
}

static void dump_opcode_window(guest_addr_t ip);
static void dump_amd64_regs(const struct cpu_state *cpu);
static void dump_amd64_hlt_tls_state(const struct cpu_state *cpu);
static void dump_amd64_loader_state(const struct cpu_state *cpu);
static void dump_amd64_store_trace(const struct cpu_state *cpu);
static void dump_fault_pt_state(guest_addr_t addr);
static bool amd64_verbose_fault_trace_enabled(void);

void handle_page_fault_interrupt(struct cpu_state *cpu) {
    read_lock(&current->mem->lock);
    void *ptr = mem_ptr(current->mem, cpu->segfault_addr, cpu->segfault_was_write ? MEM_WRITE : MEM_READ);
    read_unlock(&current->mem->lock);

    if (ptr == NULL) {
        printk("ERROR: %d(%s) page fault on %#llx at %#llx%s\n",
               current->pid, current->comm,
               (unsigned long long) cpu->segfault_addr,
               (unsigned long long) (current->abi == GUEST_ABI_AMD64 ? cpu->amd64_rip : cpu->eip),
               cpu->segfault_was_write ? " (write)" : " (read)");
        dump_opcode_window(current->abi == GUEST_ABI_AMD64 ? cpu->amd64_rip : cpu->eip);
        if (current->abi == GUEST_ABI_AMD64) {
            dump_amd64_regs(cpu);
            dump_fault_pt_state(cpu->segfault_addr);
            if (amd64_verbose_fault_trace_enabled()) {
                dump_amd64_loader_state(cpu);
                dump_amd64_store_trace(cpu);
            }
        }
        struct siginfo_ info = {
            .code = mem_segv_reason(current->mem, cpu->segfault_addr),
            .fault.addr = cpu->segfault_addr,
        };
        dump_stack(8);
        deliver_signal(current, SIGSEGV_, info);
    }
}

static void dump_fault_pt_state(guest_addr_t addr) {
    enum { PT_FAULT_LOG_BUDGET = 8 };
    static unsigned fault_pt_log_count;
    if (fault_pt_log_count >= PT_FAULT_LOG_BUDGET)
        return;
    fault_pt_log_count++;

    read_lock(&current->mem->lock);
    page_t center = PAGE(addr);
    page_t start = center > 1 ? center - 1 : center;
    page_t end = center + 1;
    printk("amd64 fault pt: floor=%#llx ceiling=%#llx page=%#llx\n",
           (unsigned long long) ((guest_addr_t) current->mem->mmap_floor << PAGE_BITS),
           (unsigned long long) ((guest_addr_t) current->mem->mmap_ceiling << PAGE_BITS),
           (unsigned long long) center);
    for (page_t page = start; page <= end; page++) {
        struct pt_entry *pt = mem_pt(current->mem, page);
        if (pt == NULL) {
            printk("amd64 fault pt: %#llx unmapped\n",
                   (unsigned long long) ((guest_addr_t) page << PAGE_BITS));
            continue;
        }
        const char *name = NULL;
        if (pt->data != NULL)
            name = pt->data->name;
        printk("amd64 fault pt: %#llx flags=%#x anon=%d cow=%d shared=%d off=%#zx name=%s\n",
               (unsigned long long) ((guest_addr_t) page << PAGE_BITS),
               pt->flags,
               !!(pt->flags & P_ANONYMOUS),
               !!(pt->flags & P_COW),
               !!(pt->flags & P_SHARED),
               pt->offset,
               name != NULL ? name : "-");
    }
    read_unlock(&current->mem->lock);
}

static void handle_general_protection_interrupt(struct cpu_state *cpu) {
    uint8_t first_opcode = 0;
    bool have_first_opcode = user_get(cpu->eip, first_opcode) == 0;
    printk("ERROR: %d(%s) general protection fault at 0x%x: ", current->pid, current->comm, cpu->eip);
    for (int i = 0; i < 8; i++) {
        uint8_t b;
        if (user_get(cpu->eip + i, b))
            break;
        printk("%02x ", b);
    }
    printk("\n");
    dump_opcode_window(cpu->eip);
    if (current->abi == GUEST_ABI_AMD64) {
        dump_amd64_regs(cpu);
        if (have_first_opcode && first_opcode == 0xf4)
            dump_amd64_hlt_tls_state(cpu);
        if (amd64_verbose_fault_trace_enabled()) {
            dump_amd64_loader_state(cpu);
            dump_amd64_store_trace(cpu);
        }
    }
    dump_stack(8);
    cpu->trapno = INT_GPF;
    cpu->segfault_addr = 0;
    cpu->segfault_was_write = false;
    struct siginfo_ info = {
        .code = SI_KERNEL_,
        .fault.addr = cpu->eip,
    };
    deliver_signal(current, SIGSEGV_, info);
}

static int amd64_nop_instruction_len(addr_t ip) {
    byte_t bytes[16];
    for (size_t i = 0; i < sizeof(bytes); i++) {
        if (user_get(ip + i, bytes[i]))
            return 0;
    }

    int i = 0;
    while (i < (int) sizeof(bytes) && (bytes[i] == 0x66 || bytes[i] == 0x2e || bytes[i] == 0x3e))
        i++;
    if (i >= (int) sizeof(bytes))
        return 0;

    if (bytes[i] == 0x90)
        return i + 1;

    if (i + 2 >= (int) sizeof(bytes) || bytes[i] != 0x0f || bytes[i + 1] != 0x1f)
        return 0;
    i += 2;

    byte_t modrm = bytes[i++];
    if (((modrm >> 3) & 7) != 0)
        return 0;

    int mod = modrm >> 6;
    int rm = modrm & 7;
    bool has_sib = mod != 3 && rm == 4;
    int disp_len = 0;

    if (has_sib) {
        byte_t sib = bytes[i++];
        int base = sib & 7;
        if (mod == 0 && base == 5)
            disp_len = 4;
    } else if (mod == 0 && rm == 5) {
        disp_len = 4;
    }

    if (mod == 1)
        disp_len = 1;
    else if (mod == 2)
        disp_len = 4;

    if (i + disp_len > (int) sizeof(bytes))
        return 0;
    return i + disp_len;
}

static bool amd64_verbose_fault_trace_enabled(void) {
    return false;
}

static void dump_opcode_window(guest_addr_t ip) {
    const int before = 4;
    const int after = 12;

    printk("opcode window around %#llx: ", (unsigned long long) ip);
    for (int i = -before; i < after; i++) {
        uint8_t b;
        guest_addr_t addr = ip + i;
        if (i == 0)
            printk("[");
        if (user_get(addr, b))
            printk("??");
        else
            printk("%02x", b);
        if (i == 0)
            printk("]");
        else
            printk(" ");
    }
    printk("\n");
}

static void dump_amd64_regs(const struct cpu_state *cpu) {
    printk("amd64 regs: rax=%#llx rbx=%#llx rcx=%#llx rdx=%#llx rsi=%#llx rdi=%#llx\n",
           (unsigned long long) cpu->amd64_regs[amd64_rax],
           (unsigned long long) cpu->amd64_regs[amd64_rbx],
           (unsigned long long) cpu->amd64_regs[amd64_rcx],
           (unsigned long long) cpu->amd64_regs[amd64_rdx],
           (unsigned long long) cpu->amd64_regs[amd64_rsi],
           (unsigned long long) cpu->amd64_regs[amd64_rdi]);
    printk("amd64 regs: rbp=%#llx rsp=%#llx r8=%#llx r9=%#llx r10=%#llx r11=%#llx\n",
           (unsigned long long) cpu->amd64_regs[amd64_rbp],
           (unsigned long long) cpu->amd64_regs[amd64_rsp],
           (unsigned long long) cpu->amd64_regs[amd64_r8],
           (unsigned long long) cpu->amd64_regs[amd64_r9],
           (unsigned long long) cpu->amd64_regs[amd64_r10],
           (unsigned long long) cpu->amd64_regs[amd64_r11]);
    printk("amd64 regs: r12=%#llx r13=%#llx r14=%#llx r15=%#llx rip=%#llx\n",
           (unsigned long long) cpu->amd64_regs[amd64_r12],
           (unsigned long long) cpu->amd64_regs[amd64_r13],
           (unsigned long long) cpu->amd64_regs[amd64_r14],
           (unsigned long long) cpu->amd64_regs[amd64_r15],
           (unsigned long long) cpu->amd64_rip);
}

static void dump_amd64_hlt_tls_state(const struct cpu_state *cpu) {
    qword_t fs_base = cpu->tls_ptr;
    qword_t fs_self = 0;
    qword_t fs_dtv = 0;
    int self_fault = fs_base == 0 ? 1 : user_get((addr_t) fs_base, fs_self);
    int dtv_fault = fs_base == 0 ? 1 : user_get((addr_t) (fs_base + 8), fs_dtv);

    printk("amd64 hlt tls: fsbase=%#llx rax=%#llx rbp=%#llx fs:[0]=",
           (unsigned long long) fs_base,
           (unsigned long long) cpu->amd64_regs[amd64_rax],
           (unsigned long long) cpu->amd64_regs[amd64_rbp]);
    if (self_fault)
        printk("??");
    else
        printk("%#llx", (unsigned long long) fs_self);
    printk(" fs:[8]=");
    if (dtv_fault)
        printk("??");
    else
        printk("%#llx", (unsigned long long) fs_dtv);
    printk("\n");
}

static void dump_amd64_loader_state(const struct cpu_state *cpu) {
    qword_t dso = cpu->amd64_regs[amd64_rax];
    qword_t deps = 0, dep_count = 0, dep_index = 0;
    if (dso == 0)
        return;
    if (user_get((addr_t) dso + 0xb0, deps) || user_get((addr_t) dso + 0xc0, dep_count) ||
            user_get((addr_t) dso + 0xc8, dep_index)) {
        printk("amd64 loader: dso=%#llx (failed to read b0/c0/c8)\n",
               (unsigned long long) dso);
        return;
    }
    printk("amd64 loader: dso=%#llx deps=%#llx count=%#llx index=%#llx\n",
           (unsigned long long) dso,
           (unsigned long long) deps,
           (unsigned long long) dep_count,
           (unsigned long long) dep_index);
    if (deps != 0) {
        for (int i = 0; i < 4; i++) {
            qword_t slot = 0;
            if (user_get((addr_t) deps + i * sizeof(slot), slot))
                break;
            printk("amd64 loader: deps[%d]=%#llx\n", i, (unsigned long long) slot);
        }
    }
}

static void dump_amd64_store_trace(const struct cpu_state *cpu) {
    unsigned total = cpu->amd64_store_trace_next;
    unsigned start = total > AMD64_STORE_TRACE_COUNT ? total - AMD64_STORE_TRACE_COUNT : 0;
    for (unsigned i = start; i < total; i++) {
        const struct amd64_store_trace *trace = &cpu->amd64_store_trace[i % AMD64_STORE_TRACE_COUNT];
        if (trace->rip == 0)
            continue;
        printk("amd64 store: rip=%#llx opcode=%#x addr=%#llx value=%#llx\n",
               (unsigned long long) trace->rip,
               trace->opcode,
               (unsigned long long) trace->addr,
               (unsigned long long) trace->value);
    }
}

struct amd64_trap_rex_prefix {
    bool present;
    bool w;
    bool r;
    bool x;
    bool b;
};

struct amd64_trap_modrm {
    bool is_reg;
    uint8_t reg;
    uint8_t rm;
    bool has_base;
    uint8_t base;
    bool has_index;
    uint8_t index;
    uint8_t scale;
    bool rip_relative;
    int32_t disp;
};

static inline void amd64_trap_sync_legacy_regs(struct cpu_state *cpu) {
    cpu->eax = (dword_t) cpu->amd64_regs[amd64_rax];
    cpu->ecx = (dword_t) cpu->amd64_regs[amd64_rcx];
    cpu->edx = (dword_t) cpu->amd64_regs[amd64_rdx];
    cpu->ebx = (dword_t) cpu->amd64_regs[amd64_rbx];
    cpu->esp = (dword_t) cpu->amd64_regs[amd64_rsp];
    cpu->ebp = (dword_t) cpu->amd64_regs[amd64_rbp];
    cpu->esi = (dword_t) cpu->amd64_regs[amd64_rsi];
    cpu->edi = (dword_t) cpu->amd64_regs[amd64_rdi];
    cpu->eip = (dword_t) cpu->amd64_rip;
}

static inline qword_t amd64_trap_mask(unsigned size) {
    switch (size) {
    case 8: return 0xff;
    case 16: return 0xffff;
    case 32: return 0xffffffffu;
    case 64: return ~0ull;
    default: return 0;
    }
}

static inline qword_t amd64_trap_sign_bit(unsigned size) {
    return 1ull << (size - 1);
}

static inline qword_t amd64_trap_trunc(qword_t value, unsigned size) {
    return value & amd64_trap_mask(size);
}

static inline bool amd64_trap_guest_addr_ok(qword_t guest_addr, unsigned size, guest_addr_t *addr_out) {
    if (!guest_abi_range_valid(GUEST_ABI_AMD64, guest_addr, size))
        return false;
    *addr_out = guest_addr;
    return true;
}

static inline qword_t amd64_trap_reg_get(const struct cpu_state *cpu, unsigned reg, unsigned size) {
    qword_t value = cpu->amd64_regs[reg & 0xf];
    switch (size) {
    case 8: return value & 0xff;
    case 16: return value & 0xffff;
    case 32: return (uint32_t) value;
    case 64: return value;
    default: return value;
    }
}

static inline void amd64_trap_reg_set(struct cpu_state *cpu, unsigned reg, unsigned size, qword_t value) {
    reg &= 0xf;
    switch (size) {
    case 8:
        cpu->amd64_regs[reg] = (cpu->amd64_regs[reg] & ~0xffull) | (value & 0xff);
        break;
    case 16:
        cpu->amd64_regs[reg] = (cpu->amd64_regs[reg] & ~0xffffull) | (value & 0xffff);
        break;
    case 32:
        cpu->amd64_regs[reg] = (uint32_t) value;
        break;
    case 64:
        cpu->amd64_regs[reg] = value;
        break;
    default:
        break;
    }
}

static inline void amd64_trap_set_add_flags(struct cpu_state *cpu, qword_t lhs, qword_t rhs, qword_t result, unsigned size) {
    qword_t mask = amd64_trap_mask(size);
    qword_t lhs_masked = amd64_trap_trunc(lhs, size);
    qword_t rhs_masked = amd64_trap_trunc(rhs, size);
    qword_t res_masked = amd64_trap_trunc(result, size);
    cpu->cf = size == 64 ? res_masked < lhs_masked : ((lhs_masked + rhs_masked) & ~mask) != 0;
    cpu->of = ((~(lhs_masked ^ rhs_masked) & (lhs_masked ^ res_masked)) & amd64_trap_sign_bit(size)) != 0;
    cpu->af = ((lhs_masked ^ rhs_masked ^ res_masked) >> 4) & 1;
    cpu->af_ops = 0;
    cpu->zf = res_masked == 0;
    cpu->sf = (res_masked & amd64_trap_sign_bit(size)) != 0;
    cpu->pf = !__builtin_parity((unsigned) (res_masked & 0xff));
    cpu->zf_res = cpu->sf_res = cpu->pf_res = 0;
    collapse_flags(cpu);
}

static bool amd64_trap_fetch_u8(guest_addr_t *ip, byte_t *out) {
    if (user_get(*ip, *out))
        return false;
    (*ip)++;
    return true;
}

static bool amd64_trap_fetch_i8(guest_addr_t *ip, int8_t *out) {
    return amd64_trap_fetch_u8(ip, (byte_t *) out);
}

static bool amd64_trap_fetch_i32(guest_addr_t *ip, int32_t *out) {
    if (user_get(*ip, *out))
        return false;
    *ip += sizeof(*out);
    return true;
}

static bool amd64_trap_mem_read(qword_t guest_addr, unsigned size, qword_t *value) {
    guest_addr_t addr;
    if (!amd64_trap_guest_addr_ok(guest_addr, size, &addr))
        return false;
    switch (size) {
    case 8: {
        uint8_t tmp;
        if (user_get(addr, tmp))
            return false;
        *value = tmp;
        return true;
    }
    case 16: {
        uint16_t tmp;
        if (user_get(addr, tmp))
            return false;
        *value = tmp;
        return true;
    }
    case 32: {
        uint32_t tmp;
        if (user_get(addr, tmp))
            return false;
        *value = tmp;
        return true;
    }
    case 64: {
        uint64_t tmp;
        if (user_get(addr, tmp))
            return false;
        *value = tmp;
        return true;
    }
    default:
        return false;
    }
}

static bool amd64_trap_mem_write(qword_t guest_addr, unsigned size, qword_t value) {
    guest_addr_t addr;
    if (!amd64_trap_guest_addr_ok(guest_addr, size, &addr))
        return false;
    switch (size) {
    case 8: {
        uint8_t tmp = value;
        return user_put(addr, tmp) == 0;
    }
    case 16: {
        uint16_t tmp = value;
        return user_put(addr, tmp) == 0;
    }
    case 32: {
        uint32_t tmp = value;
        return user_put(addr, tmp) == 0;
    }
    case 64: {
        uint64_t tmp = value;
        return user_put(addr, tmp) == 0;
    }
    default:
        return false;
    }
}

static bool amd64_trap_decode_modrm(addr_t *ip, struct amd64_trap_rex_prefix rex, struct amd64_trap_modrm *modrm) {
    byte_t modrm_byte;
    if (!amd64_trap_fetch_u8(ip, &modrm_byte))
        return false;

    unsigned mod = modrm_byte >> 6;
    modrm->reg = ((modrm_byte >> 3) & 7) | (rex.r ? 8 : 0);
    modrm->rm = (modrm_byte & 7) | (rex.b ? 8 : 0);
    modrm->is_reg = mod == 3;
    modrm->has_base = false;
    modrm->has_index = false;
    modrm->rip_relative = false;
    modrm->disp = 0;
    modrm->scale = 0;

    if (modrm->is_reg)
        return true;

    unsigned rm_low = modrm_byte & 7;
    if (rm_low == 4) {
        byte_t sib;
        if (!amd64_trap_fetch_u8(ip, &sib))
            return false;
        unsigned base_low = sib & 7;
        unsigned index_low = (sib >> 3) & 7;
        modrm->scale = sib >> 6;
        // In 64-bit mode, SIB index 100 means "no index" only when REX.X is clear.
        if (index_low != 4 || rex.x) {
            modrm->has_index = true;
            modrm->index = index_low | (rex.x ? 8 : 0);
        }
        if (mod == 0 && base_low == 5) {
            modrm->has_base = false;
        } else {
            modrm->has_base = true;
            modrm->base = base_low | (rex.b ? 8 : 0);
        }
    } else if (mod == 0 && rm_low == 5) {
        modrm->rip_relative = true;
    } else {
        modrm->has_base = true;
        modrm->base = modrm->rm;
    }

    if (mod == 1) {
        int8_t disp8;
        if (!amd64_trap_fetch_i8(ip, &disp8))
            return false;
        modrm->disp = disp8;
    } else if (mod == 2 || (mod == 0 && (rm_low == 5 || (rm_low == 4 && !modrm->has_base)))) {
        int32_t disp32;
        if (!amd64_trap_fetch_i32(ip, &disp32))
            return false;
        modrm->disp = disp32;
    }
    return true;
}

static qword_t amd64_trap_effective_addr(struct cpu_state *cpu, const struct amd64_trap_modrm *modrm, bool fs_prefix, guest_addr_t rip_after_modrm) {
    qword_t addr = (qword_t) (sqword_t) modrm->disp;
    if (modrm->rip_relative)
        addr += rip_after_modrm;
    if (modrm->has_base)
        addr += cpu->amd64_regs[modrm->base];
    if (modrm->has_index)
        addr += cpu->amd64_regs[modrm->index] << modrm->scale;
    if (fs_prefix)
        addr += cpu->tls_ptr;
    return addr;
}

static bool amd64_trap_read_rm(struct cpu_state *cpu, const struct amd64_trap_modrm *modrm, bool fs_prefix, guest_addr_t rip_after_modrm, unsigned size, qword_t *value) {
    if (modrm->is_reg) {
        *value = amd64_trap_reg_get(cpu, modrm->rm, size);
        return true;
    }
    return amd64_trap_mem_read(amd64_trap_effective_addr(cpu, modrm, fs_prefix, rip_after_modrm), size, value);
}

static bool amd64_trap_write_rm(struct cpu_state *cpu, const struct amd64_trap_modrm *modrm, bool fs_prefix, guest_addr_t rip_after_modrm, unsigned size, qword_t value) {
    if (modrm->is_reg) {
        amd64_trap_reg_set(cpu, modrm->rm, size, value);
        return true;
    }
    return amd64_trap_mem_write(amd64_trap_effective_addr(cpu, modrm, fs_prefix, rip_after_modrm), size, value);
}

static bool amd64_try_emulate_add(guest_addr_t ip, struct cpu_state *cpu) {
    struct amd64_trap_rex_prefix rex = {};
    bool operand_size_prefix = false;
    bool fs_prefix = false;
    byte_t opcode;
    guest_addr_t decode_ip = ip;

    while (true) {
        if (!amd64_trap_fetch_u8(&decode_ip, &opcode))
            return false;
        if (opcode == 0x66) {
            operand_size_prefix = true;
            continue;
        }
        if (opcode == 0x2e || opcode == 0x3e) {
            continue;
        }
        if (opcode == 0x67) {
            continue;
        }
        if (opcode == 0x64) {
            fs_prefix = true;
            continue;
        }
        if (opcode >= 0x40 && opcode <= 0x4f) {
            rex.present = true;
            rex.w = (opcode & 0x8) != 0;
            rex.r = (opcode & 0x4) != 0;
            rex.x = (opcode & 0x2) != 0;
            rex.b = (opcode & 0x1) != 0;
            continue;
        }
        break;
    }

    if (opcode != 0x01)
        return false;

    unsigned op_size = rex.w ? 64 : (operand_size_prefix ? 16 : 32);
    struct amd64_trap_modrm modrm;
    if (!amd64_trap_decode_modrm(&decode_ip, rex, &modrm))
        return false;

    qword_t lhs, rhs, result;
    if (!amd64_trap_read_rm(cpu, &modrm, fs_prefix, decode_ip, op_size, &lhs))
        return false;
    rhs = amd64_trap_reg_get(cpu, modrm.reg, op_size);
    result = amd64_trap_trunc(lhs + rhs, op_size);
    if (!amd64_trap_write_rm(cpu, &modrm, fs_prefix, decode_ip, op_size, result))
        return false;

    amd64_trap_set_add_flags(cpu, lhs, rhs, result, op_size);
    cpu->amd64_rip = decode_ip;
    amd64_trap_sync_legacy_regs(cpu);
    return true;
}

void handle_illegal_instruction_interrupt(struct cpu_state *cpu) {
    if (current->abi == GUEST_ABI_AMD64) {
        int nop_len = amd64_nop_instruction_len(cpu->eip);
        if (nop_len > 0) {
            cpu->eip += nop_len;
            cpu->amd64_rip = cpu->eip;
            return;
        }
        if (amd64_try_emulate_add(cpu->eip, cpu))
            return;
    }

    printk("ERROR: %d(%s) illegal instruction at 0x%x: ", current->pid, current->comm, cpu->eip);
    for (int i = 0; i < 8; i++) {
        uint8_t b;
        if (user_get(cpu->eip + i, b))
            break;
        printk("%02x ", b);
    }
    printk("\n");
    dump_opcode_window(cpu->eip);
    if (current->abi == GUEST_ABI_AMD64) {
        dump_amd64_regs(cpu);
        if (amd64_verbose_fault_trace_enabled()) {
            dump_amd64_loader_state(cpu);
            dump_amd64_store_trace(cpu);
        }
    }
    dump_stack(8);
    struct siginfo_ info = {
        .code = SI_KERNEL_,
        .fault.addr = cpu->eip,
    };
    deliver_signal(current, SIGILL_, info);
}

static void handle_arithmetic_interrupt(struct cpu_state *cpu) {
    printk("ERROR: %d(%s) arithmetic fault at 0x%x\n", current->pid, current->comm, cpu->eip);
    dump_stack(8);
    struct siginfo_ info = {
        .code = SI_KERNEL_,
        .fault.addr = cpu->eip,
    };
    deliver_signal(current, SIGFPE_, info);
}

static void handle_privileged_instruction_interrupt(struct cpu_state *cpu) {
    cpu->trapno = INT_GPF;
    handle_general_protection_interrupt(cpu);
}

void handle_timer_interrupt(__attribute__((unused)) struct cpu_state *cpu) {
    // For now we just return.
    return;
}

void handle_interrupt(int interrupt) {
    struct cpu_state *cpu = &current->cpu;

    switch (interrupt) {
        case INT_SYSCALL:
            handle_syscall_interrupt(cpu);
            break;
        case INT_AMD64_SYSCALL:
            handle_amd64_syscall_interrupt(cpu);
            break;
        case INT_PF:
            handle_page_fault_interrupt(cpu);
            break;
        case INT_GPF:
            handle_general_protection_interrupt(cpu);
            break;
        case INT_UNDEFINED:
            handle_illegal_instruction_interrupt(cpu);
            break;
        case INT_DIV:
            handle_arithmetic_interrupt(cpu);
            break;
        case INT_PRIV:
            handle_privileged_instruction_interrupt(cpu);
            break;
        case INT_BREAKPOINT:
            complex_lockt(&pids_lock, 0);
            send_signal(current, SIGTRAP_, (struct siginfo_) {
                .sig = SIGTRAP_,
                .code = SI_KERNEL_,
            });
            unlock(&pids_lock);
            break;
        case INT_DEBUG:
            complex_lockt(&pids_lock, 0);
            send_signal(current, SIGTRAP_, (struct siginfo_) {
                .sig = SIGTRAP_,
                .code = TRAP_TRACE_,
            });
            unlock(&pids_lock);
            break;
        case INT_TIMER:
            handle_timer_interrupt(cpu); // Just a stub for now
            break;
        default:
            printk("WARNING: %d(%s) unhandled interrupt %d\n", current->pid, current->comm, interrupt);
            sys_exit(interrupt);
            break;
    }
    // The common logic to be executed after handling any interrupt
    receive_signals();
    struct tgroup *group = current->group;
    lock(&group->lock, 0);
    while (group->stopped)
        wait_for_ignore_signals(&group->stopped_cond, &group->lock, NULL);
    unlock(&group->lock);
}


void dump_maps(void) {
    extern void proc_maps_dump(struct task *task, struct proc_data *buf);
    struct proc_data buf = {};
    proc_maps_dump(current, &buf);
    // go a line at a time because it can be fucking enormous
    char *orig_data = buf.data;
    while (buf.size > 0) {
        size_t chunk_size = buf.size;
        if (chunk_size > 1024)
            chunk_size = 1024;
        printk("%.*s", chunk_size, buf.data);
        buf.data += chunk_size;
        buf.size -= chunk_size;
    }
    free(orig_data);
}

void dump_mem(addr_t start, uint_t len) {
    const int width = 8;
    if (len == 0) {
        printk("ERROR: No memory to dump\n");
        return;
    }

    for (addr_t addr = start; addr < start + len; addr += sizeof(dword_t)) {
        unsigned from_left = (addr - start) / sizeof(dword_t) % width;
        if (from_left == 0)
            printk("%08x: ", addr);

        dword_t word;
        if (user_get(addr, word)) {
            printk("ERROR: Unable to read memory at address %08x\n", addr);
            break;
        }
        printk("%08x ", word);

        if (from_left == width - 1)
            printk("\n");
    }
}

void dump_stack(int lines) {
    printk("stack at %x, base at %x, ip at %x\n", current->cpu.esp, current->cpu.ebp, current->cpu.eip);
    if (lines <= 0) {
        printk("ERROR: Invalid number of lines specified for stack dump\n");
        return;
    }
    // Ensure lines is within a reasonable limit to prevent excessive output
    const int max_lines = 20;
    lines = (lines > max_lines) ? max_lines : lines;

    dump_mem(current->cpu.esp, lines * sizeof(dword_t) * 8);
}

// TODO find a home for this
#ifdef LOG_OVERRIDE
int log_override = 0;
#endif
