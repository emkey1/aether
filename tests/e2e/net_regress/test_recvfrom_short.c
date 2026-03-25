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

    static const char message[] = "hello";
    if (write(pair[0], message, sizeof(message) - 1) != (ssize_t) (sizeof(message) - 1))
        fail("write failed");

    unsigned char buffer[32];
    memset(buffer, 'Z', sizeof(buffer));

    ssize_t got = recvfrom(pair[1], buffer, sizeof(buffer), 0, NULL, NULL);
    if (got != (ssize_t) (sizeof(message) - 1))
        fail("recvfrom length mismatch");
    if (memcmp(buffer, message, sizeof(message) - 1) != 0)
        fail("recvfrom payload mismatch");
    for (size_t i = sizeof(message) - 1; i < sizeof(buffer); i++) {
        if (buffer[i] != 'Z')
            fail("recvfrom clobbered tail bytes");
    }

    puts("recvfrom short ok");

    close(pair[1]);
    close(pair[0]);
    return 0;
}
