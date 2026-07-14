#include <sys/stat.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __APPLE__
#include <mach/mach.h>
#endif
#include "kernel/calls.h"
#include "kernel/task.h"
#include "fs/proc.h"
#include "fs/proc/net.h"
#include "fs/devices.h"
#include "fs/real.h"
#include "platform/platform.h"
#include <sys/param.h> // for MIN and MAX
#include "emu/cpuid.h"
#include "kernel/abi.h"
#include "kernel/init.h"
#include "kernel/hostinfo.h"

extern int console_major;
extern int console_minor;

char ish_boot_command_line[4096];

static int proc_show_version(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    struct uname uts;
    do_uname(&uts);
    proc_printf(buf, "%s version %s %s\n", uts.system, uts.release, uts.version);
    return 0;
}

static size_t append_flag(char *buf, size_t size, size_t offset, const char *flag) {
    size_t len = strlen(flag);
    if (offset + len + 1 >= size)
        return offset;
    memcpy(buf + offset, flag, len);
    offset += len;
    buf[offset++] = ' ';
    buf[offset] = '\0';
    return offset;
}

static void append_cpuid_leaf_flags(char *buf, size_t size, dword_t bits,
                                    const char *const names[32]) {
    size_t offset = strlen(buf);
    for (size_t i = 0; i < 32; i++) {
        if (!(bits & (1u << i)) || names[i] == NULL || names[i][0] == '\0')
            continue;
        offset = append_flag(buf, size, offset, names[i]);
    }
}

static void append_cpuid_flags(char *buf, size_t size, dword_t ecx, dword_t edx,
                               const char *const ecx_names[32],
                               const char *const edx_names[32]) {
    append_cpuid_leaf_flags(buf, size, edx, edx_names);
    append_cpuid_leaf_flags(buf, size, ecx, ecx_names);
}

static void format_cpuid_flags(char *buf, size_t size) {
    static const char *const leaf1_edx_names[32] = {
        "fpu", "vme", "de", "pse", "tsc", "msr", "pae", "mce",
        "cx8", "apic", NULL, "sep", "mtrr", "pge", "mca", "cmov",
        NULL, "pse36", "pn", "clflush", NULL, "dts", "acpi", "mmx",
        "fxsr", "sse", "sse2", "ss", "ht", "tm", NULL, "pbe",
    };
    static const char *const leaf1_ecx_names[32] = {
        "pni", "pclmulqdq", "dtes64", "monitor", "ds_cpl", "vmx", "smx",
        "est", "tm2", "ssse3", "cid", "sdbg", "fma", "cx16", "xtpr",
        "pdcm", NULL, "pcid", "dca", "sse4_1", "sse4_2", "x2apic",
        "movbe", "popcnt", "tsc_deadline_timer", "aes", "xsave", "osxsave",
        "avx", "f16c", "rdrand", "hypervisor",
    };
    static const char *const ext_edx_names[32] = {
        "fpu", "vme", "de", "pse", "tsc", "msr", "pae", "mce",
        "cx8", "apic", NULL, "syscall", "mtrr", "pge", "mca", "cmov",
        "pat", "pse36", NULL, NULL, "nx", NULL, "mmxext", "mmx",
        "fxsr", "fxsr_opt", "pdpe1gb", "rdtscp", NULL, "lm", "3dnowext", "3dnow",
    };
    static const char *const ext_ecx_names[32] = {
        "lahf_lm", "cmp_legacy", "svm", "extapic", "cr8_legacy", "abm",
        "sse4a", "misalignsse", "3dnowprefetch", "osvw", "ibs", "xop",
        "skinit", "wdt", NULL, "lwp", "fma4", "tce", NULL, "nodeid_msr",
        NULL, "tbm", "topoext", "perfctr_core", "perfctr_nb", NULL,
        "bpext", "ptsc", "perfctr_llc", "mwaitx", NULL, NULL,
    };
    dword_t eax = 1, ebx = 0, ecx = 0, edx = 0;

    buf[0] = '\0';
    do_cpuid(&eax, &ebx, &ecx, &edx);
    append_cpuid_flags(buf, size, ecx, edx, leaf1_ecx_names, leaf1_edx_names);

    eax = 0x80000000u;
    do_cpuid(&eax, &ebx, &ecx, &edx);
    if (eax >= 0x80000001u) {
        eax = 0x80000001u;
        do_cpuid(&eax, &ebx, &ecx, &edx);
        append_cpuid_flags(buf, size, ecx, edx, ext_ecx_names, ext_edx_names);
    }
}

static void unpack32(dword_t src, void *dst) {
    unsigned char *p = dst;
    for (size_t i = 0; i < 4; i++) {
        p[i] = (src >> (0x08 * i)) & 0xff;
    }
}

void translate_vendor_id(char *buf, dword_t *ebx, dword_t *ecx, dword_t *edx) {
    unpack32(*ebx, &buf[0]);
    unpack32(*edx, &buf[4]);
    unpack32(*ecx, &buf[8]);
}

