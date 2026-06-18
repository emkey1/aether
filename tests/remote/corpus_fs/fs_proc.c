/*
 * fs_proc.c — /proc consistency for the current process.
 *
 * Asserts the magic-symlink and listing behaviors that programs rely on:
 *   - readlink("/proc/self/fd/N") resolves to the file's real path.
 *   - readlink("/proc/self/cwd") is the current directory.
 *   - /proc/self/fd lists the open descriptors.
 *   - /proc/self/{stat,status,cmdline} are readable.
 * Values that vary per run (pid, exe path) are reduced to invariants.
 */
#include "fs_common.h"

static int readable(const char *p) {
    int fd = open(p, O_RDONLY);
    if (fd < 0) return 0;
    char b[64];
    ssize_t r = read(fd, b, sizeof b);
    close(fd);
    return r >= 0;
}

int main(int argc, char **argv) {
    fs_init(argc, argv, "proc");

    int fd = open("f", O_RDWR | O_CREAT, 0644);
    if (fd < 0) { perror("open"); return 2; }

    char link[400], procpath[64];
    snprintf(procpath, sizeof procpath, "/proc/self/fd/%d", fd);

    /* /proc/self/fd/N -> the real absolute path */
    ssize_t r = readlink(procpath, link, sizeof link - 1);
    if (r > 0) {
        link[r] = '\0';
        emit_i("proc.fd.resolves_to_file", strcmp(link, fs_path("f")) == 0);
    } else {
        emit("proc.fd.resolves_to_file", "%s", res_errno(r));
    }

    /* /proc/self/cwd -> the current directory */
    r = readlink("/proc/self/cwd", link, sizeof link - 1);
    if (r > 0) {
        link[r] = '\0';
        emit_i("proc.cwd.matches", strcmp(link, fs_base) == 0);
    } else {
        emit("proc.cwd.matches", "%s", res_errno(r));
    }

    /* /proc/self/fd lists 0,1,2 and our fd */
    int has0 = 0, has1 = 0, has2 = 0, hasfd = 0, count = 0;
    DIR *d = opendir("/proc/self/fd");
    if (d) {
        struct dirent *de;
        char want[16];
        snprintf(want, sizeof want, "%d", fd);
        while ((de = readdir(d)) != NULL) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            count++;
            if (!strcmp(de->d_name, "0")) has0 = 1;
            if (!strcmp(de->d_name, "1")) has1 = 1;
            if (!strcmp(de->d_name, "2")) has2 = 1;
            if (!strcmp(de->d_name, want)) hasfd = 1;
        }
        closedir(d);
    }
    emit_i("proc.fd.has_stdin", has0);
    emit_i("proc.fd.has_stdout", has1);
    emit_i("proc.fd.has_stderr", has2);
    emit_i("proc.fd.has_ourfd", hasfd);
    emit_i("proc.fd.count_pos", count > 0);

    /* core per-process files are readable */
    emit_i("proc.stat.readable", readable("/proc/self/stat"));
    emit_i("proc.status.readable", readable("/proc/self/status"));
    emit_i("proc.cmdline.readable", readable("/proc/self/cmdline"));
    emit_i("proc.maps.readable", readable("/proc/self/maps"));

    /* /proc/self is a symlink to the numeric pid; readlink yields digits */
    r = readlink("/proc/self", link, sizeof link - 1);
    int numeric = r > 0;
    for (ssize_t i = 0; i < r; i++) if (link[i] < '0' || link[i] > '9') numeric = 0;
    emit_i("proc.self.is_numeric", numeric);

    /* stat /proc and /proc/self */
    struct stat st;
    emit_i("proc.isdir", stat("/proc", &st) == 0 && S_ISDIR(st.st_mode));
    /* /proc/self/fd/N stat (via the magic link) sees the regular file */
    emit_i("proc.fd.stat_isreg", stat(procpath, &st) == 0 && S_ISREG(st.st_mode));

    close(fd);
    return 0;
}
