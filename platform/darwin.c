#include <mach/mach.h>
#include <mach/mach_time.h>
#include <TargetConditionals.h>
#include <stdlib.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/mman.h>
#include "kernel/errno.h"
#include "platform/platform.h"
#include "debug.h"

typedef double CFTimeInterval;

extern bool doEnableMulticore;

struct cpu_usage get_total_cpu_usage(void) {
    host_cpu_load_info_data_t load;
    mach_msg_type_number_t fuck = HOST_CPU_LOAD_INFO_COUNT;
    host_statistics(mach_host_self(), HOST_CPU_LOAD_INFO, (host_info_t) &load, &fuck);
    struct cpu_usage usage;
    usage.user_ticks = load.cpu_ticks[CPU_STATE_USER];
    usage.system_ticks = load.cpu_ticks[CPU_STATE_SYSTEM];
    usage.idle_ticks = load.cpu_ticks[CPU_STATE_IDLE];
    usage.nice_ticks = load.cpu_ticks[CPU_STATE_NICE];
    return usage;
}

struct mem_usage get_mem_usage(void) {
    host_basic_info_data_t basic = {};
    mach_msg_type_number_t fuck = HOST_BASIC_INFO_COUNT;
    kern_return_t status = host_info(mach_host_self(), HOST_BASIC_INFO, (host_info_t) &basic, &fuck);
    assert(status == KERN_SUCCESS);
    vm_statistics64_data_t vm = {};
    fuck = HOST_VM_INFO64_COUNT;
    status = host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info_t) &vm, &fuck);
    assert(status == KERN_SUCCESS);

    struct mem_usage usage;
    usage.total = basic.max_mem;
    usage.free = vm.free_count * vm_page_size;
    usage.available = basic.memory_size;
    usage.cached = vm.speculative_count * vm_page_size;
    usage.active = vm.active_count * vm_page_size;
    usage.inactive = vm.inactive_count * vm_page_size;
    usage.wirecount = vm.wire_count * vm_page_size;
    usage.swapins = vm.swapins * vm_page_size;
    usage.swapouts = vm.swapouts * vm_page_size;
    return usage;
}

CFTimeInterval getSystemUptime(void) {
    enum { NANOSECONDS_IN_SEC = 1000 * 1000 * 1000 };
    static double multiply = 0;
    if (multiply == 0)
    {
        mach_timebase_info_data_t s_timebase_info;
        kern_return_t result = mach_timebase_info(&s_timebase_info);
        assert(result == 0);
        // multiply to get value in the nano seconds
        multiply = (double)s_timebase_info.numer / (double)s_timebase_info.denom;
        // multiply to get value in the seconds
        multiply /= NANOSECONDS_IN_SEC;
    }
    return mach_continuous_time() * multiply;
}

struct uptime_info get_uptime(void) {
    struct timeval kern_boottime;
    size_t size = sizeof(kern_boottime);
    if (sysctlbyname("kern.boottime", &kern_boottime, &size, NULL, 0) != 0) {
        printk("ERROR: in sysctlbyname(kern.boottime) call\n");
    }
    struct timeval now;
    if (gettimeofday(&now, NULL) != 0) {
        printk("ERROR: in gettimeofday() call\n");
    }
    extern time_t boot_time;  // Consider passing this as an argument

    struct {
        uint32_t ldavg[3];
        long scale;
    } vm_loadavg;
    size = sizeof(vm_loadavg);
    if (sysctlbyname("vm.loadavg", &vm_loadavg, &size, NULL, 0) != 0) {
        printk("ERROR: in sysctlbyname(vm.loadavg) call\n");
    }

    // Adjust the scale of load averages
    for (int i = 0; i < 3; i++) {
        if (FSHIFT < 16)
            vm_loadavg.ldavg[i] <<= 16 - FSHIFT;
        else
            vm_loadavg.ldavg[i] >>= FSHIFT - 16;
    }

    struct uptime_info uptime = {
        .uptime_ticks = (now.tv_sec - boot_time) * 100, // Ensure this calculation is as intended
        .load_1m = vm_loadavg.ldavg[0],
        .load_5m = vm_loadavg.ldavg[1],
        .load_15m = vm_loadavg.ldavg[2],
    };
    return uptime;
}

