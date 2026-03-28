#include <sys/stat.h>
#include <arpa/inet.h>
#include <sys/sysctl.h>
#include <inttypes.h>
#include "kernel/calls.h"
#include "fs/proc.h"
#include "platform/platform.h"
#include <sys/utsname.h>

void get_current_hostname(char *hostname, size_t size);

#import <ifaddrs.h>
#import <netinet/in.h>
#import <sys/socket.h>
#import <unistd.h>
#import <net/if_var.h>

extern const char *proc_ish_version;

#pragma mark - /proc/sys

static bool sys_show_abi(struct proc_entry *UNUSED(entry), unsigned long *UNUSED(index), struct proc_entry *UNUSED(next_entry)) {
    return 0;
}


static bool sys_show_dev(struct proc_entry *UNUSED(entry), unsigned long *UNUSED(index), struct proc_entry *UNUSED(next_entry)) {
    return 0;
}

static bool sys_show_fs(struct proc_entry *UNUSED(entry), unsigned long *UNUSED(index), struct proc_entry *UNUSED(next_entry)) {
    return 0;
}

static bool sys_show_fscache(struct proc_entry *UNUSED(entry), unsigned long *UNUSED(index), struct proc_entry *UNUSED(next_entry)) {
    return 0;
}

static bool sys_show_sunrpc(struct proc_entry *UNUSED(entry), unsigned long *UNUSED(index), struct proc_entry *UNUSED(next_entry)) {
    return 0;
}

static bool sys_show_user(struct proc_entry *UNUSED(entry), unsigned long *UNUSED(index), struct proc_entry *UNUSED(next_entry)) {
    return 0;
}

static bool sys_show_vm(struct proc_entry *UNUSED(entry), unsigned long *UNUSED(index), struct proc_entry *UNUSED(next_entry)) {
    return 0;
}

static bool proc_empty_dir_readdir(struct proc_entry *UNUSED(entry), unsigned long *UNUSED(index), struct proc_entry *UNUSED(next_entry)) {
    return false;
}

static int proc_binfmt_misc_status_show(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    proc_printf(buf, "enabled\n");
    return 0;
}

static int proc_binfmt_misc_empty_show(struct proc_entry *UNUSED(entry), struct proc_data *UNUSED(buf)) {
    return 0;
}

static int proc_binfmt_misc_noop_update(struct proc_entry *UNUSED(entry), struct proc_data *UNUSED(data)) {
    return 0;
}

static struct proc_dir_entry proc_binfmt_misc_entries[] = {
    {"register", S_IFREG | 0200, .show = proc_binfmt_misc_empty_show, .update = proc_binfmt_misc_noop_update},
    {"status", S_IFREG | 0644, .show = proc_binfmt_misc_status_show, .update = proc_binfmt_misc_noop_update},
};

#define PROC_BINFMT_MISC_LEN sizeof(proc_binfmt_misc_entries) / sizeof(proc_binfmt_misc_entries[0])

static bool proc_binfmt_misc_readdir(struct proc_entry *UNUSED(entry), unsigned long *index, struct proc_entry *next_entry) {
    if (*index < PROC_BINFMT_MISC_LEN) {
        *next_entry = (struct proc_entry) {&proc_binfmt_misc_entries[*index], *index, NULL, NULL, 0, 0};
        (*index)++;
        return true;
    }
    return false;
}

static struct proc_dir_entry proc_sys_fs_entries[] = {
    {"binfmt_misc", S_IFDIR, .readdir = proc_binfmt_misc_readdir},
};

#define PROC_SYS_FS_LEN sizeof(proc_sys_fs_entries) / sizeof(proc_sys_fs_entries[0])

static bool proc_sys_fs_readdir(struct proc_entry *UNUSED(entry), unsigned long *index, struct proc_entry *next_entry) {
    if (*index < PROC_SYS_FS_LEN) {
        *next_entry = (struct proc_entry) {&proc_sys_fs_entries[*index], *index, NULL, NULL, 0, 0};
        (*index)++;
        return true;
    }
    return false;
}

static bool sys_show_net_core(struct proc_entry *UNUSED(entry), unsigned long *UNUSED(index), struct proc_entry *UNUSED(next_entry)) {
    return 0;
}

static bool sys_show_net_ipv4(struct proc_entry *UNUSED(entry), unsigned long *UNUSED(index), struct proc_entry *UNUSED(next_entry)) {
    return 0;
}

static bool sys_show_net_ipv6(struct proc_entry *UNUSED(entry), unsigned long *UNUSED(index), struct proc_entry *UNUSED(next_entry)) {
    return 0;
}

static bool sys_show_net_netfilter(struct proc_entry *UNUSED(entry), unsigned long *UNUSED(index), struct proc_entry *UNUSED(next_entry)) {
    return 0;
}

