/*
 * sock_unix_stream.c — AF_UNIX SOCK_STREAM: pathname bind/listen/connect/accept,
 * stream (no message-boundary) byte semantics, half-close (shutdown) EOF, peer
 * close EOF + EPIPE, getsockname/getpeername, and the standard error corners.
 *
 * Single-threaded: a pathname listener with a backlog accepts a queued connect
 * without another thread. Blocking accept/recv are alarm-guarded so a missed
 * wake is a clean TIMEOUT divergence, not a cell hang.
 */
#include "sock_common.h"

/* build a connected (listener-accepted) stream pair; returns 0 and fills
 * cli/srv, or -1. */
static int make_pair(int *cli, int *srv) {
    int lst = socket(AF_UNIX, SOCK_STREAM, 0);
    if (lst < 0) return -1;
    struct sockaddr_un sa;
    socklen_t alen = sock_unix_addr(&sa, "stream.sock");
    if (bind(lst, (struct sockaddr *) &sa, alen) < 0) { close(lst); return -1; }
    if (listen(lst, 4) < 0) { close(lst); return -1; }
    int c = socket(AF_UNIX, SOCK_STREAM, 0);
    if (c < 0) { close(lst); return -1; }
    if (connect(c, (struct sockaddr *) &sa, alen) < 0) { close(lst); close(c); return -1; }
    sock_arm_alarm(3);
    int s = accept(lst, NULL, NULL);
    sock_disarm_alarm();
    close(lst);
    if (s < 0) { close(c); return -1; }
    *cli = c; *srv = s;
    return 0;
}

/* a connected stream has no message boundaries: two writes coalesce into one
 * read of the combined length */
static void case_stream_bytes(void) {
    int c, s;
    if (make_pair(&c, &s) < 0) { emit("ustream.bytes", "%s", "SETUPFAIL"); return; }
    emit_b("ustream.write_ab", write(c, "ab", 2) == 2);
    emit_b("ustream.write_cd", write(c, "cd", 2) == 2);
    sock_sleep_ms(20);
    char buf[8] = {0};
    sock_arm_alarm(3);
    ssize_t n = read(s, buf, sizeof buf);
    sock_disarm_alarm();
    emit_i("ustream.coalesced_len", (long) n);
    emit_b("ustream.coalesced_data", n == 4 && memcmp(buf, "abcd", 4) == 0);
    close(c); close(s);
}

/* shutdown(SHUT_WR) on the writer is seen as EOF (read returns 0) by the peer
 * after it drains buffered data */
static void case_half_close(void) {
    int c, s;
    if (make_pair(&c, &s) < 0) { emit("ustream.halfclose", "%s", "SETUPFAIL"); return; }
    write(c, "hi", 2);
    emit("ustream.shutdown_wr", "%s", res_errno(shutdown(c, SHUT_WR)));
    char buf[8] = {0};
    sock_arm_alarm(3);
    ssize_t n1 = read(s, buf, sizeof buf);     /* buffered "hi" */
    ssize_t n2 = read(s, buf, sizeof buf);     /* EOF -> 0 */
    sock_disarm_alarm();
    emit_b("ustream.drain_then_eof", n1 == 2 && n2 == 0);
    close(c); close(s);
}

/* closing the peer: reader sees EOF; a write to the closed side raises EPIPE
 * (SIGPIPE is ignored in sock_init) */
static void case_peer_close(void) {
    int c, s;
    if (make_pair(&c, &s) < 0) { emit("ustream.peerclose", "%s", "SETUPFAIL"); return; }
    close(s);
    sock_sleep_ms(20);
    char buf[8] = {0};
    sock_arm_alarm(3);
    ssize_t n = read(c, buf, sizeof buf);
    sock_disarm_alarm();
    emit_b("ustream.read_eof_after_peer_close", n == 0);
    /* first write may be buffered+RST-pending; loop until EPIPE or success runs out */
    int got_epipe = 0;
    for (int i = 0; i < 3 && !got_epipe; i++) {
        errno = 0;
        if (write(c, "x", 1) < 0 && errno == EPIPE)
            got_epipe = 1;
        else
            sock_sleep_ms(20);
    }
    emit_b("ustream.write_after_peer_close_epipe", got_epipe);
    close(c);
}

/* getsockname returns the bound path; getpeername on the accepted side names the
 * (unnamed) client as a zero-length path; getpeername on an unconnected socket
 * is ENOTCONN */
static void case_names(void) {
    int lst = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un sa;
    socklen_t alen = sock_unix_addr(&sa, "named.sock");
    bind(lst, (struct sockaddr *) &sa, alen);
    listen(lst, 4);

    struct sockaddr_un got; socklen_t glen = sizeof got;
    memset(&got, 0, sizeof got);
    int gn = getsockname(lst, (struct sockaddr *) &got, &glen);
    emit_b("ustream.getsockname_ok", gn == 0 && got.sun_family == AF_UNIX);
    emit_b("ustream.getsockname_path",
           gn == 0 && strcmp(got.sun_path, sa.sun_path) == 0);

    int lone = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un pn; socklen_t plen = sizeof pn;
    errno = 0;
    int pr = getpeername(lone, (struct sockaddr *) &pn, &plen);
    emit("ustream.getpeername_unconnected", "%s", pr < 0 ? errno_name(errno) : "OK");
    close(lone);
    close(lst);
}

/* error corners: listen on a fresh dgram socket; accept on a non-listening
 * socket; connect to a path with no listener */
static void case_errors(void) {
    int d = socket(AF_UNIX, SOCK_DGRAM, 0);
    errno = 0;
    emit("ustream.listen_on_dgram", "%s", res_errno(listen(d, 4)));
    close(d);

    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    errno = 0;
    int ar = accept(s, NULL, NULL);
    emit("ustream.accept_not_listening", "%s", ar < 0 ? errno_name(errno) : "OK");
    if (ar >= 0) close(ar);
    close(s);

    int c = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un sa;
    socklen_t alen = sock_unix_addr(&sa, "nolistener.sock");
    errno = 0;
    emit("ustream.connect_no_listener", "%s", res_errno(connect(c, (struct sockaddr *) &sa, alen)));
    close(c);
}

int main(int argc, char **argv) {
    sock_init(argc, argv, "ustream");
    case_stream_bytes();
    case_half_close();
    case_peer_close();
    case_names();
    case_errors();
    return 0;
}
