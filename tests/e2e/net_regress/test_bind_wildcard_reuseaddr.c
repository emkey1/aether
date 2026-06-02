#include <arpa/inet.h>
#include <errno.h>
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

static void enable_reuseaddr(int fd, const char *what) {
    int one = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0)
        fail_errno(what);
}

int main(void) {
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    int probe = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0 || probe < 0)
        fail_errno("socket");

    enable_reuseaddr(listener, "setsockopt(listener)");
    enable_reuseaddr(probe, "setsockopt(probe)");

    struct sockaddr_in loopback = {
        .sin_family = AF_INET,
        .sin_port = htons(0),
        .sin_addr = {.s_addr = htonl(INADDR_LOOPBACK)},
    };
    if (bind(listener, (struct sockaddr *) &loopback, sizeof(loopback)) < 0)
        fail_errno("bind(listener)");
    if (listen(listener, 1) < 0)
        fail_errno("listen(listener)");

    socklen_t loopback_len = sizeof(loopback);
    if (getsockname(listener, (struct sockaddr *) &loopback, &loopback_len) < 0)
        fail_errno("getsockname(listener)");

    struct sockaddr_in wildcard = {
        .sin_family = AF_INET,
        .sin_port = loopback.sin_port,
        .sin_addr = {.s_addr = htonl(INADDR_ANY)},
    };
    if (bind(probe, (struct sockaddr *) &wildcard, sizeof(wildcard)) == 0) {
        fprintf(stderr, "bind(probe) unexpectedly succeeded\n");
        return 1;
    }
    if (errno != EADDRINUSE) {
        fprintf(stderr, "bind(probe) expected EADDRINUSE, got %d (%s)\n", errno, strerror(errno));
        return 1;
    }

    puts("wildcard reuseaddr bind conflict ok");
    close(probe);
    close(listener);
    return 0;
}