static int proc_show_cpuinfo(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    enum guest_abi abi = current != NULL ? current->abi : GUEST_ABI_I386;
    struct guest_abi_desc abi_desc = guest_abi_desc(abi);
    dword_t eax = 0;
    dword_t ebx;
    dword_t ecx;
    dword_t edx;

    // arm64 guests get the aarch64 cpuinfo format (implementer/part/
    // Features), not the x86 one — tools parse "Features" for the same
    // capabilities the emulated ID registers advertise (gen.c's
    // ID_AA64ISAR0 value: aes pmull sha1 sha2 crc32) and "vendor_id :
    // GenuineIntel" on an aarch64 root is nonsense. The iSH-AOK "host
    // ..." extension lines are kept, matching the x86 format below.
    if (abi == GUEST_ABI_ARM64) {
        char *arm_host_architecture = copyHostArchitecture();
        char *arm_host_machine_identifier = copyHostMachineIdentifier();
        char *arm_host_device_name = copyHostDeviceName();
        char *arm_host_core_topology = copyHostCoreTopology();
        int arm_cpu_count = get_cpu_count();
        for (int cpu = 0; cpu < arm_cpu_count; cpu++) {
            proc_printf(buf, "processor       : %d\n", cpu);
            proc_printf(buf, "model name      : iSH Virtual aarch64-compatible CPU @ 1.066GHz\n");
            proc_printf(buf, "BogoMIPS        : 1066.00\n");
            // Real Linux derives this string from AT_HWCAP — keep it in
            // lockstep with kernel/exec.c's hwcap (uniform on every host
            // device: SHA512/CRC32 fall back to soft gadgets pre-A13/A10).
            proc_printf(buf, "Features        : fp asimd cpuid aes pmull sha1 sha2 crc32 atomics sha3 sha512\n");
            proc_printf(buf, "CPU implementer : 0x61\n"); // Apple (the silicon underneath)
            proc_printf(buf, "CPU architecture: 8\n");
            proc_printf(buf, "CPU variant     : 0x0\n");
            proc_printf(buf, "CPU part        : 0x023\n");
            proc_printf(buf, "CPU revision    : 1\n");
            proc_printf(buf, "host arch       : %s\n", arm_host_architecture);
            proc_printf(buf, "host machine    : %s\n", arm_host_machine_identifier);
            proc_printf(buf, "host device     : %s\n", arm_host_device_name);
            proc_printf(buf, "host cores      : %s\n", arm_host_core_topology);
            proc_printf(buf, "\n");
        }
        free(arm_host_architecture);
        free(arm_host_machine_identifier);
        free(arm_host_device_name);
        free(arm_host_core_topology);
        return 0;
    }

    // riscv64 guests get the riscv cpuinfo format. Parsers key on the
    // "isa" line; keep it in lockstep with kernel/exec.c's AT_HWCAP
    // (rv64imafdc = exactly what the JIT implements; no V, no bitmanip)
    // and the Sv39 layout from guest_abi_vm_layout.
    if (abi == GUEST_ABI_RISCV64) {
        char *rv_host_architecture = copyHostArchitecture();
        char *rv_host_machine_identifier = copyHostMachineIdentifier();
        char *rv_host_device_name = copyHostDeviceName();
        char *rv_host_core_topology = copyHostCoreTopology();
        int rv_cpu_count = get_cpu_count();
        for (int cpu = 0; cpu < rv_cpu_count; cpu++) {
            proc_printf(buf, "processor       : %d\n", cpu);
            proc_printf(buf, "hart            : %d\n", cpu);
            proc_printf(buf, "isa             : rv64imafdc\n");
            proc_printf(buf, "mmu             : sv39\n");
            proc_printf(buf, "uarch           : ish-aok,jit\n");
            proc_printf(buf, "host arch       : %s\n", rv_host_architecture);
            proc_printf(buf, "host machine    : %s\n", rv_host_machine_identifier);
            proc_printf(buf, "host device     : %s\n", rv_host_device_name);
            proc_printf(buf, "host cores      : %s\n", rv_host_core_topology);
            proc_printf(buf, "\n");
        }
        free(rv_host_architecture);
        free(rv_host_machine_identifier);
        free(rv_host_device_name);
        free(rv_host_core_topology);
        return 0;
    }

    do_cpuid(&eax, &ebx, &ecx, &edx); // Get vendor_id

    char vendor_id[13] = { 0 };
    translate_vendor_id(vendor_id, &ebx, &ecx, &edx);
    dword_t cpuid_level = eax;

    eax = 1;
    do_cpuid(&eax, &ebx, &ecx, &edx);

    char cpu_flags[512] = { 0 };
    format_cpuid_flags(cpu_flags, sizeof(cpu_flags));
    char *host_architecture = copyHostArchitecture();
    char *host_machine_identifier = copyHostMachineIdentifier();
    char *host_device_name = copyHostDeviceName();
    char *host_core_topology = copyHostCoreTopology();

    int cpu_count = get_cpu_count(); // One entry per device processor
    int clflush_size = ((ebx >> 8) & 0xff) * 8;
    if (clflush_size == 0)
        clflush_size = 64;
    int i;

    for( i=0; i<cpu_count ; i++ ) {
        proc_printf(buf, "processor       : %d\n",i);
        proc_printf(buf, "vendor_id       : %s\n", vendor_id);
        proc_printf(buf, "cpu family      : %d\n", guest_abi_is_64bit(abi) ? 6 : 1);
        proc_printf(buf, "model           : %d\n", guest_abi_is_64bit(abi) ? 85 : 1);
        proc_printf(buf, "model name      : iSH Virtual %s-compatible CPU @ 1.066GHz\n",
                    abi_desc.uname_machine);
        proc_printf(buf, "stepping        : %d\n",1);
        proc_printf(buf, "CPU MHz         : 1066.00\n");
        proc_printf(buf, "cache size      : %d kb\n",0);
        proc_printf(buf, "physical id     : %d\n",0);
        proc_printf(buf, "siblings        : %d\n",cpu_count);
        proc_printf(buf, "core id         : %d\n",i);
        proc_printf(buf, "cpu cores       : %d\n",cpu_count);
        proc_printf(buf, "apicid          : %d\n",i);
        proc_printf(buf, "initial apicid  : %d\n",i);
        proc_printf(buf, "fpu             : yes\n");
        proc_printf(buf, "fpu_exception   : yes\n");
        proc_printf(buf, "cpuid level     : %u\n", cpuid_level);
        proc_printf(buf, "wp              : yes\n");
        proc_printf(buf, "flags           : %s\n", cpu_flags);
        proc_printf(buf, "host arch       : %s\n", host_architecture);
        proc_printf(buf, "host machine    : %s\n", host_machine_identifier);
        proc_printf(buf, "host device     : %s\n", host_device_name);
        proc_printf(buf, "host cores      : %s\n", host_core_topology);
        proc_printf(buf, "bogomips        : 1066.00\n");
        proc_printf(buf, "clflush size    : %d\n", clflush_size);
        proc_printf(buf, "cache_alignment : %d\n",64);
        proc_printf(buf, "address sizes   : 36 bits physical, %d bits virtual\n",
                    guest_abi_is_64bit(abi) ? 48 : 32);
        proc_printf(buf, "power management:\n");
        proc_printf(buf, "\n");
    }

    free(host_architecture);
    free(host_machine_identifier);
    free(host_device_name);
    free(host_core_topology);

    return 0;
}

static int proc_show_cmdline(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    proc_printf(buf, "%s", ish_boot_command_line);
    // Real kernels advertise the console on the kernel command line, and tools
    // like bootlogd deduce the console device from it. iSH has no bootloader, so
    // synthesize a console= entry from the active console (matching the device
    // reported by /proc/consoles) when the command line doesn't already have one.
    if (strstr(ish_boot_command_line, "console=") == NULL) {
        const char *sep = ish_boot_command_line[0] != '\0' ? " " : "";
        if (console_major == TTY_CONSOLE_MAJOR)
            proc_printf(buf, "%sconsole=tty%d", sep, console_minor);
        else if (console_major == TTY_PSEUDO_SLAVE_MAJOR)
            proc_printf(buf, "%sconsole=pts/%d", sep, console_minor);
    }
    proc_printf(buf, "\n");
    return 0;
}

static int proc_show_consoles(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    if (console_major == TTY_CONSOLE_MAJOR) {
        proc_printf(buf, "tty%d                 -WU (E  ) %d:%d\n", console_minor, console_major, console_minor);
    } else if (console_major == TTY_PSEUDO_SLAVE_MAJOR) {
        proc_printf(buf, "pts/%d                -WU (E  ) %d:%d\n", console_minor, console_major, console_minor);
    } else {
        proc_printf(buf, "console               -WU (E  ) %d:%d\n", console_major, console_major, console_minor);
    }
    return 0;
}

