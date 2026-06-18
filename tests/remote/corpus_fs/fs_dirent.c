/*
 * fs_dirent.c — readdir / getdents conformance.
 *
 * Order is unspecified (NOT asserted). We assert spec-guaranteed facts:
 *   - "." and ".." are always present; every created entry is present.
 *   - d_type is either DT_UNKNOWN (always legal) or matches lstat's type.
 *   - getdents with a buffer too small for even one entry returns EINVAL,
 *     not 0/EOF (iSH returned 0).
 */
#include "fs_common.h"

int main(int argc, char **argv) {
    fs_init(argc, argv, "dirent");

    mkdir("D", 0755);
    fs_mkfile("D/a", "1");
    fs_mkfile("D/b", "2");
    fs_mkfile("D/c", "3");
    mkdir("D/sd", 0755);
    symlink("a", "D/ln");

    int has_dot = 0, has_dotdot = 0, has_a = 0, has_b = 0, has_c = 0, has_sd = 0, has_ln = 0;
    int count = 0, dtype_all_ok = 1;

    DIR *dir = opendir("D");
    if (!dir) { perror("opendir"); return 2; }
    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        count++;
        const char *n = de->d_name;
        if (!strcmp(n, "."))  has_dot = 1;
        if (!strcmp(n, "..")) has_dotdot = 1;
        if (!strcmp(n, "a"))  has_a = 1;
        if (!strcmp(n, "b"))  has_b = 1;
        if (!strcmp(n, "c"))  has_c = 1;
        if (!strcmp(n, "sd")) has_sd = 1;
        if (!strcmp(n, "ln")) has_ln = 1;

        /* d_type must be DT_UNKNOWN or agree with lstat */
        if (de->d_type != DT_UNKNOWN) {
            char p[400];
            snprintf(p, sizeof p, "D/%s", n);
            struct stat st;
            if (lstat(p, &st) == 0) {
                int want;
                if (S_ISREG(st.st_mode))       want = DT_REG;
                else if (S_ISDIR(st.st_mode))  want = DT_DIR;
                else if (S_ISLNK(st.st_mode))  want = DT_LNK;
                else if (S_ISCHR(st.st_mode))  want = DT_CHR;
                else if (S_ISBLK(st.st_mode))  want = DT_BLK;
                else if (S_ISFIFO(st.st_mode)) want = DT_FIFO;
                else if (S_ISSOCK(st.st_mode)) want = DT_SOCK;
                else want = DT_UNKNOWN;
                if (de->d_type != want)
                    dtype_all_ok = 0;
            }
        }
    }
    closedir(dir);

    emit_i("dirent.has_dot", has_dot);
    emit_i("dirent.has_dotdot", has_dotdot);
    emit_i("dirent.has_a", has_a);
    emit_i("dirent.has_b", has_b);
    emit_i("dirent.has_c", has_c);
    emit_i("dirent.has_sd", has_sd);
    emit_i("dirent.has_ln", has_ln);
    emit_i("dirent.count", count);              /* . .. a b c sd ln = 7 */
    emit_i("dirent.dtype_ok", dtype_all_ok);

    /* getdents into a 1-byte buffer: EINVAL, not 0 */
    int fd = open("D", O_RDONLY | O_DIRECTORY);
    if (fd >= 0) {
        char tiny[1];
        long r = syscall(SYS_getdents64, fd, tiny, (unsigned) sizeof tiny);
        emit("dirent.getdents.tiny", "%s", res_errno(r));   /* EINVAL */
        close(fd);
    }

    /* getdents on a regular file -> ENOTDIR */
    fd = open("D/a", O_RDONLY);
    if (fd >= 0) {
        char buf[256];
        long r = syscall(SYS_getdents64, fd, buf, (unsigned) sizeof buf);
        emit("dirent.getdents.onfile", "%s", res_errno(r));  /* ENOTDIR */
        close(fd);
    }

    return 0;
}
