/*
 * sock_msgflags.c — send/recv FLAG semantics: MSG_PEEK (non-consuming read),
 * MSG_DONTWAIT (per-call non-blocking), MSG_TRUNC (datagram real-length return +
 * recvmsg msg_flags), MSG_WAITALL (fill the whole buffer on a stream).
 *
 * The MSG_TRUNC datagram case is the headline: on Linux a too-small recv with
 * MSG_TRUNC returns the REAL datagram length (not the truncated copy length).
 * Darwin does not, so a host-backed iSH would leak the Darwin behavior here.
 */
#include "sock_common.h"

static int dgram_pair(int sv[2]) { return socketpair(AF_UNIX, SOCK_DGRAM, 0, sv); }
static int stream_pair(int sv[2]) { return socketpair(AF_UNIX, SOCK_STREAM, 0, sv); }

/* MSG_PEEK returns data without consuming it; a following plain recv re-reads */
static void case_peek(void) {
    int sv[2];
    if (stream_pair(sv) < 0) { emit("mf.peek", "%s", "SETUPFAIL"); return; }
    write(sv[0], "abcde", 5);
    sock_sleep_ms(10);
    char b1[8] = {0}, b2[8] = {0};
    ssize_t p = recv(sv[1], b1, sizeof b1, MSG_PEEK);
    ssize_t r = recv(sv[1], b2, sizeof b2, 0);
    emit_b("mf.peek_returns_data", p == 5 && memcmp(b1, "abcde", 5) == 0);
    emit_b("mf.peek_nonconsuming", r == 5 && memcmp(b2, "abcde", 5) == 0);
    close(sv[0]); close(sv[1]);
}

/* MSG_DONTWAIT makes a single recv non-blocking on an otherwise blocking fd */
static void case_dontwait(void) {
    int sv[2];
    if (stream_pair(sv) < 0) { emit("mf.dontwait", "%s", "SETUPFAIL"); return; }
    char buf[8];
    errno = 0;
    ssize_t n = recv(sv[1], buf, sizeof buf, MSG_DONTWAIT);
    emit("mf.dontwait_empty_eagain", "%s", n < 0 ? errno_name(errno) : "OK");
    close(sv[0]); close(sv[1]);
}

/* MSG_TRUNC on a datagram recv: the copy is truncated to the buffer but the
 * RETURN VALUE is the real datagram length */
static void case_trunc_dgram(void) {
    int sv[2];
    if (dgram_pair(sv) < 0) { emit("mf.trunc", "%s", "SETUPFAIL"); return; }
    write(sv[0], "abcdefghij", 10);     /* 10-byte datagram */
    char buf[4];
    sock_arm_alarm(3);
    ssize_t n = recv(sv[1], buf, 4, MSG_TRUNC);
    sock_disarm_alarm();
    emit_i("mf.trunc_return_len", (long) n);
    emit_b("mf.trunc_returns_real_len", n == 10);
    /* the datagram is consumed even though truncated: next recv would block, so
     * verify via MSG_DONTWAIT */
    errno = 0;
    ssize_t n2 = recv(sv[1], buf, 4, MSG_DONTWAIT);
    emit_b("mf.trunc_consumed_datagram", n2 < 0 && errno == EAGAIN);
    close(sv[0]); close(sv[1]);
}

/* recvmsg sets MSG_TRUNC in msg_flags when a datagram is truncated */
static void case_recvmsg_trunc_flag(void) {
    int sv[2];
    if (dgram_pair(sv) < 0) { emit("mf.rmsgtrunc", "%s", "SETUPFAIL"); return; }
    write(sv[0], "abcdefghij", 10);
    char buf[4];
    struct iovec iov = { .iov_base = buf, .iov_len = sizeof buf };
    struct msghdr msg;
    memset(&msg, 0, sizeof msg);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    sock_arm_alarm(3);
    ssize_t n = recvmsg(sv[1], &msg, 0);
    sock_disarm_alarm();
    emit_b("mf.recvmsg_copied_4", n == 4);
    emit_b("mf.recvmsg_sets_trunc_flag", (msg.msg_flags & MSG_TRUNC) != 0);
    close(sv[0]); close(sv[1]);
}

/* MSG_WAITALL on a stream returns the full requested count when all bytes are
 * available */
static void case_waitall(void) {
    int sv[2];
    if (stream_pair(sv) < 0) { emit("mf.waitall", "%s", "SETUPFAIL"); return; }
    write(sv[0], "123456", 6);
    sock_sleep_ms(10);
    char buf[6] = {0};
    sock_arm_alarm(3);
    ssize_t n = recv(sv[1], buf, 6, MSG_WAITALL);
    sock_disarm_alarm();
    emit_b("mf.waitall_full", n == 6 && memcmp(buf, "123456", 6) == 0);
    close(sv[0]); close(sv[1]);
}

/* an unknown/garbage flag bit: Linux ignores high unknown recv flag bits for a
 * normal recv (returns the data) — assert recv still succeeds, the spec-safe
 * invariant (the exact handling of reserved bits is not something to pin). */
static void case_oob_absent(void) {
    int sv[2];
    if (stream_pair(sv) < 0) { emit("mf.oob", "%s", "SETUPFAIL"); return; }
    /* recv(MSG_OOB) with no OOB data is rejected, but the exact errno is
     * impl-defined (Linux EINVAL, Darwin EOPNOTSUPP for AF_UNIX which has no
     * OOB) -- assert only the guaranteed invariant: it is rejected. */
    write(sv[0], "x", 1);
    sock_sleep_ms(10);
    char buf[4];
    errno = 0;
    ssize_t n = recv(sv[1], buf, sizeof buf, MSG_OOB);
    emit_b("mf.recv_oob_rejected", n < 0 && (errno == EINVAL || errno == EOPNOTSUPP));
    close(sv[0]); close(sv[1]);
}

int main(int argc, char **argv) {
    sock_init(argc, argv, "msgflags");
    case_peek();
    case_dontwait();
    case_trunc_dgram();
    case_recvmsg_trunc_flag();
    case_waitall();
    case_oob_absent();
    return 0;
}
