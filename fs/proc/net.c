#include <sys/stat.h>
#include <arpa/inet.h>
#include <sys/sysctl.h>
#include <inttypes.h>
#include <string.h>
#include "kernel/calls.h"
#include "fs/proc.h"
#include "platform/platform.h"

#import <ifaddrs.h>
#import <netinet/in.h>
#import <sys/socket.h>
#import <unistd.h>
#import <net/if_var.h>

// Partially cribbed from https://github.com/ish-app/ish/pull/315/commits/4a3d96b4ed81470216534d299b921ba3c09ba03f#diff-8c3246e6b14ecb993cb4bf40b3d502a201566f225e339aa09cff57871f0d6351

#pragma mark - /proc/net

/*
 00000000000000000000000000000001 01 80 10 80       lo
 */
static int proc_show_if_inet6(struct proc_entry * UNUSED(entry), struct proc_data *buf) {
    struct ifaddrs *addrs;
    size_t needed; // How much buffer do we need to allocate
    // char *mybuf;
    
    int mib[] = {CTL_NET, PF_ROUTE, 0, AF_INET, NET_RT_FLAGS};
    // sysctl(int *name, u_int namelen, void *oldp, size_t *oldlenp, void *newp, size_t newlen);
    if (sysctl(mib, sizeof(mib) / sizeof(mib[0]), NULL, &needed, NULL, 0) < 0) {
       printk("error in route-sysctl-estimate");
       //return 0;
    } else {
       //printk("%s\n", )
    }
    
    bool success = (getifaddrs(&addrs) == 0);
    unsigned count = 0;
    if (success) {
        const struct ifaddrs *cursor = addrs;
        while (cursor != NULL) {
            //count++;
            if (cursor->ifa_addr->sa_family == AF_LINK) {
                proc_printf(buf, "00000000000000000000000000000001 %02x 80 10 80       %s\n", count++, cursor->ifa_name);
            }
            cursor = cursor->ifa_next;
        }
        freeifaddrs(addrs);
    }
    
    return 0;
}

static int proc_show_arp(struct proc_entry * UNUSED(entry), struct proc_data *buf) {
    /*
     IP address       HW type     Flags       HW address            Mask     Device
     10.211.55.1      0x1         0x2         00:1c:42:00:00:18     *        eth0
     */
    proc_printf(buf, "IP address       HW type     Flags       HW address            Mask     Device\n");
    proc_printf(buf, "192.168.1.1      0x1         0x2         00:BE:EF:CA:FE:00     *        en0\n");
    return 0;
}

static int proc_show_raw(struct proc_entry *UNUSED(entry), struct proc_data *UNUSED(buf)) {
    return 0;
}

static int proc_show_raw6(struct proc_entry *UNUSED(entry), struct proc_data *UNUSED(buf)) {
    return 0;
}

static int proc_show_tcp(struct proc_entry *UNUSED(entry), struct proc_data *UNUSED(buf)) {
    return 0;
}

static int proc_show_tcp6(struct proc_entry *UNUSED(entry), struct proc_data *UNUSED(buf)) {
    return 0;
}

static int proc_show_udp(struct proc_entry *UNUSED(entry), struct proc_data *UNUSED(buf)) {
    return 0;
}

static int proc_show_udp6(struct proc_entry *UNUSED(entry), struct proc_data *UNUSED(buf)) {
    return 0;
}


