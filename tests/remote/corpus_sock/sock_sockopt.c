/*
 * sock_sockopt.c — SO_* option get/set round-trips and error corners. Exact
 * buffer-size rounding (SO_RCVBUF/SO_SNDBUF) is unspecified and host-dependent
 * (Linux doubles the request; Darwin does not), so those are asserted only as
 * "positive and at least the floor" invariants, not exact values (cmpxchg
 * discipline). Boolean toggles and SO_RCVTIMEO round-trips ARE pinned.
 */
#include "sock_common.h"

/* a boolean option toggles to 1 after being set */
static void bool_opt(const char *key, int fd, int level, int opt) {
    int one = 1;
    int sr = setsockopt(fd, level, opt, &one, sizeof one);
    int got = -1; socklen_t gl = sizeof got;
    int gr = getsockopt(fd, level, opt, &got, &gl);
    emit_b(key, sr == 0 && gr == 0 && got != 0);
}

static void case_bools(void) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    bool_opt("opt.reuseaddr", s, SOL_SOCKET, SO_REUSEADDR);
    bool_opt("opt.keepalive", s, SOL_SOCKET, SO_KEEPALIVE);
    close(s);
    int u = socket(AF_INET, SOCK_DGRAM, 0);
    bool_opt("opt.broadcast", u, SOL_SOCKET, SO_BROADCAST);
    close(u);
}

/* SO_TYPE / SO_DOMAIN / SO_PROTOCOL report the socket's identity */
static void case_identity(void) {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    int type = -1; socklen_t tl = sizeof type;
    emit_b("opt.so_type", getsockopt(s, SOL_SOCKET, SO_TYPE, &type, &tl) == 0 && type == SOCK_DGRAM);
#ifdef SO_DOMAIN
    int dom = -1; socklen_t dl = sizeof dom;
    emit_b("opt.so_domain", getsockopt(s, SOL_SOCKET, SO_DOMAIN, &dom, &dl) == 0 && dom == AF_INET);
#endif
#ifdef SO_PROTOCOL
    int proto = -1; socklen_t pl = sizeof proto;
    emit_b("opt.so_protocol_queryable", getsockopt(s, SOL_SOCKET, SO_PROTOCOL, &proto, &pl) == 0);
#endif
    close(s);
}

/* SO_RCVBUF/SO_SNDBUF: set a value, read it back; assert only the portable
 * invariant that the result is positive and not below the request floor */
static void case_bufsizes(void) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    int want = 32 * 1024;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, &want, sizeof want);
    int got = 0; socklen_t gl = sizeof got;
    int gr = getsockopt(s, SOL_SOCKET, SO_RCVBUF, &got, &gl);
    emit_b("opt.rcvbuf_positive", gr == 0 && got > 0);
    emit_b("opt.rcvbuf_at_least_request", gr == 0 && got >= want);
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, &want, sizeof want);
    int gots = 0; socklen_t gls = sizeof gots;
    int grs = getsockopt(s, SOL_SOCKET, SO_SNDBUF, &gots, &gls);
    emit_b("opt.sndbuf_positive", grs == 0 && gots > 0);
    close(s);
}

/* SO_RCVTIMEO round-trips the timeval through iSH's own marshalling */
static void case_rcvtimeo(void) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct timeval tv = { .tv_sec = 1, .tv_usec = 500000 };
    int sr = setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    emit("opt.rcvtimeo_set", "%s", res_errno(sr));
    struct timeval got;
    memset(&got, 0, sizeof got);
    socklen_t gl = sizeof got;
    int gr = getsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &got, &gl);
    emit_b("opt.rcvtimeo_roundtrip",
           gr == 0 && got.tv_sec == 1 && got.tv_usec == 500000);
    close(s);
}

/* SO_ACCEPTCONN flips to 1 after listen() */
static void case_acceptconn(void) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1; setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in sa; sock_inet_addr(&sa, 0);
    bind(s, (struct sockaddr *) &sa, sizeof sa);
    int before = -1; socklen_t bl = sizeof before;
    getsockopt(s, SOL_SOCKET, SO_ACCEPTCONN, &before, &bl);
    listen(s, 4);
    int after = -1; socklen_t al = sizeof after;
    getsockopt(s, SOL_SOCKET, SO_ACCEPTCONN, &after, &al);
    emit_b("opt.acceptconn_before_listen_0", before == 0);
    emit_b("opt.acceptconn_after_listen_1", after == 1);
    close(s);
}

/* error corners */
static void case_errors(void) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    /* SO_REUSEADDR with a too-short value -> EINVAL */
    char tiny = 1;
    errno = 0;
    int sr = setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &tiny, sizeof tiny);
    emit("opt.set_short_value", "%s", sr < 0 ? errno_name(errno) : "OK");
    /* getsockopt/setsockopt on a non-socket fd -> ENOTSOCK */
    int devnull = open("/dev/null", O_RDONLY);
    if (devnull >= 0) {
        int v = 0; socklen_t vl = sizeof v;
        errno = 0;
        int gr = getsockopt(devnull, SOL_SOCKET, SO_TYPE, &v, &vl);
        emit("opt.getsockopt_non_socket", "%s", gr < 0 ? errno_name(errno) : "OK");
        close(devnull);
    }
    close(s);
}

int main(int argc, char **argv) {
    sock_init(argc, argv, "sockopt");
    case_bools();
    case_identity();
    case_bufsizes();
    case_rcvtimeo();
    case_acceptconn();
    case_errors();
    return 0;
}
