#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void die(const char *what) {
    perror(what);
    exit(1);
}

static void enable_reuse(int fd, const char *label) {
    int one = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0)
        die(label);
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one)) < 0)
        die(label);
}

int main(void) {
    int first = socket(AF_INET, SOCK_DGRAM, 0);
    int second = socket(AF_INET, SOCK_DGRAM, 0);
    if (first < 0 || second < 0)
        die("socket");

    enable_reuse(first, "setsockopt(first)");
    enable_reuse(second, "setsockopt(second)");

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(0),
        .sin_addr = {.s_addr = htonl(INADDR_LOOPBACK)},
    };
    if (bind(first, (struct sockaddr *) &addr, sizeof(addr)) < 0)
        die("bind(first)");

    socklen_t addr_len = sizeof(addr);
    if (getsockname(first, (struct sockaddr *) &addr, &addr_len) < 0)
        die("getsockname(first)");

    if (bind(second, (struct sockaddr *) &addr, sizeof(addr)) < 0)
        die("bind(second)");

    puts("SO_REUSEPORT ok");

    close(second);
    close(first);
    return 0;
}
