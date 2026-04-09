#include <errno.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int dump_route_family(int fd, int type, unsigned int seq, unsigned short expect_type) {
    struct {
        struct nlmsghdr nlh;
        struct rtgenmsg gen;
    } req = {
        .nlh = {
            .nlmsg_len = NLMSG_LENGTH(sizeof(struct rtgenmsg)),
            .nlmsg_type = type,
            .nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP,
            .nlmsg_seq = seq,
        },
        .gen = {
            .rtgen_family = AF_UNSPEC,
        },
    };

    struct sockaddr_nl kernel = {
        .nl_family = AF_NETLINK,
    };
    struct iovec iov = {
        .iov_base = &req,
        .iov_len = req.nlh.nlmsg_len,
    };
    struct msghdr msg = {
        .msg_name = &kernel,
        .msg_namelen = sizeof(kernel),
        .msg_iov = &iov,
        .msg_iovlen = 1,
    };
    ssize_t sent = sendmsg(fd, &msg, 0);
    if (sent < 0) {
        perror("sendmsg");
        return 1;
    }

    struct iovec peek_iov = {
        .iov_base = NULL,
        .iov_len = 0,
    };
    struct sockaddr_nl peek_peer = {};
    struct msghdr peek_msg = {
        .msg_name = &peek_peer,
        .msg_namelen = sizeof(peek_peer),
        .msg_iov = &peek_iov,
        .msg_iovlen = 1,
    };
    ssize_t peeked = recvmsg(fd, &peek_msg, MSG_PEEK | MSG_TRUNC);
    if (peeked < 0) {
        perror("recvmsg(MSG_PEEK|MSG_TRUNC)");
        return 1;
    }
    if (peeked == 0) {
        fprintf(stderr, "unexpected EOF on netlink sizing recv\n");
        return 1;
    }

    int saw_expected = 0;
    int saw_done = 0;
    for (;;) {
        char buf[32768];
        struct iovec recv_iov = {
            .iov_base = buf,
            .iov_len = sizeof(buf),
        };
        struct sockaddr_nl peer = {};
        struct msghdr recv_msg = {
            .msg_name = &peer,
            .msg_namelen = sizeof(peer),
            .msg_iov = &recv_iov,
            .msg_iovlen = 1,
        };
        ssize_t got = recvmsg(fd, &recv_msg, 0);
        if (got < 0) {
            perror("recvmsg");
            return 1;
        }
        if (got == 0) {
            fprintf(stderr, "unexpected EOF on netlink dump\n");
            return 1;
        }

        for (struct nlmsghdr *hdr = (struct nlmsghdr *) buf;
             NLMSG_OK(hdr, (unsigned int) got);
             hdr = NLMSG_NEXT(hdr, got)) {
            if (hdr->nlmsg_seq != seq)
                continue;
            if (hdr->nlmsg_type == NLMSG_DONE) {
                if (hdr->nlmsg_len < NLMSG_LENGTH(sizeof(int))) {
                    fprintf(stderr, "netlink done message too short: %u\n", hdr->nlmsg_len);
                    return 1;
                }
                saw_done = 1;
                break;
            }
            if (hdr->nlmsg_type == NLMSG_ERROR) {
                struct nlmsgerr *err = (struct nlmsgerr *) NLMSG_DATA(hdr);
                fprintf(stderr, "netlink error %d\n", err->error);
                return 1;
            }
            if (hdr->nlmsg_type == expect_type)
                saw_expected++;
        }
        if (saw_done)
            break;
    }

    if (saw_expected == 0) {
        fprintf(stderr, "missing expected route dump messages for type %u\n", expect_type);
        return 1;
    }
    return 0;
}

static int dump_routes(int fd, unsigned int seq) {
    struct {
        struct nlmsghdr nlh;
        struct rtmsg rtm;
    } req = {
        .nlh = {
            .nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg)),
            .nlmsg_type = RTM_GETROUTE,
            .nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP,
            .nlmsg_seq = seq,
        },
        .rtm = {
            .rtm_family = AF_INET,
            .rtm_table = RT_TABLE_MAIN,
        },
    };

    struct sockaddr_nl kernel = {
        .nl_family = AF_NETLINK,
    };
    struct iovec iov = {
        .iov_base = &req,
        .iov_len = req.nlh.nlmsg_len,
    };
    struct msghdr msg = {
        .msg_name = &kernel,
        .msg_namelen = sizeof(kernel),
        .msg_iov = &iov,
        .msg_iovlen = 1,
    };
    if (sendmsg(fd, &msg, 0) < 0) {
        perror("sendmsg(route)");
        return 1;
    }

    int saw_route = 0;
    int saw_done = 0;
    for (;;) {
        char buf[32768];
        struct iovec recv_iov = {
            .iov_base = buf,
            .iov_len = sizeof(buf),
        };
        struct sockaddr_nl peer = {};
        struct msghdr recv_msg = {
            .msg_name = &peer,
            .msg_namelen = sizeof(peer),
            .msg_iov = &recv_iov,
            .msg_iovlen = 1,
        };
        ssize_t got = recvmsg(fd, &recv_msg, 0);
        if (got < 0) {
            perror("recvmsg(route)");
            return 1;
        }
        if (got == 0) {
            fprintf(stderr, "unexpected EOF on netlink route dump\n");
            return 1;
        }

        for (struct nlmsghdr *hdr = (struct nlmsghdr *) buf;
             NLMSG_OK(hdr, (unsigned int) got);
             hdr = NLMSG_NEXT(hdr, got)) {
            if (hdr->nlmsg_seq != seq)
                continue;
            if (hdr->nlmsg_type == NLMSG_DONE) {
                if (hdr->nlmsg_len < NLMSG_LENGTH(sizeof(int))) {
                    fprintf(stderr, "netlink route done message too short: %u\n", hdr->nlmsg_len);
                    return 1;
                }
                saw_done = 1;
                break;
            }
            if (hdr->nlmsg_type == NLMSG_ERROR) {
                struct nlmsgerr *err = (struct nlmsgerr *) NLMSG_DATA(hdr);
                fprintf(stderr, "netlink route error %d\n", err->error);
                return 1;
            }
            if (hdr->nlmsg_type == RTM_NEWROUTE)
                saw_route++;
        }
        if (saw_done)
            break;
    }

    if (saw_route == 0) {
        fprintf(stderr, "missing expected route dump messages for routes\n");
        return 1;
    }
    return 0;
}

int main(void) {
    int fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
    if (fd < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_nl local = {
        .nl_family = AF_NETLINK,
    };
    if (bind(fd, (struct sockaddr *) &local, sizeof(local)) < 0) {
        perror("bind");
        close(fd);
        return 1;
    }

    if (dump_route_family(fd, RTM_GETLINK, 1, RTM_NEWLINK) != 0) {
        close(fd);
        return 1;
    }
    if (dump_route_family(fd, RTM_GETADDR, 2, RTM_NEWADDR) != 0) {
        close(fd);
        return 1;
    }
    if (dump_routes(fd, 3) != 0) {
        close(fd);
        return 1;
    }

    close(fd);
    puts("netlink route dump ok");
    return 0;
}
