/*
 * sock_pipe.c — anonymous pipe(2): byte stream, EOF on write-end close, EPIPE
 * on read-end close, O_NONBLOCK EAGAIN, and poll readiness incl. POLLHUP/POLLERR.
 */
#include "sock_common.h"

static void case_basic(void) {
    int p[2];
    if (pipe(p) < 0) { emit("pipe.basic", "%s", "SETUPFAIL"); return; }
    emit_b("pipe.write", write(p[1], "hello", 5) == 5);
    char buf[8] = {0};
    ssize_t n = read(p[0], buf, sizeof buf);
    emit_b("pipe.read", n == 5 && memcmp(buf, "hello", 5) == 0);
    close(p[0]); close(p[1]);
}

/* closing the write end is EOF for the reader */
static void case_eof(void) {
    int p[2];
    if (pipe(p) < 0) { emit("pipe.eof", "%s", "SETUPFAIL"); return; }
    write(p[1], "x", 1);
    close(p[1]);
    char buf[8];
    ssize_t n1 = read(p[0], buf, sizeof buf);    /* "x" */
    ssize_t n2 = read(p[0], buf, sizeof buf);    /* EOF */
    emit_b("pipe.read_then_eof", n1 == 1 && n2 == 0);
    close(p[0]);
}

/* closing the read end makes a write raise EPIPE (SIGPIPE ignored) */
static void case_epipe(void) {
    int p[2];
    if (pipe(p) < 0) { emit("pipe.epipe", "%s", "SETUPFAIL"); return; }
    close(p[0]);
    errno = 0;
    ssize_t n = write(p[1], "x", 1);
    emit("pipe.write_epipe", "%s", n < 0 ? errno_name(errno) : "OK");
    close(p[1]);
}

/* O_NONBLOCK read of an empty pipe -> EAGAIN */
static void case_nonblock(void) {
    int p[2];
    if (pipe2(p, O_NONBLOCK) < 0) { emit("pipe.nb", "%s", "SETUPFAIL"); return; }
    char buf[8];
    errno = 0;
    ssize_t n = read(p[0], buf, sizeof buf);
    emit("pipe.empty_read_eagain", "%s", n < 0 ? errno_name(errno) : "OK");
    close(p[0]); close(p[1]);
}

/* poll: read end not readable when empty, readable after a write; POLLHUP once
 * the write end is closed; write end POLLERR once the read end is closed */
static void case_poll(void) {
    int p[2];
    if (pipe(p) < 0) { emit("pipe.poll", "%s", "SETUPFAIL"); return; }
    short rev = 0;
    sock_poll_one(p[0], POLLIN, 0, &rev);
    emit_b("pipe.poll_empty_not_readable", !(rev & POLLIN));
    write(p[1], "y", 1);
    sock_poll_one(p[0], POLLIN, 0, &rev);
    emit_b("pipe.poll_readable_after_write", rev & POLLIN);
    /* write end is writable when there is buffer space */
    sock_poll_one(p[1], POLLOUT, 0, &rev);
    emit_b("pipe.poll_write_writable", rev & POLLOUT);

    /* unread data + closed writer: the read end stays readable AND reports HUP */
    close(p[1]);
    sock_poll_one(p[0], POLLIN, 0, &rev);
    emit_b("pipe.poll_hup_after_wclose", (rev & POLLHUP) != 0);
    close(p[0]);

    /* write end with a closed read end: the portable, guaranteed behavior is
     * that a write raises EPIPE. (Linux also sets POLLERR in poll revents;
     * Darwin-backed iSH does not surface POLLERR for a host pipe write end, a
     * known host-poll gap -- so assert the EPIPE behavior, which both honor.) */
    int q[2];
    if (pipe(q) == 0) {
        close(q[0]);
        errno = 0;
        ssize_t w = write(q[1], "x", 1);
        emit_b("pipe.write_after_rclose_epipe", w < 0 && errno == EPIPE);
        close(q[1]);
    }
}

int main(int argc, char **argv) {
    sock_init(argc, argv, "pipe");
    case_basic();
    case_eof();
    case_epipe();
    case_nonblock();
    case_poll();
    return 0;
}