static int proc_show_route(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    
    proc_printf(buf, "Iface    Destination    Gateway     Flags    RefCnt    Use    Metric    Mask        MTU    Window    IRTT \n");
    
    struct ifaddrs *addrs;
    bool success = (getifaddrs(&addrs) == 0);
    if (success) {
        const struct ifaddrs *cursor = addrs;
        while (cursor != NULL) {
            int anything_but_loopback = strcmp(cursor->ifa_name, "lo0");
            if ((cursor->ifa_addr->sa_family == AF_LINK) && (anything_but_loopback)) {
                const struct if_data *stats = (struct if_data *)cursor->ifa_data;
                proc_printf(buf, "%-6.6s   %8.8d       %8.8x    %+4.4x     %1.1d         %3.3d    %8.8d  %8.8d    %1.1d   %1.1d         %1.1d\n",
                            cursor->ifa_name,
                            (unsigned long)0, // Destination
                            (unsigned long)0, // Gateway IP
                            cursor->ifa_flags,
                            (unsigned long)0, //RefCnt
                            (unsigned long)0, // Use
                            (unsigned long)stats->ifi_metric,
                            cursor->ifa_netmask,
                            (unsigned long)stats->ifi_mtu,
                            (unsigned long)0, // Window
                            (unsigned long)0  // IRTT
                            );
            }
            cursor = cursor->ifa_next;
        }
        freeifaddrs(addrs);
    }
    //proc_printf(buf, "eth0    00000000    0124A8C0    0003    0    0    202    00000000    0    0    0\n");
    //proc_printf(buf, "wlan0    00000000    0101A8C0    0003    0    0    303    00000000    0    0    0\n");
    //proc_printf(buf, "wlan0    0001A8C0    00000000    0001    0    0    303    00FFFFFF    0    0    0\n");
    //proc_printf(buf, "eth0    0024A8C0    00000000    0001    0    0    202    00FFFFFF    0    0    0\n");
    return 0;
}

static int proc_show_dev(struct proc_entry * UNUSED(entry), struct proc_data *buf) {
    proc_printf(buf, "Inter-|   Receive                            "
                 "                    |  Transmit\n"
                 " face |bytes    packets errs drop fifo frame "
                 "compressed multicast|bytes    packets errs "
                 "drop fifo colls carrier compressed\n");

    struct ifaddrs *addrs;
    bool success = (getifaddrs(&addrs) == 0);
    if (success) {
        const struct ifaddrs *cursor = addrs;
        while (cursor != NULL) {
            if (cursor->ifa_addr->sa_family == AF_LINK) {
              /*
               Inter-|   Receive                                                |  Transmit
                 face |bytes    packets errs drop fifo frame compressed multicast|bytes    packets errs drop fifo colls carrier compressed
                    lo:    4214      37    0    0    0     0          0         0     4214      37    0    0    0     0       0          0
                  eth0:  410911    6589    0    0    0     0          0       116   264679    4078    0    0    0     0       0          0
                 wlan0: 3014178    3267    0    0    0     0          0         2   126045    1205    0    0    0     0       0          0
               */
                const struct if_data *stats = (struct if_data *)cursor->ifa_data;
                
                if (stats != NULL) {
                    proc_printf(buf, "%6s:%8lu %7lu %4lu %4lu %4lu %5lu %10lu %9lu %8lu %7lu %4lu %4lu %4lu %5lu %7lu %10lu\n",
                                 cursor->ifa_name,
                                 (unsigned long)stats->ifi_ibytes,   // stats->rx_bytes,
                                 (unsigned long)stats->ifi_ipackets,   // stats->rx_packets,
                                 (unsigned long)stats->ifi_ierrors,  // stats->rx_errors,
                                 (unsigned long)0,  // stats->rx_dropped + stats->rx_missed_errors,
                                 (unsigned long)0,  // stats->rx_fifo_errors,
                                 (unsigned long)0,  // stats->rx_length_errors + stats->rx_over_errors +
                                 (unsigned long)0,  // stats->rx_crc_errors + stats->rx_frame_errors,
                                 (unsigned long)0,  // stats->rx_compressed,
                                 (unsigned long)stats->ifi_imcasts,  // stats->multicast,
                                 (unsigned long)stats->ifi_obytes,  // stats->tx_bytes,
                                 (unsigned long)stats->ifi_opackets,  // stats->tx_packets,
                                 (unsigned long)stats->ifi_oerrors,  // stats->tx_errors,
                                 (unsigned long)0,  // stats->tx_dropped,
                                 (unsigned long)0,  // stats->tx_fifo_errors,
                                 (unsigned long)stats->ifi_collisions,  // stats->collisions,
                                 (unsigned long)stats->ifi_ierrors + stats->ifi_oerrors,  // stats->tx_carrier_errors + stats->tx_aborted_errors +
                                 (unsigned long)0,  // stats->tx_window_errors + stats->tx_heartbeat_errors,
                                 (unsigned long)0
                                );  // stats->tx_compressed);
                } else {
                    proc_printf(buf, "%6.6s:%8lu %7lu %4lu %4lu %4lu %5lu %10lu %9lu %8lu %7lu %4lu %4lu %4lu %5lu %7lu %10lu\n",
                                 cursor->ifa_name,
                                 (unsigned long long)0,
                                 (unsigned long long)0,
                                 (unsigned long long)0,
                                 (unsigned long long)0,
                                 (unsigned long long)0,
                                 (unsigned long long)0,
                                 (unsigned long long)0,
                                 (unsigned long long)0,
                                 (unsigned long long)0,
                                 (unsigned long long)0,
                                 (unsigned long long)0,
                                 (unsigned long long)0,
                                 (unsigned long long)0,
                                 (unsigned long long)0,
                                 (unsigned long long)0,
                                 (unsigned long long)0);
                }
            }
            cursor = cursor->ifa_next;
        }
        freeifaddrs(addrs);
    }

    return 0;
}