static int proc_show_stat(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    int ncpus = get_cpu_count();
    struct cpu_usage total_usage = get_total_cpu_usage();
    struct cpu_usage* per_cpu_usage = 0;
    
    proc_printf(buf, "cpu  %"PRIu64" %"PRIu64" %"PRIu64" %"PRIu64" 0 0 0 0\n", total_usage.user_ticks, total_usage.nice_ticks, total_usage.system_ticks, total_usage.idle_ticks);
    
    int err = get_emulated_per_cpu_usage(&per_cpu_usage);
    if (!err) {
        for (int i = 0; i < ncpus; i++) {
            proc_printf(buf, "cpu%d  %"PRIu64" %"PRIu64" %"PRIu64" %"PRIu64" 0 0 0 0\n", i, per_cpu_usage[i].user_ticks, per_cpu_usage[i].nice_ticks, per_cpu_usage[i].system_ticks, per_cpu_usage[i].idle_ticks);
        }
        free(per_cpu_usage);
    }
    proc_printf(buf, "intr 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n");
    
    
    int blocked_task_count = get_count_of_blocked_tasks();
    int alive_task_count = get_count_of_alive_tasks();

    // Every Linux "process" iSH emulates is a pthread inside this one host
    // process, so the host process's own context-switch count (Mach
    // TASK_EVENTS_INFO.csw) is a faithful stand-in for the guest-system-wide
    // ctxt counter -- there's no equivalent for real hardware interrupts or
    // softirqs (see below), but this one has a real host-side source.
    uint64_t ctxt = 0;
#ifdef __APPLE__
    struct task_events_info task_events;
    mach_msg_type_number_t task_events_count = TASK_EVENTS_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_EVENTS_INFO,
                  (task_info_t) &task_events, &task_events_count) == KERN_SUCCESS)
        ctxt = task_events.csw;
#endif
    proc_printf(buf, "ctxt %"PRIu64"\n", ctxt);
    struct uptime_info btime = get_uptime();
    struct timespec uptime_ts = {.tv_sec = btime.uptime_ticks / 100, .tv_nsec = btime.uptime_ticks % 100};
    struct timespec boot_time = timespec_subtract(timespec_now(CLOCK_REALTIME), uptime_ts);
    proc_printf(buf, "btime %ld\n", boot_time.tv_sec);
    proc_printf(buf, "processes %d\n", alive_task_count);
    proc_printf(buf, "procs_running %d\n", alive_task_count - blocked_task_count);
    proc_printf(buf, "procs_blocked %d\n", blocked_task_count);
    proc_printf(buf, "softirq 0 0 0 0 0 0 0 0 0 0 0\n");
    
    return 0;
}

static void show_kb(struct proc_data *buf, const char *name, uint64_t value) {
    proc_printf(buf, "%s%8"PRIu64" kB\n", name, value / 1000);
}

struct mem_page_class_totals {
    uint64_t anon_bytes;
    uint64_t shmem_bytes;
    uint64_t mapped_bytes;
};

// Sums each live address space's own page-table flags into anon/shmem/mapped
// byte counts -- this is real guest-memory accounting (unlike the host-only
// figures in get_mem_usage()), just not previously aggregated anywhere. One
// entry per thread-group leader: threads created with CLONE_THREAD always
// share their leader's struct mm (CLONE_VM is required alongside it, see
// kernel/fork.c), so summing leaders avoids counting the same address space
// once per thread. A bare CLONE_VM without CLONE_THREAD would be missed, but
// nothing in this codebase creates that combination.
static void collect_mem_page_stats(struct mem_page_class_totals *out) {
    memset(out, 0, sizeof(*out));

    struct task_snapshot snapshot = {0};
    if (task_snapshot_collect(&snapshot, true) < 0)
        return;

    for (unsigned i = 0; i < snapshot.count; i++) {
        struct task *task = snapshot.tasks[i];
        // task_snapshot_collect() only checks task->exiting at snapshot time
        // (under pids_lock, skipping tasks already exiting then) -- it can't
        // stop a task from starting to exit afterward, while this loop is
        // still working through the rest of the snapshot. If that happens,
        // do_exit() sets task->exiting, locks general_lock, and then waits
        // for exit_wait_needed() to clear -- which this snapshot's own
        // reference (task_snapshot_collect's task_ref_cnt_mod(task, 1)) keeps
        // from ever being true, since it isn't dropped until
        // task_snapshot_release() at the end of this function. A blocking
        // lock() here would then wait forever for a general_lock that will
        // never be released until this very call finishes -- a genuine
        // deadlock, not just contention (confirmed via a live sample: a
        // /proc/meminfo reader stuck in mylock() here while the task it was
        // locking sat in do_exit's exit_wait_backoff loop). Use the same
        // trylock-and-skip pattern proc_entry_stat() already uses for this
        // exact class of race: a task we can't immediately lock is mid-exit
        // and about to disappear, so skipping its contribution to the totals
        // is harmless.
        if (trylock(&task->general_lock) != 0)
            continue;
        struct mm *mm = task->mm;
        if (mm != NULL)
            mm_retain(mm);
        unlock(&task->general_lock);
        if (mm == NULL)
            continue;

        struct mem *mem = &mm->mem;
        mem_read_lock_quiesce_aware(mem);
        page_t page = 0;
        while (page < mem->page_limit) {
            struct pt_entry *pt = mem_pt(mem, page);
            if (pt != NULL) {
                if (pt->flags & P_ANONYMOUS) {
                    if (pt->flags & P_SHARED)
                        out->shmem_bytes += PAGE_SIZE;
                    else
                        out->anon_bytes += PAGE_SIZE;
                } else {
                    out->mapped_bytes += PAGE_SIZE;
                }
            }
            mem_next_page(mem, &page);
        }
        mem_read_unlock_quiesce_aware(mem);
        mm_release(mm);
    }

    task_snapshot_release(&snapshot);
}

static int proc_show_filesystems(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    char *filesystems = get_filesystems();
    proc_printf(buf, "%s", filesystems);
    free(filesystems);
    return 0;
}