int get_cpu_count(void) {
     int ncpu = 1;
     size_t size = sizeof(int);
     sysctlbyname("hw.ncpu", &ncpu, &size, NULL, 0);
     const char *override = getenv("ISH_GUEST_CPU_COUNT");
     if (override != NULL && override[0] != '\0') {
         long forced = strtol(override, NULL, 10);
         if (forced > 0)
             ncpu = (int) forced;
     }
 #if TARGET_OS_OSX && defined(__aarch64__)
     // Standalone CLI / macOS dev harness: default to 4 emulated CPUs so local
     // and fakefs repro runs reproduce the concurrency -- and the TLB/COW/futex/
     // heap races -- of a multi-core device, instead of the old 2-core cap that
     // hid that whole class of bug. 4 exposes real parallelism without
     // oversubscribing a big host (this branch is macOS-only; the iOS app is not
     // TARGET_OS_OSX and keeps the true hw.ncpu). Override with
     // ISH_GUEST_CPU_COUNT=N (e.g. =6 to match a device, =1 to force serial).
     else
         ncpu = 4;
 #endif
     if (ncpu < 1)
         ncpu = 1;
     return ncpu;
}

// The number of CPUs to advertise to guest scheduler-sizing queries
// (sched_getaffinity / nproc), as opposed to the true core count reported by
// /proc/cpuinfo and /proc/stat. Multi-threaded guest workloads spawn one OS
// thread per "available" CPU -- e.g. the Go runtime sets GOMAXPROCS from
// sched_getaffinity, and `make -j$(nproc)` from nproc -- and under emulation
// running hw.ncpu such threads saturates every core, both starving the app UI
// and drowning the guest in lock/futex/TLB-shootdown overhead (Go actually
// compiles *faster* with fewer threads). On iOS we reserve roughly a third of
// the cores (at least one) so those programs leave headroom; /proc/cpuinfo
// still reports the true count, so htop and friends show all CPUs.
int get_cpu_count_for_affinity(void) {
    int ncpu = get_cpu_count();
#if TARGET_OS_IPHONE
    if (getenv("ISH_GUEST_CPU_COUNT") == NULL && ncpu > 2) {
        int reserve = ncpu / 3;
        if (reserve < 1)
            reserve = 1;
        ncpu -= reserve;
    }
#endif
    if (ncpu < 1)
        ncpu = 1;
    return ncpu;
}

int get_per_cpu_usage(struct cpu_usage** cpus_usage) {
    mach_msg_type_number_t info_size = sizeof(processor_cpu_load_info_t);
    processor_cpu_load_info_t sys_load_data = 0;
    natural_t ncpu;
    
    int err = host_processor_info(mach_host_self(), PROCESSOR_CPU_LOAD_INFO, &ncpu, (processor_info_array_t*)&sys_load_data, &info_size);
    if (err) {
        STRACE("Unable to get per cpu usage");
        return err;
    }
    
    struct cpu_usage* cpus_load_data = (struct cpu_usage*)calloc(ncpu, sizeof(struct cpu_usage));
    if (!cpus_load_data) {
        munmap(sys_load_data, vm_page_size);
        return _ENOMEM;
    }
    
    for (natural_t i = 0; i < ncpu; i++) {
        cpus_load_data[i].user_ticks = sys_load_data[i].cpu_ticks[CPU_STATE_USER];
        cpus_load_data[i].system_ticks = sys_load_data[i].cpu_ticks[CPU_STATE_SYSTEM];
        cpus_load_data[i].idle_ticks = sys_load_data[i].cpu_ticks[CPU_STATE_IDLE];
        cpus_load_data[i].nice_ticks = sys_load_data[i].cpu_ticks[CPU_STATE_NICE];
    }
    *cpus_usage = cpus_load_data;
    
    // Freeing cpu load information
    if (sys_load_data) {
        munmap(sys_load_data, vm_page_size);
    }
    
    return 0;
}