#define PROC_NET_LEN sizeof(proc_net_entries) / sizeofproc_net_entries
/*
dr-xr-xr-x 5 root root 0 Jun  5 10:55 dev_snmp6
dr-xr-xr-x 3 root root 0 Jun  5 10:55 ipconfig
dr-xr-xr-x 3 root root 0 Jun  5 10:55 netfilter
dr-xr-xr-x 4 root root 0 Jun  5 10:55 nfsfs
dr-xr-xr-x 8 root root 0 Jun  5 10:55 rpc
dr-xr-xr-x 5 root root 0 Jun  5 10:55 stat
dr-xr-xr-x 3 root root 0 Jun  5 10:55 vlan
*/
static bool net_show_net_snmp6(struct proc_entry *UNUSED(entry), unsigned long *UNUSED(index), struct proc_entry *UNUSED(next_entry)) {
    return 0;
}

static bool net_show_ipconfig(struct proc_entry *UNUSED(entry), unsigned long *UNUSED(index), struct proc_entry *UNUSED(next_entry)) {
    return 0;
}

static bool net_show_netfilter(struct proc_entry *UNUSED(entry), unsigned long *UNUSED(index), struct proc_entry *UNUSED(next_entry)) {
    return 0;
}

static bool net_show_nfsfs(struct proc_entry *UNUSED(entry), unsigned long *UNUSED(index), struct proc_entry *UNUSED(next_entry)) {
    return 0;
}

static bool net_show_rpc(struct proc_entry *UNUSED(entry), unsigned long *UNUSED(index), struct proc_entry *UNUSED(next_entry)) {
    return 0;
}

static bool net_show_stat(struct proc_entry *UNUSED(entry), unsigned long *UNUSED(index), struct proc_entry *UNUSED(next_entry)) {
    return 0;
}

static bool net_show_unix(struct proc_entry *UNUSED(entry), unsigned long *UNUSED(index), struct proc_entry *UNUSED(next_entry)) {
    return 0;
}

static bool net_show_vlan(struct proc_entry *UNUSED(entry), unsigned long *UNUSED(index), struct proc_entry *UNUSED(next_entry)) {
    return 0;
}

struct proc_children proc_net_children = PROC_CHILDREN({
    {"arp", .show = proc_show_arp},
    {"ipconfig", S_IFDIR, .readdir = net_show_ipconfig},
    {"net_snmp6", S_IFDIR, .readdir = net_show_net_snmp6},
    {"netfilter", S_IFDIR, .readdir = net_show_netfilter},
    {"nfsfs", S_IFDIR, .readdir = net_show_nfsfs},
    {"raw", .show = proc_show_raw},
    {"raw6", .show = proc_show_raw6},
    {"rpc", S_IFDIR, .readdir = net_show_rpc},
    {"stat", S_IFDIR, .readdir = net_show_stat},
    {"tcp", .show = proc_show_tcp},
    {"tcp6", .show = proc_show_tcp6},
    {"udp", .show = proc_show_udp},
    {"udp6", .show = proc_show_udp6},
    {"unix", S_IFDIR, .readdir = net_show_unix},
    {"vlan", S_IFDIR, .readdir = net_show_vlan},
    //{"defaults",  .show = proc_show_dev},
    {"dev", .show = proc_show_dev},
    {"route", .show = proc_show_route },
    {"if_inet6", .show = proc_show_if_inet6 },
});
