#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static void fail(const char *why) {
    fprintf(stderr, "%s\n", why);
    exit(1);
}

static void bind_path(int fd, const char *path) {
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    size_t path_len = strlen(path);
    if (path_len >= sizeof(addr.sun_path))
        fail("unix path too long");
    memcpy(addr.sun_path, path, path_len + 1);
    socklen_t addr_len = offsetof(struct sockaddr_un, sun_path) + path_len + 1;
    unlink(path);
    if (bind(fd, (struct sockaddr *) &addr, addr_len) < 0)
        fail("bind failed");
}

static void expect_peername(int fd, const char *path) {
    struct sockaddr_un addr = {};
    socklen_t addr_len = sizeof(addr);
    if (getpeername(fd, (struct sockaddr *) &addr, &addr_len) < 0)
        fail("getpeername failed");
    if (addr.sun_family != AF_UNIX)
        fail("unexpected peer family");
    if (strcmp(addr.sun_path, path) != 0)
        fail("unexpected peer path");
}

int main(void) {
    static const char server_path[] = "/tmp/net_regress_peer_server.sock";
    static const char client_path[] = "/tmp/net_regress_peer_client.sock";

    int server = socket(AF_UNIX, SOCK_STREAM, 0);
    int client = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server < 0 || client < 0)
        fail("socket failed");

    bind_path(server, server_path);
    bind_path(client, client_path);

    if (listen(server, 1) < 0)
        fail("listen failed");

    struct sockaddr_un server_addr = {.sun_family = AF_UNIX};
    memcpy(server_addr.sun_path, server_path, sizeof(server_path));
    socklen_t server_addr_len = offsetof(struct sockaddr_un, sun_path) + sizeof(server_path);
    if (connect(client, (struct sockaddr *) &server_addr, server_addr_len) < 0)
        fail("connect failed");

    int accepted = accept(server, NULL, NULL);
    if (accepted < 0)
        fail("accept failed");

    expect_peername(client, server_path);
    expect_peername(accepted, client_path);

    puts("unix peername ok");

    close(accepted);
    close(client);
    close(server);
    unlink(client_path);
    unlink(server_path);
    return 0;
}