static int proc_show_meminfo(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
/*
Active(anon):       2508 kB
Inactive(anon):    14628 kB
Active(file):     395460 kB
Inactive(file):   387664 kB
Unevictable:           0 kB
Mlocked:               0 kB
SwapTotal:        524284 kB
SwapFree:         515580 kB
Dirty:              1260 kB
Writeback:             0 kB
AnonPages:         16408 kB
KReclaimable:      49756 kB
SReclaimable:      49756 kB
SUnreclaim:        30112 kB
KernelStack:        1456 kB
PageTables:         1344 kB
NFS_Unstable:          0 kB
Bounce:                0 kB
WritebackTmp:          0 kB
CommitLimit:     1024300 kB
Committed_AS:     119020 kB
VmallocTotal:   34359738367 kB
VmallocUsed:       25768 kB
VmallocChunk:          0 kB
Percpu:             1432 kB
HardwareCorrupted:     0 kB
AnonHugePages:      4096 kB
ShmemHugePages:        0 kB
ShmemPmdMapped:        0 kB
FileHugePages:         0 kB
FilePmdMapped:         0 kB
HugePages_Total:       0
HugePages_Free:        0
HugePages_Rsvd:        0
HugePages_Surp:        0
Hugepagesize:       2048 kB
Hugetlb:               0 kB
DirectMap4k:      108392 kB
DirectMap2M:      940032 kB
DirectMap1G:           0 kB
*/
    struct mem_usage usage = get_mem_usage();
    struct mem_page_class_totals page_totals;
    collect_mem_page_stats(&page_totals);
    show_kb(buf, "MemTotal:       ", usage.total);
    show_kb(buf, "MemFree:        ", usage.free);
    show_kb(buf, "MemAvailable:   ", usage.available);
    show_kb(buf, "Buffers:        ", 0);
    show_kb(buf, "Cached:         ", usage.cached);
    show_kb(buf, "MemShared:      ", usage.free);
    show_kb(buf, "Active:         ", usage.active);
    show_kb(buf, "Inactive:       ", usage.inactive);
    show_kb(buf, "SwapCached:     ", 0);
    // Buffers/Dirty/Writeback/Slab have no honest analog: iSH has no Linux-style
    // unified page cache it controls, and no kernel slab allocator to account for.
    show_kb(buf, "Shmem:          ", page_totals.shmem_bytes);
    show_kb(buf, "SwapTotal:      ", 0);
    show_kb(buf, "SwapFree:       ", 0);
    show_kb(buf, "Dirty:          ", 0);
    show_kb(buf, "Writeback:      ", 0);
    show_kb(buf, "AnonPages:      ", page_totals.anon_bytes);
    show_kb(buf, "Mapped:         ", page_totals.mapped_bytes);
    show_kb(buf, "Slab:           ", 0);
    // Stuff that doesn't map elsehwere
    show_kb(buf, "Swapins:        ", usage.swapins);
    show_kb(buf, "Swapouts:       ", usage.swapouts);
    show_kb(buf, "WireCount:      ", usage.wirecount);
    return 0;
}

static int proc_show_uptime(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    struct uptime_info uptime_info = get_uptime();
    unsigned long uptime = uptime_info.uptime_ticks;
    
    proc_printf(buf, "%lu.%lu %lu.%lu\n", uptime / 100, uptime % 100, uptime / 100, uptime % 100);
    return 0;
}
static int proc_show_vmstat(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    // procps-ng's vmstat command treats a zero-byte /proc/vmstat as a read
    // error ("Unable to create vmstat structure") rather than an empty stat
    // set, since it has no notion of "file exists but no fields apply" -- so
    // this must always emit at least one line. Field values are best-effort;
    // procps parses them into a hash table and only complains about a wholly
    // empty file, not about missing individual keys.
    struct mem_usage usage = get_mem_usage();
    proc_printf(buf, "nr_free_pages %"PRIu64"\n", usage.free / 4096);
    proc_printf(buf, "nr_active_anon %"PRIu64"\n", usage.active / 4096);
    proc_printf(buf, "nr_inactive_anon %"PRIu64"\n", usage.inactive / 4096);
    proc_printf(buf, "nr_active_file 0\n");
    proc_printf(buf, "nr_inactive_file 0\n");
    // pgpgin/pgpgout are in KiB (the kernel's unit for these two), fed by the
    // per-task I/O accounting in kernel/fs.c. iotop's "Total DISK READ/WRITE"
    // header derives from the delta between reads of these.
    proc_printf(buf, "pgpgin %llu\n", (unsigned long long)
            (atomic_load_explicit(&io_disk_read_bytes, memory_order_relaxed) / 1024));
    proc_printf(buf, "pgpgout %llu\n", (unsigned long long)
            (atomic_load_explicit(&io_disk_write_bytes, memory_order_relaxed) / 1024));
    proc_printf(buf, "pswpin %"PRIu64"\n", usage.swapins / 4096);
    proc_printf(buf, "pswpout %"PRIu64"\n", usage.swapouts / 4096);
    proc_printf(buf, "pgfault 0\n");
    proc_printf(buf, "pgmajfault 0\n");
    return 0;
}
/*
 8       0 sda 52553 537 6661171 8035 394441 324883 29295529 405166 0 111828 240028 0 0 0 0
 8       1 sda1 421 0 9657 21 2 0 9 0 0 16 20 0 0 0 0
 8       2 sda2 51958 537 6642610 7999 392133 324883 29295520 405043 0 111804 239984 0 0 0 0
 8       3 sda3 70 0 4592 6 0 0 0 0 0 8 12 0 0 0 0
11       0 sr0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0
 */
static int proc_show_diskstats(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    // "disk1" aggregates every real read/write/pread/pwrite (fakefs, realfs,
    // and iosfs data all funnel through fs/real.c's realfs_io_stats -- see
    // realfs_count_read/realfs_count_write). Merge counts, I/O time, and
    // in-progress/weighted-time fields aren't tracked, so those report 0
    // rather than a fabricated value.
    uint64_t read_ops = atomic_load_explicit(&realfs_io_stats.read_ops, memory_order_relaxed);
    uint64_t read_sectors = atomic_load_explicit(&realfs_io_stats.read_bytes, memory_order_relaxed) / 512;
    uint64_t write_ops = atomic_load_explicit(&realfs_io_stats.write_ops, memory_order_relaxed);
    uint64_t write_sectors = atomic_load_explicit(&realfs_io_stats.write_bytes, memory_order_relaxed) / 512;
    proc_printf(buf, "8       0 disk1 %"PRIu64" 0 %"PRIu64" 0 %"PRIu64" 0 %"PRIu64" 0 0 0 0\n",
                read_ops, read_sectors, write_ops, write_sectors);
    return 0;
}

static int proc_show_loadavg(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    uint64_t loads[3];
    get_guest_loadavg(loads);
    struct pid *last_pid = pid_get_last_allocated();
    int last_pid_id = last_pid ? last_pid->id : 0;
    double load_1m = loads[0] / 65536.0;
    double load_5m = loads[1] / 65536.0;
    double load_15m = loads[2] / 65536.0;
    int blocked_task_count = get_count_of_blocked_tasks();
    int alive_task_count = get_count_of_alive_tasks();
    // running_task_count is calculated abool proc_net_readdir(struct proc_entry * UNUSED(entry), unsigned long *index, struct proc_entry *next_entry) pproximetly, since we don't know the real number of currently running tasks.
    int running_task_count = MIN(get_cpu_count(), (int)(alive_task_count - blocked_task_count));
    proc_printf(buf, "%.2f %.2f %.2f %u/%u %u\n", load_1m, load_5m, load_15m, running_task_count, alive_task_count, last_pid_id);
    return 0;
}

static int proc_readlink_self(struct proc_entry *UNUSED(entry), char *buf) {
    // Linux's /proc/self points at the bare pid ("123"), with no trailing slash.
    // The path resolver appends "/rest" itself, so the slash was never needed and
    // only corrupted readlink("/proc/self") for callers that parse the pid.
    snprintf(buf, MAX_PATH, "%d", current->pid);
    return 0;
}

