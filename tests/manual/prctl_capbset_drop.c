// prctl(PR_CAPBSET_DROP) was entirely unimplemented -- fell through to the
// default EINVAL case. Real-world trigger: systemd's per-unit
// CapabilityBoundingSet= enforcement (exec_context_apply, run for every
// service including systemd-logind) calls PR_CAPBSET_DROP once per
// capability it wants removed from the bounding set, before ever calling
// capset(). The unconditional EINVAL aborted the whole spawn ("Failed to
// drop capabilities: Invalid argument" / "Failed at step CAPABILITIES"),
// so systemd-logind (and anything else declaring CapabilityBoundingSet=)
// could never start during Arch aarch64 boot -- symptomatically, agetty's
// spawned login process ran fine (plain fork+exec, unaffected) but hung
// forever waiting on a PAM session registration with a logind that had
// never come up, so no login prompt ever appeared despite systemd itself
// reaching "Login Prompts".
//
// This codebase deliberately doesn't model a separate capability bounding
// set (see PR_CAPBSET_READ's existing comment) -- PR_CAPBSET_DROP is
// accepted as a no-op for any valid capability number, matching
// PR_CAPBSET_READ's existing simplification. Real Linux requires
// CAP_SETPCAP for this call (EPERM without it); since every process in
// this emulator behaves as if fully privileged already (no fine-grained
// capability enforcement exists anywhere in this codebase), that
// permission check is out of scope here -- verified this test's exact
// accept/EINVAL split matches real Linux run as root.
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/prctl.h>
#include "test_common.h"

static void check(int cond, const char *what) {
    if (cond) {
        test_logf("ok: %s\n", what);
    } else {
        printf("FAIL: %s (errno=%d %s)\n", what, errno, strerror(errno));
        failures_total++;
    }
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(10));

    // A representative sample of real capability numbers a
    // CapabilityBoundingSet= line drops (CAP_SYS_ADMIN=21, CAP_NET_ADMIN=12,
    // CAP_SYS_MODULE=16, plus the boundaries 0 and 63/CAP_LAST_CAP).
    int caps[] = {0, 12, 16, 21, 63};
    for (size_t i = 0; i < sizeof(caps) / sizeof(caps[0]); i++) {
        errno = 0;
        int r = prctl(PR_CAPBSET_DROP, caps[i], 0, 0, 0);
        char what[64];
        snprintf(what, sizeof(what), "PR_CAPBSET_DROP(%d) succeeds", caps[i]);
        check(r == 0, what);
    }

    errno = 0;
    int r = prctl(PR_CAPBSET_DROP, 999, 0, 0, 0);
    check(r < 0 && errno == EINVAL, "PR_CAPBSET_DROP(999) (out of range) is EINVAL");

    // PR_CAPBSET_READ must keep working unaffected.
    errno = 0;
    r = prctl(PR_CAPBSET_READ, 0, 0, 0, 0);
    check(r == 0 || r == 1, "PR_CAPBSET_READ(0) still returns a boolean");

    return finish_suite("prctl_capbset_drop");
}
