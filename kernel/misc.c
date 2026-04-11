#include <string.h>
#include "kernel/calls.h"

#define PRCTL_SET_PDEATHSIG_ 1
#define PRCTL_GET_PDEATHSIG_ 2
#define PRCTL_GET_DUMPABLE_ 3
#define PRCTL_SET_DUMPABLE_ 4
#define PRCTL_GET_KEEPCAPS_ 7
#define PRCTL_SET_KEEPCAPS_ 8
#define PRCTL_SET_NAME_ 15
#define PRCTL_GET_NAME_ 16
#define PRCTL_GET_SECCOMP_ 21
#define PRCTL_SET_SECCOMP_ 22
#define PRCTL_CAPBSET_READ_ 23
#define PRCTL_GET_SECUREBITS_ 27
#define PRCTL_SET_SECUREBITS_ 28
#define PRCTL_SET_TIMERSLACK_ 29
#define PRCTL_GET_TIMERSLACK_ 30
#define PRCTL_SET_MM_ 35
#define PRCTL_SET_CHILD_SUBREAPER_ 36
#define PRCTL_GET_CHILD_SUBREAPER_ 37
#define PRCTL_SET_NO_NEW_PRIVS_ 38
#define PRCTL_GET_NO_NEW_PRIVS_ 39
#define PRCTL_CAP_AMBIENT_ 47

#define PRCTL_CAP_AMBIENT_IS_SET_ 1
#define PRCTL_CAP_AMBIENT_RAISE_ 2
#define PRCTL_CAP_AMBIENT_LOWER_ 3
#define PRCTL_CAP_AMBIENT_CLEAR_ALL_ 4

#define PRCTL_SET_MM_ARG_START_ 8
#define PRCTL_SET_MM_ARG_END_ 9
#define PRCTL_SET_MM_ENV_START_ 10
#define PRCTL_SET_MM_ENV_END_ 11

#define PRCTL_CAP_LAST_CAP_ 63
#define PRCTL_DEFAULT_TIMERSLACK_NS_ 50000

#define KEYCTL_GET_KEYRING_ID_ 0
#define KEYCTL_JOIN_SESSION_KEYRING_ 1
#define KEYCTL_SETPERM_ 5
#define KEYCTL_SESSION_TO_PARENT_ 18

#define ARCH_SET_GS_ 0x1001
#define ARCH_SET_FS_ 0x1002
#define ARCH_GET_FS_ 0x1003
#define ARCH_GET_GS_ 0x1004

static bool prctl_cap_valid(uint_t cap) {
    return cap <= PRCTL_CAP_LAST_CAP_;
}

static bool prctl_cap_test(const dword_t caps[2], uint_t cap) {
    if (!prctl_cap_valid(cap))
        return false;
    return (caps[cap / 32] & (1u << (cap % 32))) != 0;
}

int_t sys_prctl(dword_t option, uint_t arg2, uint_t arg3, uint_t UNUSED(arg4), uint_t UNUSED(arg5)) {
    switch (option) {
        case PRCTL_SET_PDEATHSIG_:
            current->pdeath_signal = arg2;
            return 0;
        case PRCTL_GET_PDEATHSIG_:
            if (user_put(arg2, current->pdeath_signal))
                return _EFAULT;
            return 0;
        case PRCTL_GET_DUMPABLE_:
            return 1;
        case PRCTL_SET_DUMPABLE_:
            if (arg2 > 1)
                return _EINVAL;
            return 0;
        case PRCTL_GET_KEEPCAPS_:
        case PRCTL_SET_KEEPCAPS_:
            // Compatibility stub: enough for capability-probing startup code.
            return 0;
        case PRCTL_GET_NAME_: {
            char name[16] = {};
            lock(&current->general_lock, 0);
            strncpy(name, current->comm, sizeof(name) - 1);
            unlock(&current->general_lock);
            if (user_write(arg2, name, sizeof(name)))
                return _EFAULT;
            return 0;
        }
        case PRCTL_GET_SECCOMP_:
            // Report "disabled" rather than erroring out during helper setup.
            return 0;
        case PRCTL_SET_SECCOMP_:
            // Compatibility stub: userland may try to sandbox helpers.
            // Pretend success instead of returning EINVAL, which newer apt
            // treats as a startup failure.
            STRACE("prctl(PR_SET_SECCOMP, %#x)", arg2);
            return 0;
        case PRCTL_SET_NAME_: {
            char name[16];
            if (user_read_string(arg2, name, sizeof(name) - 1))
                return _EFAULT;
            name[sizeof(name) - 1] = '\0';
            STRACE("prctl(PRCTL_SET_NAME, \"%s\")", name);
            lock(&current->general_lock, 0);
            strncpy(current->comm, name, sizeof(current->comm));
            unlock(&current->general_lock);
            return 0;
        }
        case PRCTL_CAPBSET_READ_:
            if (!prctl_cap_valid(arg2))
                return _EINVAL;
            // We do not model a separate bounding set. Use the permitted set so
            // capability probes see a coherent answer instead of EINVAL.
            return prctl_cap_test(current->cap_permitted, arg2) ? 1 : 0;
        case PRCTL_GET_SECUREBITS_:
            return 0;
        case PRCTL_SET_SECUREBITS_:
            return 0;
        case PRCTL_SET_TIMERSLACK_:
            return 0;
        case PRCTL_GET_TIMERSLACK_:
            return PRCTL_DEFAULT_TIMERSLACK_NS_;
        case PRCTL_SET_MM_:
            if (!superuser())
                return _EPERM;
            switch (arg2) {
                case PRCTL_SET_MM_ARG_START_:
                case PRCTL_SET_MM_ARG_END_:
                case PRCTL_SET_MM_ENV_START_:
                case PRCTL_SET_MM_ENV_END_:
                    return 0;
                default:
                    return _EINVAL;
            }
        case PRCTL_SET_CHILD_SUBREAPER_:
            if (arg2 > 1)
                return _EINVAL;
            return 0;
        case PRCTL_GET_CHILD_SUBREAPER_: {
            dword_t value = 0;
            if (user_write(arg2, &value, sizeof(value)))
                return _EFAULT;
            return 0;
        }
        case PRCTL_SET_NO_NEW_PRIVS_:
            if (arg2 > 1)
                return _EINVAL;
            STRACE("prctl(PR_SET_NO_NEW_PRIVS, %#x)", arg2);
            return 0;
        case PRCTL_GET_NO_NEW_PRIVS_:
            return 0;
        case PRCTL_CAP_AMBIENT_:
            switch (arg2) {
                case PRCTL_CAP_AMBIENT_IS_SET_:
                    if (!prctl_cap_valid(arg3))
                        return _EINVAL;
                    return 0;
                case PRCTL_CAP_AMBIENT_RAISE_:
                case PRCTL_CAP_AMBIENT_LOWER_:
                    if (!prctl_cap_valid(arg3))
                        return _EINVAL;
                    return 0;
                case PRCTL_CAP_AMBIENT_CLEAR_ALL_:
                    return 0;
                default:
                    return _EINVAL;
            }
        default:
            STRACE("prctl(%#x)", option);
            return _EINVAL;
    }
}