static void proc_print_escaped(struct proc_data *buf, const char *str) {
    for (size_t i = 0; str[i]; i++) {
        switch (str[i]) {
            case '\t': case ' ': case '\\':
                proc_printf(buf, "\\%03o", str[i]);
                break;
            default:
                proc_printf(buf, "%c", str[i]);
        }
    }
}

#define proc_printf_comma(buf, at_start, format, ...) do { \
    proc_printf((buf), "%s" format, *(at_start) ? "" : ",", ##__VA_ARGS__); \
    *(at_start) = false; \
} while (0)

static int proc_show_mounts(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    struct mount *mount;
    list_for_each_entry(&mounts, mount, mounts) {
        const char *point = mount->point;
        if (point[0] == '\0')
            point = "/";

        proc_print_escaped(buf, mount->source);
        proc_printf(buf, " ");
        proc_print_escaped(buf, point);
        proc_printf(buf, " %s ", mount->fs->name);
        bool at_start = true;
        proc_printf_comma(buf, &at_start, "%s", mount->flags & MS_READONLY_ ? "ro" : "rw");
        if (mount->flags & MS_NOSUID_)
            proc_printf_comma(buf, &at_start, "nosuid");
        if (mount->flags & MS_NODEV_)
            proc_printf_comma(buf, &at_start, "nodev");
        if (mount->flags & MS_NOEXEC_)
            proc_printf_comma(buf, &at_start, "noexec");
        if (mount->info && mount->info[0] != '\0') // Ensure it's not NULL and not empty.
            proc_printf_comma(buf, &at_start, "%s", mount->info);
        proc_printf(buf, " 0 0\n");
    };
    return 0;
}

static int proc_mountinfo_id(struct mount *target) {
    int id = 1;
    struct mount *mount;
    list_for_each_entry(&mounts, mount, mounts) {
        if (mount == target)
            return id;
        id++;
    }
    return 1;
}

static int proc_mountinfo_parent_id(struct mount *target) {
    const char *point = target->point;
    if (point[0] == '\0')
        return 1;

    struct mount *mount;
    list_for_each_entry(&mounts, mount, mounts) {
        if (mount == target)
            continue;
        size_t n = mount->point_len;
        if (n >= target->point_len)
            continue;
        if (strncmp(point, mount->point, n) != 0)
            continue;
        if (point[n] != '/' && point[n] != '\0')
            continue;
        return proc_mountinfo_id(mount);
    }
    return 1;
}

int proc_show_mountinfo(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    struct mount *mount;
    list_for_each_entry(&mounts, mount, mounts) {
        const char *point = mount->point;
        if (point[0] == '\0')
            point = "/";

        int id = proc_mountinfo_id(mount);
        int parent_id = proc_mountinfo_parent_id(mount);

        proc_printf(buf, "%d %d 0:0 / ", id, parent_id);
        proc_print_escaped(buf, point);
        proc_printf(buf, " %s", mount->flags & MS_READONLY_ ? "ro" : "rw");
        if (mount->flags & MS_NOSUID_)
            proc_printf(buf, ",nosuid");
        if (mount->flags & MS_NODEV_)
            proc_printf(buf, ",nodev");
        if (mount->flags & MS_NOEXEC_)
            proc_printf(buf, ",noexec");
        proc_printf(buf, " - %s ", mount->fs->name);
        proc_print_escaped(buf, mount->source[0] == '\0' ? "/" : mount->source);
        proc_printf(buf, " %s", mount->flags & MS_READONLY_ ? "ro" : "rw");
        if (mount->info && mount->info[0] != '\0')
            proc_printf(buf, ",%s", mount->info);
        proc_printf(buf, "\n");
    }
    return 0;
}

// in alphabetical order
struct proc_dir_entry proc_root_entries[] = {
    {"cmdline", .show = proc_show_cmdline},
    {"consoles", .show = proc_show_consoles},
    {"cpuinfo", .show = proc_show_cpuinfo},
    {"diskstats", .show = proc_show_diskstats},
    {"filesystems", .show = proc_show_filesystems},
    {"ish", S_IFDIR, .children = &proc_ish_children},
    {"loadavg", .show = proc_show_loadavg},
    {"meminfo", .show = proc_show_meminfo},
    {"mountinfo", .show = proc_show_mountinfo},
    {"mounts", .show = proc_show_mounts},
    {"net", S_IFDIR, .children = &proc_net_children},
    {"self", S_IFLNK, .readlink = proc_readlink_self},
    {"stat", .show = proc_show_stat},
    {"sys", S_IFDIR, .children = &proc_sys_children},
    {"uptime", .show = proc_show_uptime},
    {"version", .show = proc_show_version},
    {"vmstat", .show = proc_show_vmstat},
};
#define PROC_ROOT_LEN sizeof(proc_root_entries)/sizeof(proc_root_entries[0])

static int proc_root_pid_compare(const void *lhs, const void *rhs) {
    pid_t_ a = *(const pid_t_ *) lhs;
    pid_t_ b = *(const pid_t_ *) rhs;
    return (a > b) - (a < b);
}

static void proc_root_refresh_pid_snapshot(struct proc_entry *entry) {
    if (entry->child_names != NULL) {
        free_string_array(entry->child_names);
        entry->child_names = NULL;
    }

    unsigned used = 0;
    pid_t_ *pids = NULL;
    complex_lockt(&pids_lock, 0);
    struct pid *pid_entry;
    list_for_each_entry(&alive_pids_list, pid_entry, alive) {
        struct task *task = pid_entry->task;
        if (task == NULL || task->zombie)
            continue;
        used++;
    }
    unlock(&pids_lock);

    pids = calloc(used ? used : 1, sizeof(*pids));
    if (pids == NULL)
        return;

    unsigned filled = 0;
    complex_lockt(&pids_lock, 0);
    list_for_each_entry(&alive_pids_list, pid_entry, alive) {
        struct task *task = pid_entry->task;
        if (task == NULL || task->zombie)
            continue;
        if (filled >= used)
            break;
        pids[filled++] = pid_entry->id;
    }
    unlock(&pids_lock);
    used = filled;

    char **names = calloc(used + 1, sizeof(*names));
    if (names == NULL)
        return;

    qsort(pids, used, sizeof(*pids), proc_root_pid_compare);
    for (unsigned i = 0; i < used; i++) {
        names[i] = malloc(16);
        if (names[i] == NULL) {
            free(pids);
            free_string_array(names);
            return;
        }
        snprintf(names[i], 16, "%d", pids[i]);
    }
    free(pids);
    entry->child_names = names;
}

