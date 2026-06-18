/*
 * sock_shutdown.c — shutdown(2) semantics: SHUT_RD makes future reads return
 * EOF; SHUT_WR is seen as EOF by the peer and makes local writes EPIPE; an
 * out-of-range `how` is EINVAL; shutdown on an unconnected socket is ENOTCONN.
 */
#include "sock_common.h"

static int stream_pair(int sv[2]) { return socketpair(AF_UNIX, SOCK_STREAM, 0, sv); }

/* SHUT_WR: the peer drains buffered bytes then sees EOF; a local write after
 * SHUT_WR raises EPIPE */
static void case_shut_wr(void) {
    int sv[2];
    if (stream_pair(sv) < 0) { emit("shut.wr", "%s", "SETUPFAIL"); return; }
    write(sv[0], "hi", 2);
    emit("shut.wr_ret", "%s", res_errno(shutdown(sv[0], SHUT_WR)));
    char buf[8];
    sock_arm_alarm(3);
    ssize_t n1 = read(sv[1], buf, sizeof buf);   /* "hi" */
    ssize_t n2 = read(sv[1], buf, sizeof buf);   /* EOF */
    sock_disarm_alarm();
    emit_b("shut.wr_peer_drains_then_eof", n1 == 2 && n2 == 0);
    errno = 0;
    ssize_t w = write(sv[0], "x", 1);
    emit("shut.wr_local_write_epipe", "%s", w < 0 ? errno_name(errno) : "OK");
    close(sv[0]); close(sv[1]);
}

/* SHUT_RD: future reads on the shut side return EOF (0) */
static void case_shut_rd(void) {
    int sv[2];
    if (stream_pair(sv) < 0) { emit("shut.rd", "%s", "SETUPFAIL"); return; }
    emit("shut.rd_ret", "%s", res_errno(shutdown(sv[1], SHUT_RD)));
    /* peer writes; the SHUT_RD side should still read EOF, not the data */
    write(sv[0], "data", 4);
    sock_sleep_ms(10);
    char buf[8];
    sock_arm_alarm(3);
    ssize_t n = read(sv[1], buf, sizeof buf);
    sock_disarm_alarm();
    emit_b("shut.rd_reads_eof", n == 0);
    close(sv[0]); close(sv[1]);
}

/* an out-of-range `how` -> EINVAL */
static void case_bad_how(void) {
    int sv[2];
    if (stream_pair(sv) < 0) { emit("shut.badhow", "%s", "SETUPFAIL"); return; }
    errno = 0;
    int r = shutdown(sv[0], 99);
    emit("shut.bad_how_einval", "%s", r < 0 ? errno_name(errno) : "OK");
    close(sv[0]); close(sv[1]);
}

/* shutdown on a never-connected socket -> ENOTCONN */
static void case_unconnected(void) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    errno = 0;
    int r = shutdown(s, SHUT_RDWR);
    emit("shut.unconnected_enotconn", "%s", r < 0 ? errno_name(errno) : "OK");
    close(s);
}

int main(int argc, char **argv) {
    sock_init(argc, argv, "shutdown");
    case_shut_wr();
    case_shut_rd();
    case_bad_how();
    case_unconnected();
    return 0;
}
