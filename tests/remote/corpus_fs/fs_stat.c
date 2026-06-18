/*
 * fs_stat.c — stat / lstat / fstat / statx field correctness, chmod, umask,
 * truncate, lseek.
 *
 * The oracle (mint) runs as a NON-root user while iSH runs as root, so this
 * test asserts only uid-independent invariants: it never compares raw uid/gid
 * (only "owner == geteuid()"), never relies on root-vs-user permission outcomes,
 * and only chmods/truncates files it owns. Device/inode/blksize raw values are
 * fs-specific and emitted only as derived invariants.
 */
#include "fs_common.h"

static struct statx sx_buf;

int main(int argc, char **argv) {
    fs_init(argc, argv, "stat");

    /* ---- regular file basic fields ---- */
    umask(0);
    int fd = open("f", O_RDWR | O_CREAT, 0644);
    if (fd < 0) { perror("open"); return 2; }
    ssize_t w = write(fd, "hello", 5); (void) w;

    struct stat st;
    fstat(fd, &st);
    emit_i("stat.isreg", !!S_ISREG(st.st_mode));
    emit_i("stat.size", (long) st.st_size);          /* 5 */
    emit_i("stat.nlink", (long) st.st_nlink);        /* 1 */
    emit_i("stat.mode_bits", st.st_mode & 0777);     /* 0644 */
    emit_i("stat.blksize_pos", st.st_blksize > 0);   /* 1 */
    emit_i("stat.owner_is_self", st.st_uid == geteuid());
    emit_i("stat.group_is_self", st.st_gid == getegid());

    /* fstat == stat(path) for the same file */
    struct stat sp;
    stat("f", &sp);
    emit_i("stat.fstat_eq_stat",
           sp.st_ino == st.st_ino && sp.st_size == st.st_size && sp.st_mode == st.st_mode);

    /* ---- truncate grows/shrinks; size tracks ---- */
    emit("trunc.extend", "%s", res_errno(ftruncate(fd, 10)));
    fstat(fd, &st); emit_i("trunc.extend.size", (long) st.st_size);   /* 10 */
    emit("trunc.shrink", "%s", res_errno(ftruncate(fd, 3)));
    fstat(fd, &st); emit_i("trunc.shrink.size", (long) st.st_size);   /* 3 */
    emit("trunc.neg", "%s", res_errno(ftruncate(fd, -1)));            /* EINVAL */

    /* extended bytes read back as zeros */
    ftruncate(fd, 8);
    lseek(fd, 3, SEEK_SET);
    char zb[5]; ssize_t r = read(fd, zb, 5);
    int zeros = (r == 5);
    for (int i = 0; i < r; i++) if (zb[i] != 0) zeros = 0;
    emit_i("trunc.zerofill", zeros);

    /* ---- lseek ---- */
    emit_i("lseek.end", (long) lseek(fd, 0, SEEK_END));     /* 8 */
    emit("lseek.neg", "%s", res_errno(lseek(fd, -1, SEEK_SET)));  /* EINVAL */
    emit_i("lseek.past_end", (long) lseek(fd, 100, SEEK_SET));    /* 100 (allowed) */
    close(fd);

    /* ---- chmod round-trips mode bits ---- */
    emit("chmod.ok", "%s", res_errno(chmod("f", 0641)));
    stat("f", &st);
    emit_i("chmod.bits", st.st_mode & 07777);              /* 0641 */
    /* type bits preserved across chmod */
    emit_i("chmod.still_reg", !!S_ISREG(st.st_mode));

    /* ---- umask masks creation mode ---- */
    umask(027);
    int fd2 = open("um", O_WRONLY | O_CREAT, 0666);
    if (fd2 >= 0) { close(fd2); stat("um", &st); emit_i("umask.mode", st.st_mode & 0777); } /* 0640 */
    mkdir("umd", 0777);
    stat("umd", &st); emit_i("umask.dir_mode", st.st_mode & 0777);  /* 0750 */
    umask(0);

    /* ---- lstat vs stat on a symlink ---- */
    symlink("f", "sl");
    struct stat ls;
    lstat("sl", &ls); stat("sl", &st);
    emit_i("stat.sym.lstat_islnk", !!S_ISLNK(ls.st_mode));
    emit_i("stat.sym.stat_isreg", !!S_ISREG(st.st_mode));
    /* a symlink's lstat size equals the target-string length */
    emit_i("stat.sym.lsize", (long) ls.st_size);          /* strlen("f") = 1 */

    /* ---- type bits for special files ---- */
    struct stat dn;
    if (stat("/dev/null", &dn) == 0) emit_i("stat.devnull_ischr", !!S_ISCHR(dn.st_mode));
    mkdir("D", 0755);
    stat("D", &st); emit_i("stat.dir_isdir", !!S_ISDIR(st.st_mode));

    /* two distinct files differ in inode but share st_dev with their dir */
    fs_mkfile("a1", "a"); fs_mkfile("a2", "b");
    struct stat s1, s2, sd;
    stat("a1", &s1); stat("a2", &s2); stat(".", &sd);
    emit_i("stat.distinct_inode", s1.st_ino != s2.st_ino);
    emit_i("stat.same_dev", s1.st_dev == s2.st_dev && s1.st_dev == sd.st_dev);

    /* ---- statx basic fields agree with stat ---- */
    if (statx(AT_FDCWD, "f", 0, STATX_BASIC_STATS, &sx_buf) == 0) {
        stat("f", &st);
        emit_i("statx.size_eq", (long) sx_buf.stx_size == (long) st.st_size);
        emit_i("statx.isreg", !!S_ISREG(sx_buf.stx_mode));
        emit_i("statx.nlink_eq", sx_buf.stx_nlink == st.st_nlink);
        emit_i("statx.mode_eq", (sx_buf.stx_mode & 07777) == (st.st_mode & 07777));
        emit_i("statx.has_basic", !!(sx_buf.stx_mask & STATX_BASIC_STATS));
        emit_i("statx.owner_is_self", sx_buf.stx_uid == geteuid());
    } else {
        emit("statx.size_eq", "%s", res_errno(-1));
    }

    return 0;
}