static bool proc_root_readdir(struct proc_entry *entry, unsigned long *index, struct proc_entry *next_entry) {
    if (*index < PROC_ROOT_LEN) {
        *next_entry = (struct proc_entry) {&proc_root_entries[*index], *index, NULL, NULL, 0, 0, NULL};
        (*index)++;
        return true;
    }

    unsigned long pid_index = *index - PROC_ROOT_LEN;
    if (pid_index == 0 || entry->child_names == NULL)
        proc_root_refresh_pid_snapshot(entry);
    if (entry->child_names == NULL || entry->child_names[pid_index] == NULL)
        return false;

    *next_entry = (struct proc_entry) {
        &proc_pid,
        .pid = (pid_t_) strtoul(entry->child_names[pid_index], NULL, 10),
    };
    (*index)++;
    return true;
}

struct proc_dir_entry proc_root = {NULL, S_IFDIR, .readdir = proc_root_readdir};

void proc_root_init(void) {
    proc_set_entries_parent(proc_root_entries, PROC_ROOT_LEN, &proc_root);
    proc_pid.parent = &proc_root;

    proc_ish_init(proc_find_entry(proc_root_entries, PROC_ROOT_LEN, "ish"));
    proc_net_init(proc_find_entry(proc_root_entries, PROC_ROOT_LEN, "net"));
    proc_sys_init(proc_find_entry(proc_root_entries, PROC_ROOT_LEN, "sys"));
    proc_pid_init();
}

enum sysfs_node_kind {
    sysfs_root,
    sysfs_devices,
    sysfs_fs,
    sysfs_cgroup,
    sysfs_cgroup_unified,
    sysfs_cgroup_elogind,
    sysfs_system,
    sysfs_cpu,
    sysfs_online,
    sysfs_possible,
    sysfs_present,
    sysfs_kernel_max,
    sysfs_offline,
    sysfs_cpu_dir,
    sysfs_block,
    sysfs_block_disk1,
    sysfs_block_disk1_dev,
    sysfs_block_disk1_stat,
};

struct sysfs_node {
    enum sysfs_node_kind kind;
    int cpu;
};

static const struct fd_ops sysfs_fdops;

static inline void *sysfs_encode_node(struct sysfs_node node) {
    uintptr_t value = ((uintptr_t) node.kind << 16) | (unsigned short) (node.cpu + 1);
    return (void *) value;
}

static inline struct sysfs_node sysfs_decode_node(void *value) {
    uintptr_t encoded = (uintptr_t) value;
    return (struct sysfs_node) {
        .kind = (enum sysfs_node_kind) (encoded >> 16),
        .cpu = ((int) (encoded & 0xffff)) - 1,
    };
}

static int sysfs_cpu_count(void) {
    return MAX(get_cpu_count(), 1);
}

static bool sysfs_node_name(struct sysfs_node node, char *buf, size_t bufsize) {
    switch (node.kind) {
        case sysfs_root:
            return snprintf(buf, bufsize, "") >= 0;
        case sysfs_devices:
            return snprintf(buf, bufsize, "devices") >= 0;
        case sysfs_fs:
            return snprintf(buf, bufsize, "fs") >= 0;
        case sysfs_cgroup:
            return snprintf(buf, bufsize, "cgroup") >= 0;
        case sysfs_cgroup_unified:
            return snprintf(buf, bufsize, "unified") >= 0;
        case sysfs_cgroup_elogind:
            return snprintf(buf, bufsize, "elogind") >= 0;
        case sysfs_system:
            return snprintf(buf, bufsize, "system") >= 0;
        case sysfs_cpu:
            return snprintf(buf, bufsize, "cpu") >= 0;
        case sysfs_online:
            return snprintf(buf, bufsize, "online") >= 0;
        case sysfs_possible:
            return snprintf(buf, bufsize, "possible") >= 0;
        case sysfs_present:
            return snprintf(buf, bufsize, "present") >= 0;
        case sysfs_kernel_max:
            return snprintf(buf, bufsize, "kernel_max") >= 0;
        case sysfs_offline:
            return snprintf(buf, bufsize, "offline") >= 0;
        case sysfs_cpu_dir:
            return snprintf(buf, bufsize, "cpu%d", node.cpu) >= 0;
        case sysfs_block:
            return snprintf(buf, bufsize, "block") >= 0;
        case sysfs_block_disk1:
            return snprintf(buf, bufsize, "disk1") >= 0;
        case sysfs_block_disk1_dev:
            return snprintf(buf, bufsize, "dev") >= 0;
        case sysfs_block_disk1_stat:
            return snprintf(buf, bufsize, "stat") >= 0;
    }
    return false;
}

static mode_t_ sysfs_node_mode(struct sysfs_node node) {
    switch (node.kind) {
        case sysfs_root:
        case sysfs_devices:
        case sysfs_fs:
        case sysfs_cgroup:
        case sysfs_cgroup_unified:
        case sysfs_cgroup_elogind:
        case sysfs_system:
        case sysfs_cpu:
        case sysfs_cpu_dir:
        case sysfs_block:
        case sysfs_block_disk1:
            return S_IFDIR | 0555;
        case sysfs_online:
        case sysfs_possible:
        case sysfs_present:
        case sysfs_kernel_max:
        case sysfs_offline:
        case sysfs_block_disk1_dev:
        case sysfs_block_disk1_stat:
            return S_IFREG | 0444;
    }
    return S_IFREG | 0444;
}

static ino_t sysfs_node_inode(struct sysfs_node node) {
    switch (node.kind) {
        case sysfs_root: return 1;
        case sysfs_devices: return 2;
        case sysfs_fs: return 3;
        case sysfs_cgroup: return 4;
        case sysfs_cgroup_unified: return 5;
        case sysfs_cgroup_elogind: return 6;
        case sysfs_system: return 7;
        case sysfs_cpu: return 8;
        case sysfs_online: return 9;
        case sysfs_possible: return 10;
        case sysfs_present: return 11;
        case sysfs_kernel_max: return 12;
        case sysfs_offline: return 13;
        case sysfs_cpu_dir: return 100 + node.cpu;
        case sysfs_block: return 14;
        case sysfs_block_disk1: return 15;
        case sysfs_block_disk1_dev: return 16;
        case sysfs_block_disk1_stat: return 17;
    }
    return 0;
}

