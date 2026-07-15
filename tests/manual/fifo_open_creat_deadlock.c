// Regression test: open(O_WRONLY|O_CREAT) of an EXISTING FIFO racing the
// reader's open(O_RDONLY) must not deadlock the whole filesystem.
//
// fs/fake.c's fakefs_open (after the 81e58b65 TOCTOU fix) took the SQLite
// write transaction (db_begin_write, which holds fs->lock until commit)
// BEFORE the real host open whenever O_CREAT was set. Opening an existing
// FIFO without O_NONBLOCK blocks in the host openat() until a peer opens the
// other end -- so a shell's `cmd > fifo` redirect (O_WRONLY|O_CREAT) that
// reached the host open before the reader's `< fifo` blocked while HOLDING
// the fakefs db lock. The reader's own open then blocked on that lock at its
// pre-open stat, never reached the host open that would have unblocked the
// writer, and every other fakefs operation in the emulator wedged behind
// them: a whole-process freeze. Pure scheduling race, so intermittent --
// observed as /AOK/tools/start-wayland.sh (whose spawn_logged routes each
// child's output through a named FIFO into tee) failing to bring up the
// Wayland stack ~40% of the time (Display applet "Timed out waiting for the
// Wayland compositor"), with the whole guest fs frozen underneath.
//
// The fix (fs/fake.c fakefs_open): when O_CREAT targets a path whose fakefs
// metadata already describes a FIFO and O_NONBLOCK is not set, drop the
// write transaction and open without O_CREAT (an open of an existing path
// creates nothing, so there is no host-create+metadata pair to keep atomic);
// if the FIFO is unlinked in the window, ENOENT retries with O_CREAT
// restored.
//
// This test forks a reader (open O_RDONLY) and a writer (open
// O_WRONLY|O_CREAT -- same flags as a shell output redirect) per round,
// alternating which side gets a head start, and fails via SIGALRM watchdog
// if any round wedges. The FIFO must live on a fakefs mount to regress
// (TMPDIR may be tmpfs on device; default cwd of the suite is fakefs).
// A/B: unfixed build wedges within a few dozen rounds (watchdog fires,
// exit 1; the stuck children are unkillable -- blocked in an
// uninterruptible host openat -- so the wedged emulator needs an external
// kill); fixed build runs all rounds clean. Also passes on real Linux.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define ROUNDS 200
#define WATCHDOG_SECS 60

static int verbose;
static char path[256];

static void say(const char *fmt, int a, int b) {
    if (verbose) {
        printf(fmt, a, b);
        fflush(stdout);
    }
}

static volatile sig_atomic_t timed_out;
static void on_alarm(int sig) {
    (void) sig;
    timed_out = 1;
    // Async-signal-safe failure report; fs may be wedged, stdout is a tty/pipe.
    static const char msg[] = "fifo_open_creat_deadlock: FAIL (watchdog: fs deadlocked)\n";
    ssize_t r = write(2, msg, sizeof(msg) - 1);
    (void) r;
    _exit(1);
}

static pid_t spawn_reader(void) {
    pid_t pid = fork();
    if (pid == 0) {
        int fd;
        do {
            fd = open(path, O_RDONLY);
        } while (fd < 0 && errno == EINTR);
        if (fd < 0)
            _exit(2);
        char c;
        ssize_t n;
        do {
            n = read(fd, &c, 1);
        } while (n < 0 && errno == EINTR);
        close(fd);
        _exit(n == 1 && c == 'x' ? 0 : 3);
    }
    return pid;
}

static pid_t spawn_writer(void) {
    pid_t pid = fork();
    if (pid == 0) {
        // Same flags as a shell's `> fifo` output redirect: the O_CREAT is
        // what routed this open through fakefs's write transaction.
        int fd;
        do {
            fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        } while (fd < 0 && errno == EINTR);
        if (fd < 0)
            _exit(2);
        ssize_t n;
        do {
            n = write(fd, "x", 1);
        } while (n < 0 && errno == EINTR);
        close(fd);
        _exit(n == 1 ? 0 : 3);
    }
    return pid;
}

static int reap(pid_t pid, const char *who, int round) {
    int status;
    pid_t w;
    do {
        w = waitpid(pid, &status, 0);
    } while (w < 0 && errno == EINTR && !timed_out);
    if (w != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "fifo_open_creat_deadlock: FAIL round %d: %s exited abnormally (status %#x)\n",
                round, who, status);
        return -1;
    }
    return 0;
}

int main(int argc, char **argv) {
    verbose = argc > 1 && strcmp(argv[1], "-v") == 0;
    snprintf(path, sizeof(path), "fifo_creat_dl.%d", (int) getpid());

    struct sigaction sa = { .sa_handler = on_alarm };
    sigaction(SIGALRM, &sa, NULL);
    alarm(WATCHDOG_SECS);

    for (int round = 0; round < ROUNDS; round++) {
        unlink(path);
        if (mkfifo(path, 0666) < 0) {
            perror("fifo_open_creat_deadlock: mkfifo");
            return 1;
        }
        // Alternate which side gets a head start so both interleavings are
        // exercised; writer-first is the order that deadlocked.
        pid_t first, second;
        int writer_first = round & 1;
        first = writer_first ? spawn_writer() : spawn_reader();
        // A tiny stagger makes the head start real more often, without
        // serializing the opens completely.
        if (round % 4 < 2)
            usleep(1000);
        second = writer_first ? spawn_reader() : spawn_writer();
        if (first < 0 || second < 0) {
            perror("fifo_open_creat_deadlock: fork");
            return 1;
        }
        if (reap(first, writer_first ? "writer" : "reader", round) < 0 ||
                reap(second, writer_first ? "reader" : "writer", round) < 0)
            return 1;
        if (round % 50 == 0)
            say("fifo_open_creat_deadlock: round %d/%d\n", round, ROUNDS);
    }
    unlink(path);
    alarm(0);
    printf("fifo_open_creat_deadlock: PASS (%d rounds)\n", ROUNDS);
    return 0;
}
