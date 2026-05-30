#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void fail_errno(const char *what) {
    perror(what);
    exit(1);
}

static void fail(const char *what) {
    fprintf(stderr, "%s\n", what);
    exit(1);
}

static bool recv_and_check_ttl(int recv_fd) {
    char data[32];
    char control[256];
    struct iovec iov = {.iov_base = data, .iov_len = sizeof(data)};
    struct msghdr msg = {
        .msg_iov = &iov,
        .msg_iovlen = 1,
        .msg_control = control,
        .msg_controllen = sizeof(control),
    };
    ssize_t got = recvmsg(recv_fd, &msg, 0);
    if (got < 0)
        fail_errno("recvmsg(IP_RECVTTL)");
    for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
            cmsg != NULL;
            cmsg = CMSG_NXTHDR(&msg, cmsg)) {
        if (cmsg->cmsg_level == IPPROTO_IP && cmsg->cmsg_type == IP_TTL) {
            int ttl = 0;
            memcpy(&ttl, CMSG_DATA(cmsg), sizeof(ttl));
            if (ttl <= 0)
                fail("invalid IPv4 TTL ancillary data");
            return true;
        }
    }
    return false;
}

static bool recv_and_check_hoplimit(int recv_fd) {
    char data[32];
    char control[256];
    struct iovec iov = {.iov_base = data, .iov_len = sizeof(data)};
    struct msghdr msg = {
        .msg_iov = &iov,
        .msg_iovlen = 1,
        .msg_control = control,
        .msg_controllen = sizeof(control),
    };
    ssize_t got = recvmsg(recv_fd, &msg, 0);
    if (got < 0)
        fail_errno("recvmsg(IPV6_RECVHOPLIMIT)");
    for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
            cmsg != NULL;
            cmsg = CMSG_NXTHDR(&msg, cmsg)) {
        if (cmsg->cmsg_level == IPPROTO_IPV6 && cmsg->cmsg_type == IPV6_HOPLIMIT) {
            int hoplimit = 0;
            memcpy(&hoplimit, CMSG_DATA(cmsg), sizeof(hoplimit));
            if (hoplimit <= 0)
                fail("invalid IPv6 hoplimit ancillary data");
            return true;
        }
    }
    return false;
}

static void test_ipv4_recvttl(void) {
    int recv_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (recv_fd < 0)
        fail_errno("socket(AF_INET)");
    int send_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (send_fd < 0)
        fail_errno("socket(AF_INET sender)");

    int one = 1;
    if (setsockopt(recv_fd, IPPROTO_IP, IP_RECVTTL, &one, sizeof(one)) < 0)
        fail_errno("setsockopt(IP_RECVTTL)");

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = 0,
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };
    if (bind(recv_fd, (struct sockaddr *) &addr, sizeof(addr)) < 0)
        fail_errno("bind(AF_INET)");
    socklen_t addr_len = sizeof(addr);
    if (getsockname(recv_fd, (struct sockaddr *) &addr, &addr_len) < 0)
        fail_errno("getsockname(AF_INET)");

    static const char payload[] = "ttl";
    if (sendto(send_fd, payload, sizeof(payload), 0,
                (struct sockaddr *) &addr, sizeof(addr)) < 0)
        fail_errno("sendto(AF_INET)");

    if (!recv_and_check_ttl(recv_fd))
        fail("missing IPv4 TTL ancillary data");

    close(send_fd);
    close(recv_fd);
}

static void test_ipv6_recvhoplimit(void) {
    int recv_fd = socket(AF_INET6, SOCK_DGRAM, 0);
    if (recv_fd < 0)
        fail_errno("socket(AF_INET6)");
    int send_fd = socket(AF_INET6, SOCK_DGRAM, 0);
    if (send_fd < 0)
        fail_errno("socket(AF_INET6 sender)");

    int one = 1;
    if (setsockopt(recv_fd, IPPROTO_IPV6, IPV6_RECVHOPLIMIT, &one, sizeof(one)) < 0)
        fail_errno("setsockopt(IPV6_RECVHOPLIMIT)");

    struct sockaddr_in6 addr = {
        .sin6_family = AF_INET6,
        .sin6_port = 0,
        .sin6_addr = IN6ADDR_LOOPBACK_INIT,
    };
    if (bind(recv_fd, (struct sockaddr *) &addr, sizeof(addr)) < 0)
        fail_errno("bind(AF_INET6)");
    socklen_t addr_len = sizeof(addr);
    if (getsockname(recv_fd, (struct sockaddr *) &addr, &addr_len) < 0)
        fail_errno("getsockname(AF_INET6)");

    static const char payload[] = "hoplimit";
    if (sendto(send_fd, payload, sizeof(payload), 0,
                (struct sockaddr *) &addr, sizeof(addr)) < 0)
        fail_errno("sendto(AF_INET6)");

    if (!recv_and_check_hoplimit(recv_fd))
        fail("missing IPv6 hoplimit ancillary data");

    close(send_fd);
    close(recv_fd);
}

int main(void) {
    test_ipv4_recvttl();
    test_ipv6_recvhoplimit();
    puts("recvmsg ancillary metadata ok");
    return 0;
}
