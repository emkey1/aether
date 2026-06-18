/*
 * sock_inet.c — AF_INET over 127.0.0.1: TCP bind(port 0)/listen/connect/accept
 * + data, UDP sendto/recvfrom + boundaries, getsockname assigns an ephemeral
 * port, connect to a closed port -> ECONNREFUSED. Ports are never emitted (they
 * vary) — only derived invariants.
 */
#include "sock_common.h"

/* discover the ephemeral port a socket was auto-bound to */
static uint16_t bound_port(int fd) {
    struct sockaddr_in sa; socklen_t l = sizeof sa;
    if (getsockname(fd, (struct sockaddr *) &sa, &l) < 0)
        return 0;
    return ntohs(sa.sin_port);
}

/* full TCP loopback round trip */
static void case_tcp(void) {
    int lst = socket(AF_INET, SOCK_STREAM, 0);
    if (lst < 0) { emit("inet.tcp", "%s", "NOSOCK"); return; }
    int one = 1;
    setsockopt(lst, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in sa;
    sock_inet_addr(&sa, 0);                       /* port 0 -> ephemeral */
    emit("inet.tcp.bind", "%s", res_errno(bind(lst, (struct sockaddr *) &sa, sizeof sa)));
    emit("inet.tcp.listen", "%s", res_errno(listen(lst, 8)));
    uint16_t port = bound_port(lst);
    emit_b("inet.tcp.ephemeral_port_assigned", port != 0);

    int cli = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in ca;
    sock_inet_addr(&ca, port);
    sock_arm_alarm(3);
    int cr = connect(cli, (struct sockaddr *) &ca, sizeof ca);
    int srv = accept(lst, NULL, NULL);
    sock_disarm_alarm();
    emit_b("inet.tcp.connect_accept", cr == 0 && srv >= 0);

    if (srv >= 0) {
        write(cli, "hello", 5);
        char buf[16] = {0};
        sock_arm_alarm(3);
        ssize_t n = read(srv, buf, sizeof buf);
        sock_disarm_alarm();
        emit_b("inet.tcp.data", n == 5 && memcmp(buf, "hello", 5) == 0);

        /* getpeername on the accepted socket names the client on 127.0.0.1 */
        struct sockaddr_in pn; socklen_t pl = sizeof pn;
        memset(&pn, 0, sizeof pn);
        int pr = getpeername(srv, (struct sockaddr *) &pn, &pl);
        emit_b("inet.tcp.peer_is_loopback",
               pr == 0 && pn.sin_family == AF_INET &&
               pn.sin_addr.s_addr == htonl(INADDR_LOOPBACK));
        close(srv);
    }
    close(cli); close(lst);
}

/* SO_TYPE / SO_ERROR on a fresh TCP socket */
static void case_sockstate(void) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    int type = -1; socklen_t tl = sizeof type;
    int gr = getsockopt(s, SOL_SOCKET, SO_TYPE, &type, &tl);
    emit_b("inet.so_type_stream", gr == 0 && type == SOCK_STREAM);
    int err = -1; socklen_t el = sizeof err;
    int er = getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &el);
    emit_b("inet.so_error_fresh_zero", er == 0 && err == 0);
    close(s);
}

/* UDP loopback: bound recv socket gets a sendto, with boundaries preserved */
static void case_udp(void) {
    int rcv = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in sa;
    sock_inet_addr(&sa, 0);
    emit("inet.udp.bind", "%s", res_errno(bind(rcv, (struct sockaddr *) &sa, sizeof sa)));
    uint16_t port = bound_port(rcv);
    emit_b("inet.udp.ephemeral_port_assigned", port != 0);

    int snd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in da;
    sock_inet_addr(&da, port);
    sendto(snd, "one", 3, 0, (struct sockaddr *) &da, sizeof da);
    sendto(snd, "two!", 4, 0, (struct sockaddr *) &da, sizeof da);
    char buf[16];
    sock_arm_alarm(3);
    ssize_t n1 = recv(rcv, buf, sizeof buf, 0);
    ssize_t n2 = recv(rcv, buf, sizeof buf, 0);
    sock_disarm_alarm();
    emit_b("inet.udp.boundaries", n1 == 3 && n2 == 4);
    close(snd); close(rcv);
}

/* connecting TCP to a port with no listener on loopback -> ECONNREFUSED */
static void case_refused(void) {
    /* grab a port, bind+listen, then close so nothing listens on it */
    int tmp = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa;
    sock_inet_addr(&sa, 0);
    bind(tmp, (struct sockaddr *) &sa, sizeof sa);
    uint16_t port = bound_port(tmp);
    close(tmp);

    int c = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in ca;
    sock_inet_addr(&ca, port);
    sock_arm_alarm(3);
    errno = 0;
    int r = connect(c, (struct sockaddr *) &ca, sizeof ca);
    sock_disarm_alarm();
    emit("inet.tcp.connect_refused", "%s", r < 0 ? errno_name(errno) : "OK");
    close(c);
}

int main(int argc, char **argv) {
    sock_init(argc, argv, "inet");
    case_tcp();
    case_sockstate();
    case_udp();
    case_refused();
    return 0;
}
