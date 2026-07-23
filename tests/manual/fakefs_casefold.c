// Regression test for fakefs case sensitivity on case-insensitive hosts.
//
// fakefs stores guest files on the host filesystem (APFS on iOS/macOS, which
// is case-insensitive) and historically used the raw guest names as host
// names. Guest paths differing only in ASCII case then collided in the same
// host directory: in an Alpine guest, installing ncurses-terminfo lost
// /usr/share/terminfo/{a,e,l,m,n,p,q,x} because their uppercase twins
// {A,E,L,M,N,P,Q,X} already existed -- apk reported "failed to extract ...:
// No such file or directory", mkdir returned EEXIST for a directory that ls
// didn't show, and files silently shared content with their case twins.
//
// Fixed by escaping guest names into a case-collision-free on-disk form
// ('A' -> "%a", '%' -> "%%"; fs/fake-path.h). This test exercises the guest-
// visible contract: names differing only in case are independent files, and
// names containing the escape character round-trip exactly.

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>

#include "test_common.h"

#define FAKEFS_MAGIC 0x66616b65

static char g_dir[512];

static void check(int cond, const char *what) {
    if (cond) {
        test_logf("ok: %s\n", what);
    } else {
        printf("FAIL fakefs_casefold: %s (errno=%d %s)\n", what, errno, strerror(errno));
        failures_total++;
    }
}

static int write_file(const char *name, const char *content) {
    char path[600];
    snprintf(path, sizeof(path), "%s/%s", g_dir, name);
    int fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0644);
    if (fd < 0)
        return -1;
    ssize_t n = write(fd, content, strlen(content));
    close(fd);
    return n == (ssize_t) strlen(content) ? 0 : -1;
}

static int read_file(const char *name, char *buf, size_t size) {
    char path[600];
    snprintf(path, sizeof(path), "%s/%s", g_dir, name);
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    ssize_t n = read(fd, buf, size - 1);
    close(fd);
    if (n < 0)
        return -1;
    buf[n] = '\0';
    return 0;
}

static int file_content_is(const char *name, const char *expected) {
    char buf[64];
    if (read_file(name, buf, sizeof(buf)) < 0)
        return 0;
    return strcmp(buf, expected) == 0;
}

// Count entries of the test dir and check the exact-name presence of each
// expected entry (excluding "." and "..").
static int dir_matches(const char *const *names, unsigned count) {
    DIR *dir = opendir(g_dir);
    if (dir == NULL)
        return 0;
    unsigned seen = 0;
    int ok = 1;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        seen++;
        int found = 0;
        for (unsigned i = 0; i < count; i++) {
            if (strcmp(ent->d_name, names[i]) == 0) {
                found = 1;
                break;
            }
        }
        if (!found) {
            test_log_if(1, "unexpected direntry: %s\n", ent->d_name);
            ok = 0;
        }
    }
    closedir(dir);
    return ok && seen == count;
}

static void cleanup_tree(void) {
    char cmd[600];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_dir);
    int res = system(cmd);
    (void) res;
}

