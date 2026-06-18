/*
 * sock_peercred.c — SO_PEERCRED on AF_UNIX sockets. Both ends of a socketpair
 * and of a self-connected stream belong to THIS process, so the peer creds must
 * equal our own. mint runs as a non-root user and iSH as root, so we never emit
 * raw uid/gid/pid — only the derived invariants cred==our own ids, which hold
 * regardless of what those ids actually are.
 */
#include "sock_common.h"

struct ucred_compat { pid_t pid; uid_t uid; gid_t gid; };

static int get_peercred(int fd, struct ucred_compat *uc) {
#ifdef SO_PEERCRED
    socklen_t l = sizeof *uc;
    return getsockopt(fd, SOL_SOCKET, SO_PEERCRED, uc, &l);
#else
    (void) fd; (void) uc; errno = ENOPROTOOPT; return -1;
#endif
}

static void emit_cred_is_self(const char *key, int fd) {
    struct ucred_compat uc;
    memset(&uc, 0, sizeof uc);
    int r = get_peercred(fd, &uc);
    if (r < 0) { emit(key, "%s", errno_name(errno)); return; }
    /* derived, host-independent: the peer is us */
    emit_b(key, uc.pid == getpid() && uc.uid == geteuid() && uc.gid == getegid());
}

/* socketpair: both fds are this process, so both report our creds */
static void case_socketpair(void) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) { emit("cred.pair", "%s", "SETUPFAIL"); return; }
    emit_cred_is_self("cred.pair_end0_is_self", sv[0]);
    emit_cred_is_self("cred.pair_end1_is_self", sv[1]);
    close(sv[0]); close(sv[1]);
}

/* a self-connected stream (listener + client in the same process): both the
 * accepted and the client socket report our creds */
static void case_connected(void) {
    int lst = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un sa;
    socklen_t alen = sock_unix_addr(&sa, "cred.sock");
    if (bind(lst, (struct sockaddr *) &sa, alen) < 0 || listen(lst, 4) < 0) {
        emit("cred.connected", "%s", "SETUPFAIL"); close(lst); return;
    }
    int cli = socket(AF_UNIX, SOCK_STREAM, 0);
    if (connect(cli, (struct sockaddr *) &sa, alen) < 0) {
        emit("cred.connected", "%s", "CONNFAIL"); close(lst); close(cli); return;
    }
    sock_arm_alarm(3);
    int srv = accept(lst, NULL, NULL);
    sock_disarm_alarm();
    if (srv < 0) { emit("cred.connected", "%s", "ACCEPTFAIL"); close(lst); close(cli); return; }
    emit_cred_is_self("cred.accepted_peer_is_self", srv);
    emit_cred_is_self("cred.client_peer_is_self", cli);
    close(srv); close(cli); close(lst);
}

/* SO_PASSCRED is an AF_UNIX/AF_NETLINK option; on a plain AF_INET socket it is
 * rejected, but the exact errno is kernel-version-dependent (older Linux and
 * iSH use ENOPROTOOPT, newer Linux uses EOPNOTSUPP). Assert only the guaranteed
 * invariant: it is rejected (not silently accepted). */
static void case_passcred_inet(void) {
#ifdef SO_PASSCRED
    int s = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    errno = 0;
    int r = setsockopt(s, SOL_SOCKET, SO_PASSCRED, &one, sizeof one);
    emit_b("cred.passcred_on_inet_rejected",
           r < 0 && (errno == ENOPROTOOPT || errno == EOPNOTSUPP));
    close(s);
#endif
}

int main(int argc, char **argv) {
    sock_init(argc, argv, "peercred");
    case_socketpair();
    case_connected();
    case_passcred_inet();
    return 0;
}
