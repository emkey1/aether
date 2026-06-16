// name_to_handle_at / open_by_handle_at must return a clean errno on amd64.
//
// Regression for missing amd64 syscalls 303 (name_to_handle_at) and 304
// (open_by_handle_at): they had no amd64 table entry, so the dispatcher logged
// "missing syscall 303" and stubbed them, and wiring a real stub naively would
// SIGSYS because their pointer args are 64-bit guest addresses (the stack lives
// high, e.g. 0x7fff_xxxx_xxxx) that the legacy arg marshaller rejects. They are
// now EOPNOTSUPP stubs classified as 0-arg (args ignored), matching the i386
// table. elogind/systemd call name_to_handle_at for a path's mount id and fall
// back to statx(STATX_MNT_ID)/proc mountinfo on EOPNOTSUPP.
//
// The test only requires a clean return (no SIGSYS): EOPNOTSUPP or ENOSYS, or a
// real success, are all acceptable. On i386 this has always been EOPNOTSUPP.
#define _GNU_SOURCE
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>
#include "test_common.h"

#ifndef SYS_name_to_handle_at
# ifdef __NR_name_to_handle_at
#  define SYS_name_to_handle_at __NR_name_to_handle_at
# elif defined(__x86_64__)
#  define SYS_name_to_handle_at 303
# else
#  define SYS_name_to_handle_at 341
# endif
#endif

int main(int argc, char **argv) {
    test_init(argc, argv);

    struct {
        unsigned handle_bytes;
        int handle_type;
        unsigned char f_handle[128];
    } handle;
    handle.handle_bytes = sizeof handle.f_handle;
    int mount_id = 0;

    // Reaching the next line at all proves no SIGSYS / no "missing syscall" kill.
    errno = 0;
    long r = syscall(SYS_name_to_handle_at, AT_FDCWD, "/", &handle, &mount_id, 0);
    int e = errno;

    if (r == 0) {
        test_logf("name_to_handle_at implemented (mount_id=%d)\n", mount_id);
    } else if (e == EOPNOTSUPP || e == ENOSYS) {
        test_logf("name_to_handle_at -> -1 %s (callers fall back) ok\n", strerror(e));
    } else {
        printf("FAIL: name_to_handle_at -> -1 %s (want EOPNOTSUPP/ENOSYS or success)\n", strerror(e));
        failures_total++;
    }

    return finish_suite("name_to_handle_at");
}
