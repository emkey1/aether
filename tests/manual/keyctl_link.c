// keyctl(KEYCTL_LINK, ...) must not ENOSYS: systemd-executor's setup_keyring()
// runs this for every service with the default KeyringMode=shared (link the
// user keyring into the freshly joined session keyring), right after
// KEYCTL_JOIN_SESSION_KEYRING. kernel/misc.c's sys_keyctl is a stub with no
// real keyring backing store, but it already treats KEYCTL_JOIN_SESSION_KEYRING/
// KEYCTL_GET_KEYRING_ID/KEYCTL_SETPERM as no-op successes -- KEYCTL_LINK was
// missing from that list and fell to the default ENOSYS, so every service
// spawn logged "Failed to link user keyring into session keyring: Function
// not implemented" (non-fatal via log_error_errno but part of a fatal
// sequence for units like tmp.mount that route through the KEYRING step).
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "test_common.h"

#ifndef SYS_keyctl
# define SYS_keyctl 219 // "common" number on i386/amd64/arm64
#endif

#define KEYCTL_GET_KEYRING_ID 0
#define KEYCTL_JOIN_SESSION_KEYRING 1
#define KEYCTL_LINK 8
#define KEYCTL_SETPERM 5

#define KEY_SPEC_SESSION_KEYRING (-3)
#define KEY_SPEC_USER_KEYRING (-4)

static long raw_keyctl(int cmd, long a2, long a3, long a4, long a5) {
    return syscall(SYS_keyctl, cmd, a2, a3, a4, a5);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(5));

    long session = raw_keyctl(KEYCTL_JOIN_SESSION_KEYRING, 0, 0, 0, 0);
    test_logf("KEYCTL_JOIN_SESSION_KEYRING -> %ld\n", session);
    if (session < 0) {
        printf("FAIL: KEYCTL_JOIN_SESSION_KEYRING -> %ld (%s)\n", session, strerror(errno));
        failures_total++;
    }

    long r = raw_keyctl(KEYCTL_LINK, KEY_SPEC_USER_KEYRING, session, 0, 0);
    test_logf("KEYCTL_LINK(user -> session) -> %ld errno=%d\n", r, errno);
    if (r != 0) {
        printf("FAIL: KEYCTL_LINK -> %ld (%s) -- this is exactly what made "
               "\"Failed to link user keyring into session keyring\" fatal "
               "for every clone3-spawned service\n", r, strerror(errno));
        failures_total++;
    }

    r = raw_keyctl(KEYCTL_SETPERM, session, 0x3f3f0000, 0, 0);
    test_log_if(r == 0, "KEYCTL_SETPERM ok\n");
    if (r != 0) {
        printf("FAIL: KEYCTL_SETPERM -> %ld (%s)\n", r, strerror(errno));
        failures_total++;
    }

    return finish_suite("keyctl_link");
}