int main(int argc, char **argv) {
    test_init(argc, argv);

    const char *base = getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp";
    snprintf(g_dir, sizeof(g_dir), "%s/fakefs_casefold.%d", base, (int) getpid());

    struct statfs sfs;
    if (statfs(base, &sfs) == 0 && sfs.f_type != FAKEFS_MAGIC) {
        // e.g. tmpfs mounted on /tmp: nothing here would touch fakefs
        printf("fakefs_casefold: SKIP (%s is not on fakefs)\n", base);
        return 0;
    }

    if (mkdir(g_dir, 0755) < 0) {
        printf("FAIL fakefs_casefold: mkdir %s: %s\n", g_dir, strerror(errno));
        return 1;
    }

    // Case-distinct regular files are independent.
    check(write_file("x", "lower") == 0, "create file 'x'");
    check(write_file("X", "upper") == 0, "create case-twin file 'X' (O_EXCL)");
    check(file_content_is("x", "lower"), "'x' keeps its own content");
    check(file_content_is("X", "upper"), "'X' keeps its own content");

    struct stat st_lower, st_upper;
    char path_lower[600], path_upper[600];
    snprintf(path_lower, sizeof(path_lower), "%s/x", g_dir);
    snprintf(path_upper, sizeof(path_upper), "%s/X", g_dir);
    check(stat(path_lower, &st_lower) == 0 && stat(path_upper, &st_upper) == 0 &&
          st_lower.st_ino != st_upper.st_ino, "'x' and 'X' are distinct inodes");

    // Case-distinct directories are independent (the terminfo failure mode:
    // mkdir of the lowercase twin returned EEXIST).
    char path_a[600], path_A[600], path_af[600];
    snprintf(path_a, sizeof(path_a), "%s/a", g_dir);
    snprintf(path_A, sizeof(path_A), "%s/A", g_dir);
    check(mkdir(path_A, 0755) == 0, "mkdir 'A'");
    check(mkdir(path_a, 0755) == 0, "mkdir case-twin 'a'");
    snprintf(path_af, sizeof(path_af), "%s/a/inner", g_dir);
    check(write_file("a/inner", "in-a") == 0, "create file inside 'a'");
    struct stat st_dir;
    snprintf(path_af, sizeof(path_af), "%s/A/inner", g_dir);
    check(stat(path_af, &st_dir) < 0 && errno == ENOENT,
          "'A' does not see the file created inside 'a'");

    // Escape-character robustness: literal '%' names round-trip and don't
    // collide with the escaped forms of other names.
    check(write_file("%x", "pct-x") == 0, "create file '%x'");
    check(write_file("%%", "pct-pct") == 0, "create file '%%'");
    check(write_file("%X", "pct-upper") == 0, "create file '%X'");
    check(file_content_is("%x", "pct-x"), "'%x' keeps its own content");
    check(file_content_is("%%", "pct-pct"), "'%%' keeps its own content");
    check(file_content_is("%X", "pct-upper"), "'%X' keeps its own content");
    check(file_content_is("X", "upper"), "'X' unaffected by '%x' twin games");

    // Symlink under an uppercase name.
    char path_link[600];
    snprintf(path_link, sizeof(path_link), "%s/LINK", g_dir);
    check(symlink("x", path_link) == 0, "symlink 'LINK' -> 'x'");
    char target[64];
    ssize_t tlen = readlink(path_link, target, sizeof(target) - 1);
    if (tlen >= 0)
        target[tlen] = '\0';
    check(tlen == 1 && strcmp(target, "x") == 0, "readlink 'LINK'");

    // readdir returns the exact guest names, nothing more or less.
    const char *const expected[] = {"x", "X", "a", "A", "%x", "%%", "%X", "LINK"};
    check(dir_matches(expected, sizeof(expected)/sizeof(expected[0])),
          "readdir lists exact case-distinct names");

    // Case-only rename leaves exactly one entry under the new name.
    char path_case[600], path_CASE[600];
    snprintf(path_case, sizeof(path_case), "%s/case", g_dir);
    snprintf(path_CASE, sizeof(path_CASE), "%s/CASE", g_dir);
    check(write_file("case", "renamed") == 0, "create file 'case'");
    check(rename(path_case, path_CASE) == 0, "case-only rename 'case' -> 'CASE'");
    check(stat(path_case, &st_dir) < 0 && errno == ENOENT, "'case' gone after rename");
    check(file_content_is("CASE", "renamed"), "'CASE' present after rename");

    // Unlinking one case twin leaves the other.
    check(unlink(path_upper) == 0, "unlink 'X'");
    check(file_content_is("x", "lower"), "'x' survives unlink of 'X'");
    check(stat(path_upper, &st_dir) < 0 && errno == ENOENT, "'X' really gone");

    cleanup_tree();
    return finish_suite("fakefs_casefold");
}
