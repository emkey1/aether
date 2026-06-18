/*
 * sock_eventfd.c — eventfd(2) counter mode AND semaphore mode (EFD_SEMAPHORE),
 * EFD_NONBLOCK, the read/write size & value error corners, and poll readiness.
 *
 * eventfd is iSH's OWN code (kernel/eventfd.c), not host-backed, so any
 * divergence here is a directly fixable kernel-layer bug.
 *
 * Invariants asserted (all deterministic — no timing):
 *   - counter mode: a write of N adds N; the next read returns the full counter
 *     and resets it to 0; writes accumulate;
 *   - SEMAPHORE mode: a read returns 1 and decrements the counter by 1, so after
 *     write(3) exactly three reads of value 1 succeed and the fourth would block;
 *   - EFD_SEMAPHORE must be an ACCEPTED create flag (not EINVAL);
 *   - EFD_NONBLOCK: read of an empty counter -> EAGAIN; write that would overflow
 *     -> EAGAIN;
 *   - a read/write buffer smaller than 8 bytes -> EINVAL;
 *   - writing 0xffffffffffffffff -> EINVAL;
 *   - initval is the starting counter;
 *   - an unknown create flag bit -> EINVAL;
 *   - poll reports POLLIN iff counter > 0 and POLLOUT iff counter < UINT64_MAX-1.
 */
#include "sock_common.h"

#include <sys/eventfd.h>

#ifndef EFD_SEMAPHORE
#define EFD_SEMAPHORE 1
#endif
#ifndef EFD_NONBLOCK
#define EFD_NONBLOCK O_NONBLOCK
#endif
#ifndef EFD_CLOEXEC
#define EFD_CLOEXEC O_CLOEXEC
#endif

static int efd_read(int fd, uint64_t *out) {
    errno = 0;
    ssize_t n = read(fd, out, sizeof *out);
    if (n == (ssize_t) sizeof *out)
        return 0;
    return -1;
}
static int efd_write(int fd, uint64_t v) {
    errno = 0;
    ssize_t n = write(fd, &v, sizeof v);
    return n == (ssize_t) sizeof v ? 0 : -1;
}

/* counter (default) mode: accumulate then drain to zero */
static void case_counter(void) {
    int fd = eventfd(0, EFD_NONBLOCK);
    if (fd < 0) { emit("efd.counter.create", "%s", errno_name(errno)); return; }
    emit_b("efd.counter.empty_read_eagain", efd_read(fd, &(uint64_t){0}) < 0 && errno == EAGAIN);

    efd_write(fd, 1);
    efd_write(fd, 2);
    efd_write(fd, 3);
    uint64_t v = 0;
    int r = efd_read(fd, &v);
    emit_b("efd.counter.drains_sum", r == 0 && v == 6);
    /* after a drain the counter is 0 again */
    emit_b("efd.counter.empty_after_drain", efd_read(fd, &v) < 0 && errno == EAGAIN);
    close(fd);
}

/* initial value is the starting counter */
static void case_initval(void) {
    int fd = eventfd(5, EFD_NONBLOCK);
    if (fd < 0) { emit("efd.initval.create", "%s", errno_name(errno)); return; }
    uint64_t v = 0;
    int r = efd_read(fd, &v);
    emit_b("efd.initval.reads_initial", r == 0 && v == 5);
    close(fd);
}

/* SEMAPHORE mode: each read returns 1 and decrements by 1 */
static void case_semaphore(void) {
    errno = 0;
    int fd = eventfd(0, EFD_SEMAPHORE | EFD_NONBLOCK);
    /* the create itself must be accepted — iSH historically rejected
     * EFD_SEMAPHORE with EINVAL */
    emit("efd.sem.create", "%s", fd < 0 ? errno_name(errno) : "OK");
    if (fd < 0)
        return;

    efd_write(fd, 3);               /* counter = 3 */
    uint64_t a = 0, b = 0, c = 0, d = 99;
    int ra = efd_read(fd, &a);
    int rb = efd_read(fd, &b);
    int rc = efd_read(fd, &c);
    emit_b("efd.sem.three_reads_of_one",
           ra == 0 && a == 1 && rb == 0 && b == 1 && rc == 0 && c == 1);
    /* the fourth read finds an empty counter -> EAGAIN (nonblocking) */
    int rd = efd_read(fd, &d);
    emit_b("efd.sem.fourth_read_eagain", rd < 0 && errno == EAGAIN);

    /* in semaphore mode poll is readable while the counter is > 0 */
    efd_write(fd, 2);
    short rev = 0;
    int pr = sock_poll_one(fd, POLLIN, 0, &rev);
    emit_b("efd.sem.poll_readable", pr == 1 && (rev & POLLIN));
    uint64_t v = 0;
    int r1 = efd_read(fd, &v);                 /* returns 1, leaves 1 */
    emit_b("efd.sem.poll_still_readable_after_one",
           r1 == 0 && v == 1 &&
           sock_poll_one(fd, POLLIN, 0, &rev) == 1 && (rev & POLLIN));
    close(fd);
}

/* size and value error corners */
static void case_errors(void) {
    int fd = eventfd(0, EFD_NONBLOCK);
    if (fd < 0) { emit("efd.err.create", "%s", errno_name(errno)); return; }

    /* read into a < 8 byte buffer -> EINVAL */
    char small[4] = {0};
    errno = 0;
    ssize_t rn = read(fd, small, sizeof small);
    emit("efd.err.short_read", "%s", rn < 0 ? errno_name(errno) : "OK");

    /* write from a < 8 byte buffer -> EINVAL */
    errno = 0;
    ssize_t wn = write(fd, small, sizeof small);
    emit("efd.err.short_write", "%s", wn < 0 ? errno_name(errno) : "OK");

    /* writing the reserved 0xffffffffffffffff -> EINVAL */
    uint64_t bad = UINT64_MAX;
    errno = 0;
    ssize_t bn = write(fd, &bad, sizeof bad);
    emit("efd.err.write_uint64max", "%s", bn < 0 ? errno_name(errno) : "OK");
    close(fd);

    /* an unknown create flag bit -> EINVAL */
    errno = 0;
    int bf = eventfd(0, 0x04);        /* 0x04 is not SEMAPHORE/CLOEXEC/NONBLOCK */
    emit("efd.err.bad_create_flag", "%s", bf < 0 ? errno_name(errno) : "OK");
    if (bf >= 0) close(bf);
}

/* poll: not readable when empty, readable after a write, writable when low */
static void case_poll(void) {
    int fd = eventfd(0, EFD_NONBLOCK);
    if (fd < 0) { emit("efd.poll.create", "%s", errno_name(errno)); return; }
    short rev = 0;
    int pr0 = sock_poll_one(fd, POLLIN | POLLOUT, 0, &rev);
    emit_b("efd.poll.empty_not_readable", !(rev & POLLIN));
    emit_b("efd.poll.empty_writable", pr0 == 1 && (rev & POLLOUT));

    efd_write(fd, 1);
    int pr1 = sock_poll_one(fd, POLLIN, 0, &rev);
    emit_b("efd.poll.readable_after_write", pr1 == 1 && (rev & POLLIN));
    close(fd);
}

int main(int argc, char **argv) {
    sock_init(argc, argv, NULL);     /* no scratch dir needed */
    case_counter();
    case_initval();
    case_semaphore();
    case_errors();
    case_poll();
    return 0;
}
