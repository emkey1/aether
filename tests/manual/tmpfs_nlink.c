// tmpfs st_nlink maintenance. Was: fs/tmp.c never set stat.nlink, so every
// tmpfs file reported st_nlink == 0 -- which systemd-journald's
// journal_file_fstat() treats as "file already deleted" (-EIDRM, part of its
// journal-corruption error set), so every journal file it created on the
// /run tmpfs was instantly declared "corrupted or uncleanly shut down,
// renaming and replacing" and the Journal Service crash-looped at boot.
//
// Covers Linux semantics: new file nlink 1; new dir nlink 2; parent gains a
// link per subdirectory ("..") and drops it on rmdir; an unlinked-but-open
// file's fstat shows nlink 0; rename-overwrite drops the replaced file's
// link; a directory renamed across parents moves its ".." link with it.
// Mounts a private tmpfs to test in (same pattern as tmpfs_mmap.c) -- /tmp
// itself may be fakefs/realfs in a CLI guest, whose host backing (e.g. APFS)
// has its own directory-nlink convention. Skips if the mount is denied.
// Also passes on real Linux (as root).
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

#include "test_common.h"

static void check_nlink(const char *what, nlink_t got, nlink_t want) {
    test_log_if(got == want, "%s nlink=%ju (want %ju)\n", what,
                (uintmax_t) got, (uintmax_t) want);
    if (got != want) {
        printf("FAIL: %s nlink=%ju (want %ju)\n", what, (uintmax_t) got,
               (uintmax_t) want);
        failures_total++;
    }
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(10));

    char mnt[] = "/tmp/tmpfs_nlink_mnt.XXXXXX";
    if (mkdtemp(mnt) == NULL) {
        perror("mkdtemp");
        return 1;
    }
    if (mount("tmpfs", mnt, "tmpfs", 0, NULL) != 0) {
        printf("tmpfs_nlink: SKIP (cannot mount tmpfs: %s)\n", strerror(errno));
        rmdir(mnt);
        return 0;
    }
    char base[512];
    snprintf(base, sizeof(base), "%s/work", mnt);
    if (mkdir(base, 0755) != 0) {
        perror("mkdir work");
        return 1;
    }
    struct stat st;
    char path[512], path2[512];

    // Fresh directory: 2 ("." + parent entry).
    if (stat(base, &st) != 0) { perror("stat base"); return 1; }
    check_nlink("fresh dir", st.st_nlink, 2);

    // Fresh regular file: 1.
    snprintf(path, sizeof(path), "%s/file", base);
    int fd = open(path, O_RDWR | O_CREAT, 0644);
    if (fd < 0) { perror("open"); return 1; }
    if (fstat(fd, &st) != 0) { perror("fstat"); return 1; }
    check_nlink("fresh file", st.st_nlink, 1);

    // Subdirectory bumps the parent (its "..").
    snprintf(path2, sizeof(path2), "%s/sub", base);
    if (mkdir(path2, 0755) != 0) { perror("mkdir"); return 1; }
    stat(base, &st);
    check_nlink("parent after mkdir", st.st_nlink, 3);
    stat(path2, &st);
    check_nlink("new subdir", st.st_nlink, 2);

    // rmdir drops it again.
    if (rmdir(path2) != 0) { perror("rmdir"); return 1; }
    stat(base, &st);
    check_nlink("parent after rmdir", st.st_nlink, 2);

    // Unlinked-but-open file: fstat reports 0 (the journald check).
    if (unlink(path) != 0) { perror("unlink"); return 1; }
    if (fstat(fd, &st) != 0) { perror("fstat after unlink"); return 1; }
    check_nlink("unlinked open file", st.st_nlink, 0);
    close(fd);

    // Rename-overwrite: the replaced file's inode drops to 0.
    snprintf(path, sizeof(path), "%s/a", base);
    snprintf(path2, sizeof(path2), "%s/b", base);
    int afd = open(path, O_RDWR | O_CREAT, 0644);
    int bfd = open(path2, O_RDWR | O_CREAT, 0644);
    if (afd < 0 || bfd < 0) { perror("open a/b"); return 1; }
    if (rename(path, path2) != 0) { perror("rename overwrite"); return 1; }
    fstat(bfd, &st);
    check_nlink("replaced file (open fd)", st.st_nlink, 0);
    fstat(afd, &st);
    check_nlink("surviving file", st.st_nlink, 1);
    close(afd);
    close(bfd);
    unlink(path2);

    // Directory moved across parents carries its ".." link.
    snprintf(path, sizeof(path), "%s/d1", base);
    snprintf(path2, sizeof(path2), "%s/d2", base);
    mkdir(path, 0755);
    mkdir(path2, 0755);
    stat(base, &st);
    check_nlink("base with two subdirs", st.st_nlink, 4);
    char moved[512];
    snprintf(moved, sizeof(moved), "%s/d2/d1", base);
    if (rename(path, moved) != 0) { perror("rename dir"); return 1; }
    stat(base, &st);
    check_nlink("base after dir moved out", st.st_nlink, 3);
    stat(path2, &st);
    check_nlink("new parent after dir moved in", st.st_nlink, 3);

    rmdir(moved);
    rmdir(path2);
    rmdir(base);
    umount2(mnt, MNT_DETACH);
    rmdir(mnt);

    return finish_suite("tmpfs_nlink");
}
