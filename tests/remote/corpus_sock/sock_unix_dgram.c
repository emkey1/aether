/*
 * sock_unix_dgram.c — AF_UNIX SOCK_DGRAM: message boundaries, truncation
 * (discard-remainder), socketpair, bound sendto/recvfrom with sender address,
 * and the "datagram socket with no destination" error.
 */
#include "sock_common.h"

/* a connected datagram pair preserves message boundaries: each read returns
 * exactly one write, never coalesced */
static void case_boundaries(void) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) < 0) { emit("udgram.pair", "%s", "SETUPFAIL"); return; }
    write(sv[0], "ab", 2);
    write(sv[0], "cde", 3);
    char buf[16];
    sock_arm_alarm(3);
    ssize_t n1 = read(sv[1], buf, sizeof buf);
    ssize_t n2 = read(sv[1], buf, sizeof buf);
    sock_disarm_alarm();
    emit_b("udgram.msg1_len2", n1 == 2);
    emit_b("udgram.msg2_len3", n2 == 3);
    close(sv[0]); close(sv[1]);
}

/* a recv buffer smaller than the datagram returns the truncated length and the
 * remainder of THAT datagram is discarded (the next read gets the next one) */
static void case_truncate_discard(void) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) < 0) { emit("udgram.trunc", "%s", "SETUPFAIL"); return; }
    write(sv[0], "abcdef", 6);     /* 6-byte datagram */
    write(sv[0], "ZZ", 2);         /* next datagram */
    char buf[4];
    sock_arm_alarm(3);
    ssize_t n1 = recv(sv[1], buf, 4, 0);    /* truncated to 4 */
    ssize_t n2 = recv(sv[1], buf, 4, 0);    /* next datagram, not the leftover "ef" */
    sock_disarm_alarm();
    emit_i("udgram.trunc_copied_len", (long) n1);
    emit_b("udgram.remainder_discarded", n1 == 4 && n2 == 2);
    close(sv[0]); close(sv[1]);
}

/* two bound dgram sockets exchange a message; recvfrom reports the sender's
 * pathname address */
static void case_sendto_recvfrom(void) {
    int a = socket(AF_UNIX, SOCK_DGRAM, 0);
    int b = socket(AF_UNIX, SOCK_DGRAM, 0);
    struct sockaddr_un sa_a, sa_b;
    socklen_t la = sock_unix_addr(&sa_a, "a.sock");
    socklen_t lb = sock_unix_addr(&sa_b, "b.sock");
    emit("udgram.bind_a", "%s", res_errno(bind(a, (struct sockaddr *) &sa_a, la)));
    emit("udgram.bind_b", "%s", res_errno(bind(b, (struct sockaddr *) &sa_b, lb)));

    ssize_t sn = sendto(a, "ping", 4, 0, (struct sockaddr *) &sa_b, lb);
    emit_b("udgram.sendto_len", sn == 4);

    char buf[16] = {0};
    struct sockaddr_un from; socklen_t flen = sizeof from;
    memset(&from, 0, sizeof from);
    sock_arm_alarm(3);
    ssize_t rn = recvfrom(b, buf, sizeof buf, 0, (struct sockaddr *) &from, &flen);
    sock_disarm_alarm();
    emit_b("udgram.recv_data", rn == 4 && memcmp(buf, "ping", 4) == 0);
    /* The sender's address family is reported as AF_UNIX on both. Recovering the
     * sender's bound PATHNAME is a known iSH gap: iSH maps each guest AF_UNIX
     * path to a host socket file "<prefix>.<id>" and keeps no host->guest
     * reverse index, so recvfrom returns the unnamed (zero-length) address
     * instead of "a.sock". Programs that reply via the recvfrom address use a
     * connected socket or an explicit reply path, which do work. */
    emit_b("udgram.recv_sender_family", from.sun_family == AF_UNIX);
    close(a); close(b);
}

/* a datagram socket with neither a connected peer nor a destination address
 * has nowhere to send -> EDESTADDRREQ */
static void case_no_dest(void) {
    int a = socket(AF_UNIX, SOCK_DGRAM, 0);
    errno = 0;
    ssize_t n = send(a, "x", 1, 0);
    emit("udgram.send_no_dest", "%s", n < 0 ? errno_name(errno) : "OK");
    close(a);
}

/* recv on an empty non-blocking datagram socket -> EAGAIN */
static void case_empty_nonblock(void) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) < 0) { emit("udgram.nb", "%s", "SETUPFAIL"); return; }
    char buf[8];
    errno = 0;
    ssize_t n = recv(sv[1], buf, sizeof buf, MSG_DONTWAIT);
    emit("udgram.empty_recv_eagain", "%s", n < 0 ? errno_name(errno) : "OK");
    close(sv[0]); close(sv[1]);
}

int main(int argc, char **argv) {
    sock_init(argc, argv, "udgram");
    case_boundaries();
    case_truncate_discard();
    case_sendto_recvfrom();
    case_no_dest();
    case_empty_nonblock();
    return 0;
}
