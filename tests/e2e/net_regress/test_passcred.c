#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

static void fail(const char *why) {
    fprintf(stderr, "%s\n", why);
    exit(1);
}

int main(void) {
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) < 0)
        fail("socketpair failed");

    int enabled = 1;
    if (setsockopt(pair[1], SOL_SOCKET, SO_PASSCRED, &enabled, sizeof(enabled)) < 0)
        fail("setsockopt SO_PASSCRED failed");

    socklen_t enabled_len = sizeof(enabled);
    enabled = 0;
    if (getsockopt(pair[1], SOL_SOCKET, SO_PASSCRED, &enabled, &enabled_len) < 0)
        fail("getsockopt SO_PASSCRED failed");
    if (enabled != 1)
        fail("SO_PASSCRED not enabled");

    static const char byte = 'X';
    if (write(pair[0], &byte, sizeof(byte)) != (ssize_t) sizeof(byte))
        fail("write failed");

    char payload = 0;
    struct iovec iov = {
        .iov_base = &payload,
        .iov_len = sizeof(payload),
    };
    char control[CMSG_SPACE(sizeof(struct ucred))];
    memset(control, 0, sizeof(control));
    struct msghdr msg = {
        .msg_iov = &iov,
        .msg_iovlen = 1,
        .msg_control = control,
        .msg_controllen = sizeof(control),
    };

    ssize_t got = recvmsg(pair[1], &msg, 0);
    if (got != (ssize_t) sizeof(payload))
        fail("recvmsg length mismatch");
    if (payload != byte)
        fail("recvmsg payload mismatch");

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    if (cmsg == NULL)
        fail("missing credentials cmsg");
    if (cmsg->cmsg_level != SOL_SOCKET)
        fail("wrong credentials cmsg level");
    if (cmsg->cmsg_type != SCM_CREDENTIALS)
        fail("wrong credentials cmsg type");
    if (cmsg->cmsg_len != CMSG_LEN(sizeof(struct ucred)))
        fail("wrong credentials cmsg length");

    struct ucred *cred = (struct ucred *) CMSG_DATA(cmsg);
    if (cred->pid != getpid())
        fail("wrong credential pid");
    if (cred->uid != geteuid())
        fail("wrong credential uid");
    if (cred->gid != getegid())
        fail("wrong credential gid");

    puts("SO_PASSCRED ok");

    close(pair[1]);
    close(pair[0]);
    return 0;
}
