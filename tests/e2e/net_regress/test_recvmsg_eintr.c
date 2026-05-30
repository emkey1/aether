#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

static volatile sig_atomic_t handler_count;
static volatile sig_atomic_t last_sig;

static pid_t gettid_linux(void) {
    return (pid_t) syscall(SYS_gettid);
}

static void failf(const char *msg, int a, int b, int c, int ea, int eb, int ec) {
    fprintf(stderr, "%s: got=(%d,%d,%d) expected=(%d,%d,%d)\n", msg, a, b, c, ea, eb, ec);
    exit(1);
}

static void on_signal(int sig) {
    handler_count++;
    last_sig = sig;
}

struct sender_args {
    pid_t pid;
    pid_t tid;
};

static void *sender_thread(void *arg_) {
    struct sender_args *arg = arg_;
    usleep(20000);
    syscall(SYS_tgkill, arg->pid, arg->tid, SIGUSR1);
    return NULL;
}

int main(void) {
    struct sigaction act = {
        .sa_handler = on_signal,
        .sa_flags = 0,
    };
    sigemptyset(&act.sa_mask);
    if (sigaction(SIGUSR1, &act, NULL) != 0) {
        perror("sigaction");
        return 1;
    }

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = 0,
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };
    if (bind(fd, (struct sockaddr *) &addr, sizeof(addr)) != 0) {
        perror("bind");
        return 1;
    }

    pthread_t thread;
    struct sender_args args = {
        .pid = getpid(),
        .tid = gettid_linux(),
    };
    if (pthread_create(&thread, NULL, sender_thread, &args) != 0) {
        perror("pthread_create");
        return 1;
    }

    char buf[16];
    struct iovec iov = {.iov_base = buf, .iov_len = sizeof(buf)};
    struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1};
    errno = 0;
    ssize_t rc = recvmsg(fd, &msg, 0);
    int err = errno;
    pthread_join(thread, NULL);

    if (rc != -1 || err != EINTR || handler_count != 1 || last_sig != SIGUSR1)
        failf("recvmsg EINTR", (int) rc, err, handler_count, -1, EINTR, 1);

    close(fd);
    puts("recvmsg EINTR ok");
    return 0;
}