static bool sysfs_lookup_node(const char *path, struct sysfs_node *node_out) {
    if (path[0] == '/')
        path++;
    if (strcmp(path, "") == 0) {
        *node_out = (struct sysfs_node) {.kind = sysfs_root, .cpu = -1};
        return true;
    }
    if (strcmp(path, "devices") == 0) {
        *node_out = (struct sysfs_node) {.kind = sysfs_devices, .cpu = -1};
        return true;
    }
    if (strcmp(path, "fs") == 0) {
        *node_out = (struct sysfs_node) {.kind = sysfs_fs, .cpu = -1};
        return true;
    }
    if (strcmp(path, "fs/cgroup") == 0) {
        *node_out = (struct sysfs_node) {.kind = sysfs_cgroup, .cpu = -1};
        return true;
    }
    if (strcmp(path, "fs/cgroup/unified") == 0) {
        *node_out = (struct sysfs_node) {.kind = sysfs_cgroup_unified, .cpu = -1};
        return true;
    }
    if (strcmp(path, "fs/cgroup/elogind") == 0) {
        *node_out = (struct sysfs_node) {.kind = sysfs_cgroup_elogind, .cpu = -1};
        return true;
    }
    if (strcmp(path, "devices/system") == 0) {
        *node_out = (struct sysfs_node) {.kind = sysfs_system, .cpu = -1};
        return true;
    }
    if (strcmp(path, "devices/system/cpu") == 0) {
        *node_out = (struct sysfs_node) {.kind = sysfs_cpu, .cpu = -1};
        return true;
    }
    if (strcmp(path, "devices/system/cpu/online") == 0) {
        *node_out = (struct sysfs_node) {.kind = sysfs_online, .cpu = -1};
        return true;
    }
    if (strcmp(path, "devices/system/cpu/possible") == 0) {
        *node_out = (struct sysfs_node) {.kind = sysfs_possible, .cpu = -1};
        return true;
    }
    if (strcmp(path, "devices/system/cpu/present") == 0) {
        *node_out = (struct sysfs_node) {.kind = sysfs_present, .cpu = -1};
        return true;
    }
    if (strcmp(path, "devices/system/cpu/kernel_max") == 0) {
        *node_out = (struct sysfs_node) {.kind = sysfs_kernel_max, .cpu = -1};
        return true;
    }
    if (strcmp(path, "devices/system/cpu/offline") == 0) {
        *node_out = (struct sysfs_node) {.kind = sysfs_offline, .cpu = -1};
        return true;
    }
    if (strcmp(path, "block") == 0) {
        *node_out = (struct sysfs_node) {.kind = sysfs_block, .cpu = -1};
        return true;
    }
    if (strcmp(path, "block/disk1") == 0) {
        *node_out = (struct sysfs_node) {.kind = sysfs_block_disk1, .cpu = -1};
        return true;
    }
    if (strcmp(path, "block/disk1/dev") == 0) {
        *node_out = (struct sysfs_node) {.kind = sysfs_block_disk1_dev, .cpu = -1};
        return true;
    }
    if (strcmp(path, "block/disk1/stat") == 0) {
        *node_out = (struct sysfs_node) {.kind = sysfs_block_disk1_stat, .cpu = -1};
        return true;
    }

    int cpu;
    if (sscanf(path, "devices/system/cpu/cpu%d", &cpu) == 1 && cpu >= 0 && cpu < sysfs_cpu_count()) {
        char exact[32];
        snprintf(exact, sizeof(exact), "devices/system/cpu/cpu%d", cpu);
        if (strcmp(path, exact) == 0) {
            *node_out = (struct sysfs_node) {.kind = sysfs_cpu_dir, .cpu = cpu};
            return true;
        }
    }

    return false;
}

// Real Linux keeps this at /sys/block/<dev>/stat: the same 11 diskstats
// fields as /proc/diskstats, minus the leading major/minor/name -- backed by
// the same realfs_io_stats counters as proc_show_diskstats.
static size_t sysfs_disk1_stat_format(char *buf, size_t bufsize) {
    uint64_t read_ops = atomic_load_explicit(&realfs_io_stats.read_ops, memory_order_relaxed);
    uint64_t read_sectors = atomic_load_explicit(&realfs_io_stats.read_bytes, memory_order_relaxed) / 512;
    uint64_t write_ops = atomic_load_explicit(&realfs_io_stats.write_ops, memory_order_relaxed);
    uint64_t write_sectors = atomic_load_explicit(&realfs_io_stats.write_bytes, memory_order_relaxed) / 512;
    return snprintf(buf, bufsize, "%"PRIu64" 0 %"PRIu64" 0 %"PRIu64" 0 %"PRIu64" 0 0 0 0\n",
                     read_ops, read_sectors, write_ops, write_sectors);
}

static size_t sysfs_file_size(struct sysfs_node node) {
    int last_cpu = sysfs_cpu_count() - 1;
    switch (node.kind) {
        case sysfs_online:
        case sysfs_possible:
        case sysfs_present:
            if (last_cpu == 0)
                return strlen("0\n");
            return strlen("0-\n") + 10;
        case sysfs_kernel_max:
            return 12;
        case sysfs_offline:
            return strlen("\n");
        case sysfs_block_disk1_dev:
            return strlen("8:0\n");
        case sysfs_block_disk1_stat:
            return sysfs_disk1_stat_format(NULL, 0);
        default:
            return 0;
    }
}

static size_t sysfs_file_data(struct sysfs_node node, char *buf, size_t bufsize) {
    int last_cpu = sysfs_cpu_count() - 1;
    switch (node.kind) {
        case sysfs_online:
        case sysfs_possible:
        case sysfs_present:
            if (last_cpu == 0)
                return snprintf(buf, bufsize, "0\n");
            return snprintf(buf, bufsize, "0-%d\n", last_cpu);
        case sysfs_kernel_max:
            return snprintf(buf, bufsize, "%d\n", last_cpu);
        case sysfs_offline:
            return snprintf(buf, bufsize, "\n");
        case sysfs_block_disk1_dev:
            return snprintf(buf, bufsize, "8:0\n");
        case sysfs_block_disk1_stat:
            return sysfs_disk1_stat_format(buf, bufsize);
        default:
            return 0;
    }
}

static struct fd *sysfs_open(struct mount *mount, const char *path, int UNUSED(flags), int UNUSED(mode)) {
    struct sysfs_node node;
    if (!sysfs_lookup_node(path, &node))
        return ERR_PTR(_ENOENT);
    struct fd *fd = fd_create(&sysfs_fdops);
    if (fd == NULL)
        return ERR_PTR(_ENOMEM);
    mount_retain(mount);
    fd->mount = mount;
    fd->type = sysfs_node_mode(node) & S_IFMT;
    fd->fs_data = sysfs_encode_node(node);
    return fd;
}

static int sysfs_stat_common(struct sysfs_node node, struct statbuf *stat) {
    memset(stat, 0, sizeof(*stat));
    stat->inode = sysfs_node_inode(node);
    stat->mode = sysfs_node_mode(node);
    stat->nlink = S_ISDIR(stat->mode) ? 2 : 1;
    stat->size = sysfs_file_size(node);
    return 0;
}

static int sysfs_stat(struct mount *UNUSED(mount), const char *path, struct statbuf *stat) {
    struct sysfs_node node;
    if (!sysfs_lookup_node(path, &node))
        return _ENOENT;
    return sysfs_stat_common(node, stat);
}

static int sysfs_fstat(struct fd *fd, struct statbuf *stat) {
    return sysfs_stat_common(sysfs_decode_node(fd->fs_data), stat);
}

