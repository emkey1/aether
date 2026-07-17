// getpeername()/getsockname() with a small or zero-length caller buffer.
//
// fs/sock.c's sys_getpeername_common/sys_getsockname_common sized their
// local scratch buffer with a VLA declared as `char sockaddr[sockaddr_len]`,
// where sockaddr_len came straight from the guest -- e.g. 0, or anything
// smaller than sizeof(sa_family_t), which POSIX explicitly allows (the
// syscall is supposed to just truncate, never error, and report the true
// required length). Two distinct bugs fell out of that:
//
//   1. Generic (AF_INET/etc.) path: sockaddr_write() read the address family
//      out of the front of that buffer BEFORE any truncation was applied.
//      With a too-small guest length, the host getpeername()/getsockname()
//      call wrote nothing into the (correspondingly too-small) buffer, so
//      that read pulled uninitialized stack garbage -- surfaced as a bogus
//      EINVAL when the garbage didn't decode to a recognized family
//      (stress-ng's --sock stressor: "getpeername failed, errno=22").
//   2. AF_UNIX path: copy_unix_name() computed
//      `*sockaddr_len - offsetof(struct sockaddr_, data)` with an unsigned
//      subtraction. A guest length smaller than that offset (2 bytes)
//      underflowed to a huge size_t, which then drove a memset/memcpy far
//      past the end of the (possibly zero-sized) stack buffer -- a real
//      stack-corruption bug, more severe than the EINVAL case.
//
// Fixed by always using a fixed-size local buffer for the host call
// (independent of the guest's requested length) and only truncating the
// copy back to the guest at the very end, matching real getpeername/
// getsockname semantics (verified against a real kernel).
//
// A third, unrelated but adjacent issue is covered here too: a live
// getpeername() on a connected TCP socket occasionally (racily, roughly
// 1-in-several-thousand under stress-ng's tight connect/getpeername loop)
// gets host EINVAL on this codebase's macOS/iOS host where Linux would
// return ENOTCONN or still succeed -- a Darwin socket-state quirk, not
// reproducible as a small-buffer issue. Since the host call now always uses
// our own valid full-size buffer, a host EINVAL here cannot mean a bad
// addrlen (Linux's only documented EINVAL case for this call), so it's
// mapped to ENOTCONN -- a real Linux errno instead of a leaked host one.
// That race isn't practical to hit deterministically in a regression test;
// this file only covers the deterministic small/zero-length-buffer bugs.
#define _GNU_SOURCE
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "test_common.h"

static void check_errno(const char *label, int ret, int want_ret, int want_errno) {
    int err = errno;
    if (ret != want_ret || (want_ret < 0 && err != want_errno)) {
        printf("FAIL %s: ret=%d errno=%d(%s) want ret=%d errno=%d\n",
               label, ret, err, strerror(err), want_ret, want_errno);
        failures_total++;
    } else {
        test_logf("%s: ret=%d errno=%d ok\n", label, ret, err);
    }
}

int main(int argc, char **argv) {
    test_init(argc, argv);

    // AF_INET UDP, connect()ed so getpeername has a real answer.
    {
        int s = socket(AF_INET, SOCK_DGRAM, 0);
        struct sockaddr_in a = {0};
        a.sin_family = AF_INET;
        a.sin_port = htons(9);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (connect(s, (struct sockaddr *) &a, sizeof(a)) < 0) {
            printf("FAIL: connect: %s\n", strerror(errno));
            failures_total++;
        }

        struct sockaddr_storage ss;
        socklen_t len = 0;
        errno = 0;
        check_errno("INET getpeername len=0", getpeername(s, (struct sockaddr *) &ss, &len), 0, 0);
        if (len != sizeof(struct sockaddr_in)) {
            printf("FAIL INET getpeername len=0: out len=%u want %zu\n",
                   (unsigned) len, sizeof(struct sockaddr_in));
            failures_total++;
        }

        char tiny;
        len = 1;
        errno = 0;
        check_errno("INET getpeername len=1", getpeername(s, (struct sockaddr *) &tiny, &len), 0, 0);
        if (len != sizeof(struct sockaddr_in)) {
            printf("FAIL INET getpeername len=1: out len=%u want %zu\n",
                   (unsigned) len, sizeof(struct sockaddr_in));
            failures_total++;
        }

        len = 0;
        errno = 0;
        check_errno("INET getsockname len=0", getsockname(s, (struct sockaddr *) &ss, &len), 0, 0);

        close(s);
    }

    // AF_UNIX (socketpair): the copy_unix_name stack-corruption path.
    // A too-small buffer must not crash the process and must behave like
    // real Linux (success, truncated copy, true length reported).
    {
        int sv[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
            printf("FAIL: socketpair: %s\n", strerror(errno));
            return finish_suite("getpeername_smallbuf");
        }

        struct sockaddr_storage ss;
        socklen_t len = 0;
        errno = 0;
        check_errno("UNIX getpeername len=0", getpeername(sv[0], (struct sockaddr *) &ss, &len), 0, 0);

        char tiny;
        len = 1;
        errno = 0;
        check_errno("UNIX getpeername len=1", getpeername(sv[0], (struct sockaddr *) &tiny, &len), 0, 0);

        len = 0;
        errno = 0;
        check_errno("UNIX getsockname len=0", getsockname(sv[0], (struct sockaddr *) &ss, &len), 0, 0);

        len = 1;
        errno = 0;
        check_errno("UNIX getsockname len=1", getsockname(sv[0], (struct sockaddr *) &tiny, &len), 0, 0);

        close(sv[0]);
        close(sv[1]);
    }

    printf("getpeername_smallbuf: process survived (no stack corruption)\n");
    return finish_suite("getpeername_smallbuf");
}
