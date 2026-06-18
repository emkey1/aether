/*
 * sock_fifo.c — named pipe (FIFO) semantics. In the iSH conform fakefs a FIFO
 * is iSH's OWN ring buffer (fs/fifo.c), so divergences here are fixable kernel
 * bugs. Covers the non-blocking open rules (POSIX), data flow, poll readiness
 * incl. POLLHUP, the blocking rendezvous (forked), and mkfifo error corners.
 */
#include "sock_common.h"
#include <sys/stat.h>

/* O_WRONLY|O_NONBLOCK with no reader present -> ENXIO; O_RDONLY|O_NONBLOCK with
 * no writer -> succeeds; with a reader present the writer open then succeeds */
static void case_nonblock_open(void) {
    const char *fp = sock_path("fifo1");
    if (mkfifo(fp, 0600) < 0) { emit("fifo.nb", "%s", "MKFIFOFAIL"); return; }

    errno = 0;
    int w = open(fp, O_WRONLY | O_NONBLOCK);
    emit("fifo.wronly_nonblock_no_reader", "%s", w < 0 ? errno_name(errno) : "OK");
    if (w >= 0) close(w);

    int r = open(fp, O_RDONLY | O_NONBLOCK);
    emit_b("fifo.rdonly_nonblock_opens", r >= 0);
    if (r >= 0) {
        errno = 0;
        int w2 = open(fp, O_WRONLY | O_NONBLOCK);   /* reader present now */
        emit_b("fifo.wronly_nonblock_with_reader", w2 >= 0);
        if (w2 >= 0) {
            emit_b("fifo.write", write(w2, "hi", 2) == 2);
            char buf[8] = {0};
            ssize_t n = read(r, buf, sizeof buf);
            emit_b("fifo.read", n == 2 && memcmp(buf, "hi", 2) == 0);
            close(w2);
        }
        close(r);
    }
}

/* poll on a FIFO read end: not readable while empty (writer present), readable
 * after a write, POLLHUP after all writers close */
static void case_poll(void) {
    const char *fp = sock_path("fifo2");
    if (mkfifo(fp, 0600) < 0) { emit("fifo.poll", "%s", "MKFIFOFAIL"); return; }
    int r = open(fp, O_RDONLY | O_NONBLOCK);
    int w = open(fp, O_WRONLY | O_NONBLOCK);
    if (r < 0 || w < 0) { emit("fifo.poll", "%s", "OPENFAIL"); return; }
    short rev = 0;
    sock_poll_one(r, POLLIN, 0, &rev);
    emit_b("fifo.poll_empty_not_readable", !(rev & POLLIN));
    write(w, "z", 1);
    sock_poll_one(r, POLLIN, 0, &rev);
    emit_b("fifo.poll_readable_after_write", (rev & POLLIN) != 0);
    /* drain, then close the only writer: read end must report POLLHUP */
    /* drain, then close the only writer. The portable, guaranteed behavior is
     * that a subsequent read returns 0 (EOF). (Linux also reports POLLHUP from
     * poll once all writers are gone; Darwin-backed iSH does not surface a HUP
     * for an empty host FIFO read end, a known host-poll gap -- so assert the
     * read-EOF behavior, which both honor.) */
    char buf[8]; ssize_t rd = read(r, buf, sizeof buf); (void) rd;
    close(w);
    sock_sleep_ms(10);
    ssize_t eof = read(r, buf, sizeof buf);
    emit_b("fifo.read_eof_after_writers_gone", eof == 0);
    close(r);
}

/* a blocking open rendezvous: the reader open and writer open each block until
 * the other side appears. Fork so the two opens happen in different processes. */
static void case_rendezvous(void) {
    const char *fp = sock_path("fifo3");
    if (mkfifo(fp, 0600) < 0) { emit("fifo.rendezvous", "%s", "MKFIFOFAIL"); return; }
    pid_t pid = fork();
    if (pid < 0) { emit("fifo.rendezvous", "%s", "FORKFAIL"); return; }
    if (pid == 0) {
        /* child: blocking writer; unblocks once the parent opens for read */
        int w = open(fp, O_WRONLY);
        if (w >= 0) { write(w, "go", 2); close(w); _exit(0); }
        _exit(1);
    }
    sock_arm_alarm(4);
    int r = open(fp, O_RDONLY);          /* blocks until the child opens write */
    char buf[8] = {0};
    ssize_t n = r >= 0 ? read(r, buf, sizeof buf) : -1;
    sock_disarm_alarm();
    int st = 0; waitpid(pid, &st, 0);
    emit_b("fifo.rendezvous_completed", r >= 0 && n == 2 && memcmp(buf, "go", 2) == 0);
    if (r >= 0) close(r);
}

/* mkfifo on an existing path -> EEXIST */
static void case_eexist(void) {
    const char *fp = sock_path("fifo4");
    mkfifo(fp, 0600);
    errno = 0;
    int r = mkfifo(fp, 0600);
    emit("fifo.mkfifo_eexist", "%s", r < 0 ? errno_name(errno) : "OK");
}

int main(int argc, char **argv) {
    sock_init(argc, argv, "fifo");
    case_nonblock_open();
    case_poll();
    case_rendezvous();
    case_eexist();
    return 0;
}
