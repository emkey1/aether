// Covers two related pty/tty line-discipline gaps (issue #423 Tier 3):
//  1. ECHOKE vs plain ECHOK on VKILL -- and the fact that ECHOKE_ was
//     previously defined at the wrong termios c_lflag bit (1<<6, which is
//     actually ECHONL's position) so it never matched what any real program
//     set via tcsetattr.
//  2. tty_poll() unconditionally reported POLLOUT even when the peer side of
//     a pty had a full input buffer and a write would actually block/EAGAIN.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pty.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include "test_common.h"

static void dump_buf(char *out, const char *buf, ssize_t n) {
    size_t o = 0;
    for (ssize_t i = 0; i < n; i++) {
        unsigned char c = buf[i];
        if (c == '\r') { out[o++] = '\\'; out[o++] = 'r'; }
        else if (c == '\n') { out[o++] = '\\'; out[o++] = 'n'; }
        else if (c < 32 || c == 127) { out[o++] = '^'; out[o++] = c ^ 0x40; }
        else out[o++] = c;
    }
    out[o] = '\0';
}

static void test_kill_echo(const char *name, int echoke_on, const char *expected) {
    int master, slave;
    if (openpty(&master, &slave, NULL, NULL, NULL) != 0) {
        printf("FAIL %s: openpty failed\n", name);
        failures_total++;
        return;
    }
    struct termios t;
    tcgetattr(slave, &t);
    t.c_lflag |= ECHO | ECHOK | ICANON;
    if (echoke_on)
        t.c_lflag |= ECHOKE;
    else
        t.c_lflag &= ~ECHOKE;
    tcsetattr(slave, TCSANOW, &t);

    char kill_char = t.c_cc[VKILL];
    if (write(master, "abc", 3) != 3 || write(master, &kill_char, 1) != 1) {
        printf("FAIL %s: write failed\n", name);
        failures_total++;
        close(master); close(slave);
        return;
    }
    usleep(100000);

    char buf[256];
    ssize_t n = read(master, buf, sizeof(buf));
    char got[512];
    dump_buf(got, buf, n < 0 ? 0 : n);

    test_logf("%s: got=\"%s\"\n", name, got);
    if (strcmp(got, expected) != 0) {
        printf("FAIL %s: got=\"%s\" expected=\"%s\"\n", name, got, expected);
        failures_total++;
    }
    close(master);
    close(slave);
}

static int pollout_ready(int fd) {
    struct pollfd pfd = {.fd = fd, .events = POLLOUT};
    if (poll(&pfd, 1, 0) < 0)
        return -1;
    return (pfd.revents & POLLOUT) ? 1 : 0;
}

static void test_pty_pollout_backpressure(void) {
    int master, slave;
    if (openpty(&master, &slave, NULL, NULL, NULL) != 0) {
        printf("FAIL pty_pollout_backpressure: openpty failed\n");
        failures_total++;
        return;
    }
    struct termios t;
    tcgetattr(slave, &t);
    cfmakeraw(&t);
    tcsetattr(slave, TCSANOW, &t);
    fcntl(master, F_SETFL, O_NONBLOCK);

    int initial = pollout_ready(master);

    char chunk[4096];
    memset(chunk, 'x', sizeof(chunk));
    ssize_t total = 0;
    for (int i = 0; i < 20; i++) {
        ssize_t n = write(master, chunk, sizeof(chunk));
        if (n < 0)
            break;
        total += n;
    }
    int full = pollout_ready(master);

    char drain[8192];
    read(slave, drain, sizeof(drain));
    usleep(50000);
    int after_drain = pollout_ready(master);

    test_logf("pty_pollout_backpressure: initial=%d full=%d(after %zd bytes) after_drain=%d\n",
              initial, full, total, after_drain);

    if (initial != 1 || full != 0 || after_drain != 1) {
        printf("FAIL pty_pollout_backpressure: initial=%d full=%d after_drain=%d (want 1,0,1)\n",
               initial, full, after_drain);
        failures_total++;
    }
    close(master);
    close(slave);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    test_kill_echo("kill_with_echoke", 1, "abc^H ^H^H ^H^H ^H");
    test_kill_echo("kill_without_echoke", 0, "abc^U\\r\\n");
    test_pty_pollout_backpressure();
    return finish_suite("pty_line_discipline");
}
