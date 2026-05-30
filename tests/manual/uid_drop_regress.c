#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

static void fail_errno(const char *what) {
    fprintf(stderr, "%s: %s\n", what, strerror(errno));
    exit(1);
}

static void fail_msg(const char *what) {
    fprintf(stderr, "%s\n", what);
    exit(1);
}

int main(void) {
    uid_t old_uid = getuid();
    gid_t old_gid = getgid();
    uid_t target_uid = 65534;
    gid_t target_gid = 65534;

    if (old_uid != 0)
        fail_msg("must run as root");

    if (setresgid(target_gid, target_gid, target_gid) < 0)
        fail_errno("setresgid");
    if (setresuid(target_uid, target_uid, target_uid) < 0)
        fail_errno("setresuid");

    errno = 0;
    if (setgid(old_gid) != -1)
        fail_msg("setgid unexpectedly restored old gid");
    errno = 0;
    if (setegid(old_gid) != -1)
        fail_msg("setegid unexpectedly restored old gid");
    errno = 0;
    if (setuid(old_uid) != -1)
        fail_msg("setuid unexpectedly restored old uid");
    errno = 0;
    if (seteuid(old_uid) != -1)
        fail_msg("seteuid unexpectedly restored old uid");

    if (getuid() != target_uid || geteuid() != target_uid)
        fail_msg("uid drop did not stick");
    if (getgid() != target_gid || getegid() != target_gid)
        fail_msg("gid drop did not stick");

    puts("uid_drop_regress ok");
    return 0;
}
