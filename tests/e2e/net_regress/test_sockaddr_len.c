#define _GNU_SOURCE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void fail_errno(const char *what) {
    perror(what);
    exit(1);
}

static void test_bind_ipv4_oversized_len(void) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        fail_errno("socket(AF_INET)");

    struct {
        struct sockaddr_in addr;
        char pad[12];
    } storage;
    memset(&storage, 0, sizeof(storage));
    struct sockaddr_in *addr = &storage.addr;
    addr->sin_family = AF_INET;
    addr->sin_port = 0;
    addr->sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(fd, (struct sockaddr *) &storage, sizeof(storage)) != 0)
        fail_errno("bind(AF_INET oversized)");

    close(fd);
}

static void test_bind_ipv6_oversized_len(void) {
    int fd = socket(AF_INET6, SOCK_DGRAM, 0);
    if (fd < 0)
        fail_errno("socket(AF_INET6)");

    struct {
        struct sockaddr_in6 addr;
        char pad[4];
    } storage;
    memset(&storage, 0, sizeof(storage));
    struct sockaddr_in6 *addr = &storage.addr;
    static const struct in6_addr any = IN6ADDR_ANY_INIT;
    addr->sin6_family = AF_INET6;
    addr->sin6_port = 0;
    addr->sin6_addr = any;

    if (bind(fd, (struct sockaddr *) &storage, sizeof(storage)) != 0)
        fail_errno("bind(AF_INET6 oversized)");

    close(fd);
}

int main(void) {
    test_bind_ipv4_oversized_len();
    test_bind_ipv6_oversized_len();
    puts("sockaddr len normalization ok");
    return 0;
}