int_t sys_arch_prctl(int_t code, addr_t addr) {
    STRACE("arch_prctl(%#x, %#x)", code, addr);
    if (!task_is_64bit(current))
        return _EINVAL;

    switch (code) {
        case ARCH_SET_FS_:
            current->cpu.tls_ptr = addr;
            return 0;
        case ARCH_GET_FS_: {
            qword_t fs_base = current->cpu.tls_ptr;
            if (user_put(addr, fs_base))
                return _EFAULT;
            return 0;
        }
        case ARCH_SET_GS_:
        case ARCH_GET_GS_:
            // The current long-mode bring-up only has one TLS base, used for
            // amd64 FS-relative accesses.
            return _EINVAL;
        default:
            return _EINVAL;
    }
}

int_t sys_rseq(addr_t rseq_addr, dword_t rseq_len, dword_t flags, dword_t sig) {
    STRACE("rseq(%#x, %u, %#x, %#x)", rseq_addr, rseq_len, flags, sig);
    // Deliberately report rseq as unsupported. Modern glibc falls back cleanly
    // on ENOSYS, but a fake success here would expose an ABI we do not emulate.
    return _ENOSYS;
}

int_t sys_keyctl(dword_t cmd, dword_t arg2, dword_t arg3, dword_t arg4, dword_t arg5) {
    STRACE("keyctl(%u, %#x, %#x, %#x, %#x)", cmd, arg2, arg3, arg4, arg5);
    switch (cmd) {
        case KEYCTL_GET_KEYRING_ID_:
            return 1;
        case KEYCTL_JOIN_SESSION_KEYRING_:
            return 1;
        case KEYCTL_SETPERM_:
        case KEYCTL_SESSION_TO_PARENT_:
            return 0;
        default:
            return _ENOSYS;
    }
}

#define REBOOT_MAGIC1 0xfee1dead
#define REBOOT_MAGIC2 672274793
#define REBOOT_MAGIC2A 85072278
#define REBOOT_MAGIC2B 369367448
#define REBOOT_MAGIC2C 537993216

#define REBOOT_CMD_CAD_OFF 0
#define REBOOT_CMD_CAD_ON 0x89abcdef

int_t sys_reboot(int_t magic, int_t magic2, int_t cmd) {
    STRACE("reboot(%#x, %d, %d)", magic, magic2, cmd);
    if (!superuser())
        return _EPERM;
    if (magic != (int) REBOOT_MAGIC1 ||
            (magic2 != REBOOT_MAGIC2 &&
             magic2 != REBOOT_MAGIC2A &&
             magic2 != REBOOT_MAGIC2B &&
             magic2 != REBOOT_MAGIC2C))
        return _EINVAL;

    switch (cmd) {
        case REBOOT_CMD_CAD_ON:
        case REBOOT_CMD_CAD_OFF:
            return 0;
        default:
            return _EPERM;
    }
}
