/*
 * ktop -- a small, dependency-free top(1) clone with one extra column: the
 * guest CPU architecture (arm64 / x86_64 / x86 / riscv64) of each process's
 * binary, read straight from its ELF header via /proc/<pid>/exe.
 *
 * Built for iSH-AOK: a single guest can run i386, amd64, arm64 and riscv64
 * binaries side by side (e.g. a chroot into another installed root via
 * /AOK/tools/mount-root.sh), and stock top has no way to show that mix.
 * ktop otherwise behaves like ordinary top: an interactive, periodically
 * refreshing process table sorted by %CPU, or -b batch mode for scripting.
 *
 * No dependencies beyond a C99 libc and /proc -- no ncurses, no procps.
 *
 * Usage: ktop [-b] [-n iterations] [-d seconds]
 *   -b            batch mode: no screen clearing/terminal control, just
 *                 print each snapshot and exit after -n iterations
 *                 (default 1 in batch mode).
 *   -n iterations number of snapshots before exiting (default: run until
 *                 'q' is pressed or the process is killed, in interactive
 *                 mode).
 *   -d seconds    delay between snapshots (default: 3).
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pwd.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define MAX_PROCS 4096

struct proc_sample {
    pid_t pid;
    uid_t uid;
    char comm[64];
    char state;
    long priority, nice;
    unsigned long vsize;       // bytes
    long rss_pages;
    unsigned long long utime, stime; // jiffies, cumulative
    const char *arch;          // interned string, never freed
    unsigned long long cpu_delta; // jiffies since the previous sample;
                                   // transient, filled in by print_snapshot
                                   // just before sorting/printing (0 for a
                                   // process not present in the previous
                                   // sample, including the very first one)
};

// ---- ELF-header architecture detection -----------------------------------

static const char *arch_intern(const char *s) {
    // All callers pass one of these three literals or "?"; returning the
    // literal itself keeps proc_sample.arch a non-owning pointer with no
    // allocation/free bookkeeping.
    return s;
}

static const char *detect_arch(pid_t pid) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/exe", (int) pid);
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return arch_intern("?");

    unsigned char hdr[20];
    ssize_t n = read(fd, hdr, sizeof(hdr));
    close(fd);
    if (n < 20 || memcmp(hdr, "\x7f""ELF", 4) != 0)
        return arch_intern("?");

    bool little_endian = hdr[5] != 2; // EI_DATA: 1 = LSB, 2 = MSB
    unsigned e_machine = little_endian
        ? (unsigned) hdr[18] | ((unsigned) hdr[19] << 8)
        : (unsigned) hdr[19] | ((unsigned) hdr[18] << 8);

    switch (e_machine) {
        case 3:   return arch_intern("x86");    // EM_386
        case 62:  return arch_intern("x86_64"); // EM_X86_64
        case 183: return arch_intern("arm64");  // EM_AARCH64
        case 40:  return arch_intern("arm");    // EM_ARM (32-bit, just in case)
        case 243: return arch_intern("riscv64"); // EM_RISCV
        default:  return arch_intern("?");
    }
}

// ---- /proc/<pid>/stat parsing ---------------------------------------------

// Reads one process's fields we need. /proc/<pid>/stat's 2nd field (comm) is
// the only one that can contain spaces or parens, so we locate it by the
// *last* ')' on the line rather than naive whitespace splitting.
static bool read_proc_stat(pid_t pid, struct proc_sample *out) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/stat", (int) pid);
    FILE *f = fopen(path, "r");
    if (f == NULL)
        return false;
    char line[1024];
    bool ok = fgets(line, sizeof(line), f) != NULL;
    fclose(f);
    if (!ok)
        return false;

    char *lparen = strchr(line, '(');
    char *rparen = strrchr(line, ')');
    if (lparen == NULL || rparen == NULL || rparen < lparen)
        return false;

    size_t comm_len = (size_t) (rparen - lparen - 1);
    if (comm_len >= sizeof(out->comm))
        comm_len = sizeof(out->comm) - 1;
    memcpy(out->comm, lparen + 1, comm_len);
    out->comm[comm_len] = '\0';

    out->pid = pid;

    // Fields after ") " are: state pid.ppid... -- field 3 (state) is the
    // first token past the comm; utime/stime are fields 14/15; priority/nice
    // are 18/19; vsize/rss are 23/24 (1-indexed, full /proc/pid/stat layout).
    char *p = rparen + 2;
    int field = 3;
    char state = '?';
    unsigned long long utime = 0, stime = 0;
    long priority = 0, nice_val = 0;
    unsigned long vsize = 0;
    long rss = 0;
    while (*p != '\0' && field <= 24) {
        char *end;
        switch (field) {
            case 3:
                state = *p;
                while (*p != '\0' && !isspace((unsigned char) *p))
                    p++;
                break;
            case 14:
                utime = strtoull(p, &end, 10);
                p = end;
                break;
            case 15:
                stime = strtoull(p, &end, 10);
                p = end;
                break;
            case 18:
                priority = strtol(p, &end, 10);
                p = end;
                break;
            case 19:
                nice_val = strtol(p, &end, 10);
                p = end;
                break;
            case 23:
                vsize = strtoul(p, &end, 10);
                p = end;
                break;
            case 24:
                rss = strtol(p, &end, 10);
                p = end;
                break;
            default:
                while (*p != '\0' && !isspace((unsigned char) *p))
                    p++;
                break;
        }
        while (*p == ' ')
            p++;
        field++;
    }

    out->state = state;
    out->utime = utime;
    out->stime = stime;
    out->priority = priority;
    out->nice = nice_val;
    out->vsize = vsize;
    out->rss_pages = rss;
    return true;
}

static uid_t read_proc_uid(pid_t pid) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/status", (int) pid);
    FILE *f = fopen(path, "r");
    if (f == NULL)
        return (uid_t) -1;
    uid_t uid = (uid_t) -1;
    char line[256];
    while (fgets(line, sizeof(line), f) != NULL) {
        unsigned long real;
        if (sscanf(line, "Uid: %lu", &real) == 1) {
            uid = (uid_t) real;
            break;
        }
    }
    fclose(f);
    return uid;
}

// ---- system-wide totals ----------------------------------------------------

static void read_meminfo(unsigned long *total_kb, unsigned long *free_kb, unsigned long *avail_kb) {
    *total_kb = *free_kb = *avail_kb = 0;
    FILE *f = fopen("/proc/meminfo", "r");
    if (f == NULL)
        return;
    char line[256];
    while (fgets(line, sizeof(line), f) != NULL) {
        sscanf(line, "MemTotal: %lu kB", total_kb);
        sscanf(line, "MemFree: %lu kB", free_kb);
        sscanf(line, "MemAvailable: %lu kB", avail_kb);
    }
    fclose(f);
}

static void read_loadavg(double *l1, double *l5, double *l15) {
    *l1 = *l5 = *l15 = 0;
    FILE *f = fopen("/proc/loadavg", "r");
    if (f == NULL)
        return;
    if (fscanf(f, "%lf %lf %lf", l1, l5, l15) != 3) {
        *l1 = *l5 = *l15 = 0;
    }
    fclose(f);
}

// Total CPU jiffies across all modes, from /proc/stat's aggregate "cpu " line.
static unsigned long long read_total_cpu_jiffies(void) {
    FILE *f = fopen("/proc/stat", "r");
    if (f == NULL)
        return 0;
    char label[16];
    unsigned long long vals[10] = {0};
    unsigned long long total = 0;
    if (fscanf(f, "%15s", label) == 1 && strcmp(label, "cpu") == 0) {
        for (int i = 0; i < 10; i++) {
            if (fscanf(f, "%llu", &vals[i]) != 1)
                break;
            total += vals[i];
        }
    }
    fclose(f);
    return total;
}

// ---- process table snapshot ------------------------------------------------

static int collect(struct proc_sample *procs, int max) {
    DIR *d = opendir("/proc");
    if (d == NULL) {
        perror("ktop: opendir /proc");
        return 0;
    }
    int count = 0;
    struct dirent *de;
    while (count < max && (de = readdir(d)) != NULL) {
        if (!isdigit((unsigned char) de->d_name[0]))
            continue;
        pid_t pid = (pid_t) atol(de->d_name);
        struct proc_sample p = {0};
        if (!read_proc_stat(pid, &p))
            continue;
        p.uid = read_proc_uid(pid);
        p.arch = detect_arch(pid);
        procs[count++] = p;
    }
    closedir(d);
    return count;
}

// Sorts by cpu_delta (jiffies used since the previous sample), which
// print_snapshot fills in for every entry before calling qsort -- NOT by raw
// cumulative utime+stime, which would permanently rank long-lived idle
// daemons above short-lived busy ones.
static int cmp_by_cpu_delta(const void *pa, const void *pb) {
    const struct proc_sample *a = pa, *b = pb;
    if (a->cpu_delta != b->cpu_delta)
        return a->cpu_delta > b->cpu_delta ? -1 : 1;
    return a->pid < b->pid ? -1 : (a->pid > b->pid ? 1 : 0);
}

static const char *username_for(uid_t uid, char *buf, size_t bufsize) {
    if (uid == (uid_t) -1) {
        snprintf(buf, bufsize, "?");
        return buf;
    }
    struct passwd *pw = getpwuid(uid);
    if (pw != NULL) {
        snprintf(buf, bufsize, "%s", pw->pw_name);
        return buf;
    }
    snprintf(buf, bufsize, "%lu", (unsigned long) uid);
    return buf;
}

static void format_uptime(char *buf, size_t bufsize) {
    FILE *f = fopen("/proc/uptime", "r");
    double seconds = 0;
    if (f != NULL) {
        if (fscanf(f, "%lf", &seconds) != 1)
            seconds = 0;
        fclose(f);
    }
    long total = (long) seconds;
    long days = total / 86400;
    long hours = (total % 86400) / 3600;
    long mins = (total % 3600) / 60;
    if (days > 0)
        snprintf(buf, bufsize, "%ld days, %02ld:%02ld", days, hours, mins);
    else
        snprintf(buf, bufsize, "%02ld:%02ld", hours, mins);
}

static void print_snapshot(struct proc_sample *cur, int cur_n,
                            struct proc_sample *prev, int prev_n,
                            unsigned long long cpu_jiffies_delta,
                            long clk_tck, long page_kb, bool batch) {
    if (!batch)
        fputs("\033[H\033[J", stdout); // home + clear, no ncurses needed

    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    char timebuf[16];
    strftime(timebuf, sizeof(timebuf), "%H:%M:%S", &tm_now);

    char uptime[32];
    format_uptime(uptime, sizeof(uptime));
    double l1, l5, l15;
    read_loadavg(&l1, &l5, &l15);
    printf("ktop - %s up %s, load average: %.2f, %.2f, %.2f\n",
           timebuf, uptime, l1, l5, l15);

    int running = 0, sleeping = 0, stopped = 0, zombie = 0;
    for (int i = 0; i < cur_n; i++) {
        switch (cur[i].state) {
            case 'R': running++; break;
            case 'S': case 'D': sleeping++; break;
            case 'T': case 't': stopped++; break;
            case 'Z': zombie++; break;
            default: break;
        }
    }
    printf("Tasks: %d total, %d running, %d sleeping, %d stopped, %d zombie\n",
           cur_n, running, sleeping, stopped, zombie);

    // %CPU across all cores: fraction of aggregate jiffy-delta that wasn't
    // idle+iowait, matching how the per-process %CPU below is derived from
    // the same clk_tck-based accounting.
    unsigned long total_kb, free_kb, avail_kb;
    read_meminfo(&total_kb, &free_kb, &avail_kb);
    unsigned long used_kb = total_kb > free_kb ? total_kb - free_kb : 0;
    printf("Mem: %lukB total, %lukB used, %lukB free\n",
           total_kb, used_kb, free_kb);
    (void) avail_kb;
    (void) cpu_jiffies_delta;

    printf("\n%6s %-8s %3s %3s %8s %8s %-7s %5s %5s  %s\n",
           "PID", "USER", "PR", "NI", "VIRT(K)", "RES(K)", "ARCH", "%CPU", "%MEM", "COMMAND");

    // Fill in cpu_delta (jiffies used since the previous sample) BEFORE
    // sorting: a process absent from the previous sample -- including every
    // process on the very first snapshot, where prev_n is 0 -- gets 0, not
    // its full lifetime utime+stime. Without this, long-lived processes
    // would show wildly inflated %CPU on first display and permanently
    // outrank recently-busy ones in the sort.
    for (int i = 0; i < cur_n; i++) {
        cur[i].cpu_delta = 0;
        unsigned long long cur_total = cur[i].utime + cur[i].stime;
        for (int j = 0; j < prev_n; j++) {
            if (prev[j].pid == cur[i].pid) {
                unsigned long long prev_total = prev[j].utime + prev[j].stime;
                cur[i].cpu_delta = cur_total >= prev_total ? cur_total - prev_total : 0;
                break;
            }
        }
    }
    qsort(cur, (size_t) cur_n, sizeof(cur[0]), cmp_by_cpu_delta);

    for (int i = 0; i < cur_n; i++) {
        double cpu_pct = 0;
        if (clk_tck > 0)
            cpu_pct = (double) cur[i].cpu_delta * 100.0 / (double) clk_tck; // per sampling window (~-d seconds)

        double mem_pct = total_kb > 0
            ? (double) cur[i].rss_pages * (double) page_kb * 100.0 / (double) total_kb
            : 0;

        char userbuf[32];
        username_for(cur[i].uid, userbuf, sizeof(userbuf));

        printf("%6d %-8s %3ld %3ld %8lu %8ld %-7s %5.1f %5.1f  %s\n",
               (int) cur[i].pid, userbuf, cur[i].priority, cur[i].nice,
               cur[i].vsize / 1024, cur[i].rss_pages * page_kb,
               cur[i].arch, cpu_pct, mem_pct, cur[i].comm);
    }
    fflush(stdout);
}

static struct termios orig_termios;
static bool termios_saved = false;

static void restore_termios(void) {
    if (termios_saved)
        tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios);
}

static void enable_raw_stdin(void) {
    if (!isatty(STDIN_FILENO))
        return;
    if (tcgetattr(STDIN_FILENO, &orig_termios) != 0)
        return;
    termios_saved = true;
    atexit(restore_termios);
    struct termios raw = orig_termios;
    raw.c_lflag &= (tcflag_t) ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
}

// Sleeps up to `seconds`, returning early (true) if 'q' or Ctrl-C-like input
// arrives on stdin. In batch mode or when stdin isn't a tty, just sleeps.
static bool wait_or_quit(double seconds, bool batch) {
    if (batch || !isatty(STDIN_FILENO)) {
        struct timespec ts = {(time_t) seconds, (long) ((seconds - (time_t) seconds) * 1e9)};
        nanosleep(&ts, NULL);
        return false;
    }
    fd_set fds;
    struct timeval tv;
    double remaining = seconds;
    while (remaining > 0) {
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        double chunk = remaining > 0.2 ? 0.2 : remaining;
        tv.tv_sec = (time_t) chunk;
        tv.tv_usec = (suseconds_t) ((chunk - tv.tv_sec) * 1e6);
        int r = select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv);
        if (r > 0) {
            char c;
            if (read(STDIN_FILENO, &c, 1) == 1 && (c == 'q' || c == 'Q'))
                return true;
        }
        remaining -= chunk;
    }
    return false;
}

int main(int argc, char **argv) {
    bool batch = false;
    int iterations = -1; // -1 = unbounded (interactive default)
    double delay = 3.0;

    int opt;
    while ((opt = getopt(argc, argv, "bn:d:h")) != -1) {
        switch (opt) {
            case 'b': batch = true; break;
            case 'n': iterations = atoi(optarg); break;
            case 'd': delay = atof(optarg); break;
            case 'h':
                printf("usage: %s [-b] [-n iterations] [-d seconds]\n", argv[0]);
                return 0;
            default:
                fprintf(stderr, "usage: %s [-b] [-n iterations] [-d seconds]\n", argv[0]);
                return 2;
        }
    }
    if (batch && iterations < 0)
        iterations = 1;

    long clk_tck = sysconf(_SC_CLK_TCK);
    if (clk_tck <= 0)
        clk_tck = 100;
    long page_kb = sysconf(_SC_PAGESIZE) / 1024;
    if (page_kb <= 0)
        page_kb = 4;

    static struct proc_sample bufs[2][MAX_PROCS];
    int counts[2] = {0, 0};
    int cur_buf = 0;

    if (!batch)
        enable_raw_stdin();

    counts[cur_buf] = collect(bufs[cur_buf], MAX_PROCS);
    // First snapshot has no previous sample to diff against for %CPU; show
    // it once immediately (0% CPU for everything) so the user sees output
    // right away instead of waiting a full -d seconds for the first table.
    print_snapshot(bufs[cur_buf], counts[cur_buf], NULL, 0, 0, clk_tck, page_kb, batch);

    int iter = 1;
    while (iterations < 0 || iter < iterations) {
        if (wait_or_quit(delay, batch))
            break;
        int prev_buf = cur_buf;
        cur_buf ^= 1;
        counts[cur_buf] = collect(bufs[cur_buf], MAX_PROCS);
        unsigned long long total_now = read_total_cpu_jiffies();
        (void) total_now;
        print_snapshot(bufs[cur_buf], counts[cur_buf],
                        bufs[prev_buf], counts[prev_buf],
                        0, clk_tck, page_kb, batch);
        iter++;
    }
    return 0;
}
