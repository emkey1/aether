#include <sys/stat.h>
#include <arpa/inet.h>
#include <sys/sysctl.h>
#include <inttypes.h>
#include "kernel/calls.h"
#include "fs/proc.h"
#include "platform/platform.h"
#include <sys/utsname.h>

#import <ifaddrs.h>
#import <netinet/in.h>
#import <sys/socket.h>
#import <unistd.h>
#import <net/if_var.h>

extern const char *proc_ish_version;

#pragma mark - /proc/sys

static void get_child_names(struct proc_entry *entry, unsigned long index) {
    if (index == 0 || entry->child_names == NULL) {
        if (entry->child_names != NULL)
            free_string_array(entry->child_names);
        //entry->child_names = get_all_defaults_keys();
    }
}
static bool sys_show_abi(struct proc_entry *entry, unsigned long *index, struct proc_entry *next_entry) {
    return 0;
}

static bool sys_show_debug(struct proc_entry *entry, unsigned long *index, struct proc_entry *next_entry) {
    return 0;
}

static bool sys_show_dev(struct proc_entry *entry, unsigned long *index, struct proc_entry *next_entry) {
    return 0;
}

static bool sys_show_fs(struct proc_entry *entry, unsigned long *index, struct proc_entry *next_entry) {
    return 0;
}

static bool sys_show_fscache(struct proc_entry *entry, unsigned long *index, struct proc_entry *next_entry) {
    return 0;
}

static bool sys_show_kernel(struct proc_entry *entry, unsigned long *index, struct proc_entry *next_entry) {
    return 0;
}

static bool sys_show_net(struct proc_entry *entry, unsigned long *index, struct proc_entry *next_entry) {
    get_child_names(entry, *index);
    char *friendly_name = NULL;
    do {
        const char *name = entry->child_names[*index];
        if (name == NULL)
            return false;
        (*index)++;
    } while (friendly_name == NULL);
    next_entry->name = friendly_name;
    return true;
    return 0;
}

static bool sys_show_sunrpc(struct proc_entry *entry, unsigned long *index, struct proc_entry *next_entry) {
    return 0;
}

static bool sys_show_user(struct proc_entry *entry, unsigned long *index, struct proc_entry *next_entry) {
    return 0;
}

static bool sys_show_vm(struct proc_entry *entry, unsigned long *index, struct proc_entry *next_entry) {
    return 0;
}

static bool sys_show_net_core(struct proc_entry *entry, unsigned long *index, struct proc_entry *next_entry) {
    return 0;
}

static bool sys_show_net_ipv4(struct proc_entry *entry, unsigned long *index, struct proc_entry *next_entry) {
    return 0;
}

static bool sys_show_net_ipv6(struct proc_entry *entry, unsigned long *index, struct proc_entry *next_entry) {
    return 0;
}

static bool sys_show_net_netfilter(struct proc_entry *entry, unsigned long *index, struct proc_entry *next_entry) {
    return 0;
}

static bool sys_show_net_unix(struct proc_entry *entry, unsigned long *index, struct proc_entry *next_entry) {
    return 0;
}

static int sys_show_net_unix_hostname(struct proc_entry * UNUSED(entry), struct proc_data *buf) {
    struct utsname real_uname;
    uname(&real_uname);
    
    proc_printf(buf, "%s\n", real_uname.nodename);
    return 0;
}

static int sys_show_net_version(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    proc_printf(buf, "%s\n", proc_ish_version);
    return 0;
}

struct proc_dir_entry proc_sys_kernel[] = {
    {"hostname", .show = sys_show_net_unix_hostname},
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

/*
 dr-xr-xr-x 1 root root 0 Jun  4 22:45 core
 dr-xr-xr-x 1 root root 0 Jun  3 23:49 ipv4
 dr-xr-xr-x 1 root root 0 Jun  3 23:49 ipv6
 dr-xr-xr-x 1 root root 0 Jun  4 22:45 netfilter
 dr-xr-xr-x 1 root root 0 Jun  4 22:45 unix
 */
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
    {"debug", S_IFDIR, .readdir = sys_show_debug},
    {"dev", S_IFDIR, .readdir = sys_show_dev},
    {"fs", S_IFDIR, .readdir = sys_show_fs},
    {"fscache", S_IFDIR, .readdir = sys_show_fscache},
    {"kernel", S_IFDIR, .readdir = &proc_sys_kernel_readdir},
    {"net", S_IFDIR, .readdir = &proc_sys_net_readdir},
    {"sunrpc", S_IFDIR, .readdir = sys_show_sunrpc},
    {"user", S_IFDIR, .readdir = sys_show_user},
    {"vm", S_IFDIR, .readdir = sys_show_vm},
   //{"dev", .show = proc_show_dev},
});