static bool sys_show_net_unix(struct proc_entry *UNUSED(entry), unsigned long *UNUSED(index), struct proc_entry *UNUSED(next_entry)) {
    return 0;
}

static int sys_show_net_debug_exception_trace(struct proc_entry * UNUSED(entry), struct proc_data *buf) {
    proc_printf(buf, "%d\n", 0);
    
    return 0;
}

struct proc_dir_entry proc_sys_debug[] = {
    {"exception-trace", .show = sys_show_net_debug_exception_trace},
};

#define PROC_SYS_DEBUG_LEN sizeof(proc_sys_debug)/sizeof(proc_sys_debug[0])

static bool proc_sys_debug_readdir(struct proc_entry *UNUSED(entry), unsigned long *index, struct proc_entry *next_entry) {
    if (*index < PROC_SYS_DEBUG_LEN) {
        *next_entry = (struct proc_entry) {&proc_sys_debug[*index], *index, NULL, NULL, 0, 0};
        (*index)++;
        return true;
    }
    
    return false;
}

static int sys_show_net_unix_hostname(struct proc_entry * UNUSED(entry), struct proc_data *buf) {
    struct uname uts;
    char hostname[sizeof(uts.hostname)];
    get_current_hostname(hostname, sizeof(hostname));
    proc_printf(buf, "%s\n", hostname);
    return 0;
}

static int sys_show_net_version(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    proc_printf(buf, "%s\n", proc_ish_version);
    return 0;
}

static int sys_show_kernel_osrelease(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    struct uname uts;
    do_uname(&uts);
    proc_printf(buf, "%s\n", uts.release);
    return 0;
}

static int sys_show_kernel_cap_last_cap(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    // Keep this aligned with the advertised 4.20 kernel release.
    proc_printf(buf, "%d\n", 37);
    return 0;
}

struct proc_dir_entry proc_sys_kernel[] = {
    {"cap_last_cap", .show = sys_show_kernel_cap_last_cap},
    {"hostname", .show = sys_show_net_unix_hostname},
    {"osrelease", .show = sys_show_kernel_osrelease},
    {"version", .show = sys_show_net_version},
};

#define PROC_SYS_KERNEL_LEN sizeof(proc_sys_kernel)/sizeof(proc_sys_kernel[0])

static bool proc_sys_kernel_readdir(struct proc_entry *UNUSED(entry), unsigned long *index, struct proc_entry *next_entry) {
    if (*index < PROC_SYS_KERNEL_LEN) {
        *next_entry = (struct proc_entry) {&proc_sys_kernel[*index], *index, NULL, NULL, 0, 0};
        (*index)++;
        return true;
    }
    
    return false;
}

struct proc_dir_entry proc_sys_net[] = {
    {"core", S_IFDIR, .readdir = sys_show_net_core},
    {"ipv4", S_IFDIR, .readdir = sys_show_net_ipv4},
    {"ipv6", S_IFDIR, .readdir = sys_show_net_ipv6},
    {"netfilter", S_IFDIR, .readdir = sys_show_net_netfilter},
    {"unix", S_IFDIR, .readdir = sys_show_net_unix},
};

#define PROC_SYS_NET_LEN sizeof(proc_sys_net)/sizeof(proc_sys_net[0])

static bool proc_sys_net_readdir(struct proc_entry *UNUSED(entry), unsigned long *index, struct proc_entry *next_entry) {
    if (*index < PROC_SYS_NET_LEN) {
        *next_entry = (struct proc_entry) {&proc_sys_net[*index], *index, NULL, NULL, 0, 0};
        (*index)++;
        return true;
    }
    
    return false;
}

struct proc_dir_entry proc_net = {NULL, S_IFDIR, .readdir = proc_sys_net_readdir};

struct proc_children proc_sys_children = PROC_CHILDREN({
    {"abi", S_IFDIR, .readdir = sys_show_abi},
    {"debug", S_IFDIR, .readdir = proc_sys_debug_readdir},
    {"dev", S_IFDIR, .readdir = sys_show_dev},
    {"fs", S_IFDIR, .readdir = proc_sys_fs_readdir},
    {"fscache", S_IFDIR, .readdir = sys_show_fscache},
    {"kernel", S_IFDIR, .readdir = &proc_sys_kernel_readdir},
    {"net", S_IFDIR, .readdir = &proc_sys_net_readdir},
    {"sunrpc", S_IFDIR, .readdir = sys_show_sunrpc},
    {"user", S_IFDIR, .readdir = sys_show_user},
    {"vm", S_IFDIR, .readdir = sys_show_vm},
   //{"dev", .show = proc_show_dev},
});