static int sysfs_getpath(struct fd *fd, char *buf) {
    struct sysfs_node node = sysfs_decode_node(fd->fs_data);
    switch (node.kind) {
        case sysfs_root:
            strcpy(buf, "");
            break;
        case sysfs_devices:
            strcpy(buf, "/devices");
            break;
        case sysfs_fs:
            strcpy(buf, "/fs");
            break;
        case sysfs_cgroup:
            strcpy(buf, "/fs/cgroup");
            break;
        case sysfs_cgroup_unified:
            strcpy(buf, "/fs/cgroup/unified");
            break;
        case sysfs_cgroup_elogind:
            strcpy(buf, "/fs/cgroup/elogind");
            break;
        case sysfs_system:
            strcpy(buf, "/devices/system");
            break;
        case sysfs_cpu:
            strcpy(buf, "/devices/system/cpu");
            break;
        case sysfs_online:
            strcpy(buf, "/devices/system/cpu/online");
            break;
        case sysfs_possible:
            strcpy(buf, "/devices/system/cpu/possible");
            break;
        case sysfs_present:
            strcpy(buf, "/devices/system/cpu/present");
            break;
        case sysfs_kernel_max:
            strcpy(buf, "/devices/system/cpu/kernel_max");
            break;
        case sysfs_offline:
            strcpy(buf, "/devices/system/cpu/offline");
            break;
        case sysfs_cpu_dir:
            snprintf(buf, MAX_PATH, "/devices/system/cpu/cpu%d", node.cpu);
            break;
        case sysfs_block:
            strcpy(buf, "/block");
            break;
        case sysfs_block_disk1:
            strcpy(buf, "/block/disk1");
            break;
        case sysfs_block_disk1_dev:
            strcpy(buf, "/block/disk1/dev");
            break;
        case sysfs_block_disk1_stat:
            strcpy(buf, "/block/disk1/stat");
            break;
    }
    return 0;
}

static ssize_t sysfs_pread(struct fd *fd, void *buf, size_t bufsize, off_t off) {
    struct sysfs_node node = sysfs_decode_node(fd->fs_data);
    if (S_ISDIR(sysfs_node_mode(node)))
        return _EISDIR;

    char data[128];
    size_t size = sysfs_file_data(node, data, sizeof(data));
    if ((size_t) off > size)
        return 0;
    size_t remaining = size - off;
    if (bufsize > remaining)
        bufsize = remaining;
    memcpy(buf, data + off, bufsize);
    return bufsize;
}

static ssize_t sysfs_read(struct fd *fd, void *buf, size_t bufsize) {
    ssize_t res = sysfs_pread(fd, buf, bufsize, fd->offset);
    if (res > 0)
        fd->offset += res;
    return res;
}

static ssize_t sysfs_write(struct fd *UNUSED(fd), const void *UNUSED(buf), size_t UNUSED(bufsize)) {
    return _EROFS;
}

static off_t_ sysfs_lseek(struct fd *fd, off_t_ off, int whence) {
    struct sysfs_node node = sysfs_decode_node(fd->fs_data);
    if (S_ISDIR(sysfs_node_mode(node)))
        return _EINVAL;
    return generic_seek(fd, off, whence, sysfs_file_size(node));
}

static int sysfs_readdir(struct fd *fd, struct dir_entry *entry) {
    struct sysfs_node node = sysfs_decode_node(fd->fs_data);
    unsigned long index = fd->offset++;
    struct sysfs_node child;

    switch (node.kind) {
        case sysfs_root:
            switch (index) {
                case 0: child = (struct sysfs_node) {.kind = sysfs_devices, .cpu = -1}; break;
                case 1: child = (struct sysfs_node) {.kind = sysfs_fs, .cpu = -1}; break;
                case 2: child = (struct sysfs_node) {.kind = sysfs_block, .cpu = -1}; break;
                default: return 0;
            }
            break;
        case sysfs_devices:
            if (index != 0)
                return 0;
            child = (struct sysfs_node) {.kind = sysfs_system, .cpu = -1};
            break;
        case sysfs_fs:
            if (index != 0)
                return 0;
            child = (struct sysfs_node) {.kind = sysfs_cgroup, .cpu = -1};
            break;
        case sysfs_cgroup:
            if (index == 0) {
                child = (struct sysfs_node) {.kind = sysfs_cgroup_unified, .cpu = -1};
                break;
            }
            if (index != 1)
                return 0;
            child = (struct sysfs_node) {.kind = sysfs_cgroup_elogind, .cpu = -1};
            break;
        case sysfs_cgroup_unified:
        case sysfs_cgroup_elogind:
            return 0;
        case sysfs_system:
            if (index != 0)
                return 0;
            child = (struct sysfs_node) {.kind = sysfs_cpu, .cpu = -1};
            break;
        case sysfs_cpu: {
            int ncpus = sysfs_cpu_count();
            switch (index) {
                case 0: child = (struct sysfs_node) {.kind = sysfs_online, .cpu = -1}; break;
                case 1: child = (struct sysfs_node) {.kind = sysfs_possible, .cpu = -1}; break;
                case 2: child = (struct sysfs_node) {.kind = sysfs_present, .cpu = -1}; break;
                case 3: child = (struct sysfs_node) {.kind = sysfs_kernel_max, .cpu = -1}; break;
                case 4: child = (struct sysfs_node) {.kind = sysfs_offline, .cpu = -1}; break;
                default:
                    if ((int) index - 5 >= ncpus)
                        return 0;
                    child = (struct sysfs_node) {.kind = sysfs_cpu_dir, .cpu = (int) index - 5};
                    break;
            }
            break;
        }
        case sysfs_cpu_dir:
            return 0;
        case sysfs_block:
            if (index != 0)
                return 0;
            child = (struct sysfs_node) {.kind = sysfs_block_disk1, .cpu = -1};
            break;
        case sysfs_block_disk1:
            switch (index) {
                case 0: child = (struct sysfs_node) {.kind = sysfs_block_disk1_dev, .cpu = -1}; break;
                case 1: child = (struct sysfs_node) {.kind = sysfs_block_disk1_stat, .cpu = -1}; break;
                default: return 0;
            }
            break;
        case sysfs_block_disk1_dev:
        case sysfs_block_disk1_stat:
            return 0;
        default:
            return _ENOTDIR;
    }

    sysfs_node_name(child, entry->name, sizeof(entry->name));
    entry->inode = sysfs_node_inode(child);
    entry->type = dir_entry_type_for_mode(sysfs_node_mode(child));
    return 1;
}

static int sysfs_close(struct fd *UNUSED(fd)) {
    return 0;
}

static const struct fd_ops sysfs_fdops = {
    .read = sysfs_read,
    .write = sysfs_write,
    .pread = sysfs_pread,
    .pwrite = NULL,
    .lseek = sysfs_lseek,
    .readdir = sysfs_readdir,
    .close = sysfs_close,
};

const struct fs_ops sysfs = {
    .name = "sysfs",
    .magic = 0x62656572,
    .open = sysfs_open,
    .stat = sysfs_stat,
    .fstat = sysfs_fstat,
    .getpath = sysfs_getpath,
};
