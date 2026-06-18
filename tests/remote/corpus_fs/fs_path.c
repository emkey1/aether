/*
 * fs_path.c — path resolution conformance: '.'/'..', trailing slash, symlinks,
 * O_NOFOLLOW, symlink loops, dangling links, readlink, empty path.
 *
 * Candidate iSH divergences this probes (all confirmed against mint's real
 * Linux by the conductor):
 *   - O_NOFOLLOW on a final symlink must fail ELOOP; iSH ignored the flag.
 *   - stat("/nonexistent/..") must be ENOENT; iSH normalized '..' lexically.
 *   - open("regfile/")/trailing slash on a non-dir must be ENOTDIR.
 *   - a path longer than PATH_MAX must be ENAMETOOLONG; iSH returned EFAULT
 *     because user_read_string conflated "too long" with a real memory fault.
 */
#include "fs_common.h"
#include <sys/inotify.h>
#include <sys/mount.h>

static int otype(const char *path, int flags) {
    /* open then report S_ISxxx of the result, or the errno symbol. Always pass a
     * mode so O_CREAT files get 0644 (a garbage mode would make the non-root
     * oracle return EACCES on a later open). */
    int fd = open(path, flags, 0644);
    if (fd < 0)
        return -errno;
    close(fd);
    return 0;
}

static const char *stat_kind(const char *path, int use_lstat) {
    struct stat st;
    int r = use_lstat ? lstat(path, &st) : stat(path, &st);
    if (r < 0)
        return errno_name(errno);
    if (S_ISREG(st.st_mode))  return "REG";
    if (S_ISDIR(st.st_mode))  return "DIR";
    if (S_ISLNK(st.st_mode))  return "LNK";
    if (S_ISCHR(st.st_mode))  return "CHR";
    if (S_ISBLK(st.st_mode))  return "BLK";
    if (S_ISFIFO(st.st_mode)) return "FIFO";
    if (S_ISSOCK(st.st_mode)) return "SOCK";
    return "OTHER";
}

int main(int argc, char **argv) {
    fs_init(argc, argv, "path");

    /* layout: dir a/b/c with file f; a regular file "reg"; dir "d" */
    mkdir("a", 0755); mkdir("a/b", 0755); mkdir("a/b/c", 0755);
    fs_mkfile("a/b/c/f", "hello");
    fs_mkfile("reg", "x");
    mkdir("d", 0755);

    /* ---- '.' and '..' ---- */
    emit("path.dotdot.reenter", "%s", res_errno(otype("a/b/c/../c/f", O_RDONLY)));
    emit("path.dot.chain",      "%s", res_errno(otype("a/./b/./c/f", O_RDONLY)));
    emit("path.dotdot.parent",  "%s", stat_kind("a/b/c/../..", 0));   /* -> a (DIR) */
    emit("path.dotdot.atroot",  "%s", stat_kind("/..", 0));           /* -> / (DIR) */

    /* '..' through a NONEXISTENT component must fail, not be popped lexically */
    emit("path.dotdot.nonexist",   "%s", stat_kind("nope/..", 0));        /* ENOENT */
    emit("path.dotdot.nonexist.ac","%s", res_errno(access("nope/..", F_OK)));
    emit("path.dotdot.nonexist.op","%s", res_errno(otype("nope/../reg", O_RDONLY)));
    /* '.' after a nonexistent component is also ENOENT */
    emit("path.dot.nonexist",      "%s", stat_kind("nope/.", 0));

    /* ---- trailing slash ---- */
    emit("path.slash.onreg.stat", "%s", stat_kind("reg/", 0));          /* ENOTDIR */
    emit("path.slash.onreg.open", "%s", res_errno(otype("reg/", O_RDONLY)));
    emit("path.slash.ondir.stat", "%s", stat_kind("d/", 0));            /* DIR */
    emit("path.slash.under.reg",  "%s", res_errno(otype("reg/foo", O_RDONLY))); /* ENOTDIR */

    /* O_CREAT with a trailing slash must fail (can't create a dir via open) */
    emit("path.slash.creat", "%s", res_errno(otype("newf/", O_RDONLY | O_CREAT | O_WRONLY)));

    /* ---- symlink following ---- */
    symlink("reg", "sl_reg");
    symlink("d",   "sl_dir");
    emit("path.sym.stat",    "%s", stat_kind("sl_reg", 0));   /* REG (followed) */
    emit("path.sym.lstat",   "%s", stat_kind("sl_reg", 1));   /* LNK */
    emit("path.sym.open",    "%s", res_errno(otype("sl_reg", O_RDONLY)));
    emit("path.symdir.trav", "%s", res_errno(otype("sl_dir/f2", O_RDONLY | O_CREAT | O_WRONLY)));

    char buf[64];
    ssize_t rl = readlink("sl_reg", buf, sizeof buf);
    emit_i("path.readlink.len", rl);
    if (rl > 0) { buf[rl] = '\0'; emit("path.readlink.val", "%s", buf); }
    emit("path.readlink.onreg", "%s", res_errno(readlink("reg", buf, sizeof buf)));  /* EINVAL */
    emit("path.readlink.noent", "%s", res_errno(readlink("nope", buf, sizeof buf))); /* ENOENT */

    /* readlink truncation: returns min(len,bufsize), never null-terminates */
    symlink("abcdefghij", "sl_long");
    char small[4];
    ssize_t tr = readlink("sl_long", small, sizeof small);
    emit_i("path.readlink.trunc.ret", tr);

    /* ---- O_NOFOLLOW ---- */
    emit("path.nofollow.sym",  "%s", res_errno(otype("sl_reg", O_RDONLY | O_NOFOLLOW))); /* ELOOP */
    emit("path.nofollow.reg",  "%s", res_errno(otype("reg",    O_RDONLY | O_NOFOLLOW))); /* OK */
    emit("path.nofollow.dir",  "%s", res_errno(otype("d", O_RDONLY | O_DIRECTORY | O_NOFOLLOW))); /* OK */
    /* O_NOFOLLOW only affects the final component: a symlink in the middle is followed */
    emit("path.nofollow.mid",  "%s", res_errno(otype("sl_dir/f2", O_RDONLY | O_NOFOLLOW)));

    /* ---- symlink loop ---- */
    symlink("loopB", "loopA");
    symlink("loopA", "loopB");
    emit("path.loop.open",     "%s", res_errno(otype("loopA", O_RDONLY)));  /* ELOOP */
    emit("path.loop.stat",     "%s", stat_kind("loopA", 0));                /* ELOOP */
    emit("path.loop.lstat",    "%s", stat_kind("loopA", 1));                /* LNK */
    rl = readlink("loopA", buf, sizeof buf);
    if (rl > 0) { buf[rl] = '\0'; emit("path.loop.readlink", "%s", buf); }  /* "loopB" */

    /* self-referential symlink */
    symlink("self", "self");
    emit("path.loop.self.open", "%s", res_errno(otype("self", O_RDONLY))); /* ELOOP */

    /* ---- dangling symlink ---- */
    symlink("nowhere", "dangle");
    emit("path.dangle.stat",  "%s", stat_kind("dangle", 0));   /* ENOENT (followed) */
    emit("path.dangle.lstat", "%s", stat_kind("dangle", 1));   /* LNK */
    emit("path.dangle.open",  "%s", res_errno(otype("dangle", O_RDONLY))); /* ENOENT */

    /* ---- empty path / ENAMETOOLONG ---- */
    emit("path.empty.open", "%s", res_errno(otype("", O_RDONLY)));  /* ENOENT */
    emit("path.empty.stat", "%s", stat_kind("", 0));               /* ENOENT */

    /* A path longer than PATH_MAX must fail ENAMETOOLONG, not EFAULT: user_read_path
     * tells "buffer overran before a NUL" apart from a real memory fault. (Single
     * 4999-char component overruns the MAX_PATH=4096 read buffer either way.) */
    char longname[5000];
    memset(longname, 'a', sizeof longname - 1);
    longname[sizeof longname - 1] = '\0';
    emit("path.toolong.open", "%s", res_errno(otype(longname, O_RDONLY)));   /* ENAMETOOLONG */
    emit("path.toolong.stat", "%s", stat_kind(longname, 0));                 /* ENAMETOOLONG */

    /* The same ENAMETOOLONG (not EFAULT) contract holds for every other syscall
     * that pulls a guest pathname through user_read_path. Regression-lock the
     * sites converted alongside open/stat: execve/execveat (kernel/exec.c),
     * inotify_add_watch (kernel/inotify.c), and the mount point + umount2 target
     * (fs/mount.c). In each, the pathname is copied in before any argv/mask/
     * permission/existence check, so even a non-root caller observes
     * ENAMETOOLONG first on both iSH and mint -- we keep the other arguments
     * valid so nothing else fails first.
     *
     * NB: mount's *source* is deliberately NOT tested here. In Linux it goes
     * through copy_mount_string() (strndup_user, EINVAL when over-long), not
     * getname(), so it is not a PATH_MAX/ENAMETOOLONG pathname -- the over-long
     * path lives in the mount *point* (a real getname path) instead. */
    char *const tl_argv[] = { (char *) "x", NULL };
    char *const tl_envp[] = { NULL };
    emit("path.toolong.execve",   "%s", res_errno(execve(longname, tl_argv, tl_envp)));
    emit("path.toolong.execveat", "%s",
         res_errno(syscall(SYS_execveat, AT_FDCWD, longname, tl_argv, tl_envp, 0)));

    int ifd = inotify_init();
    emit("path.toolong.inotify", "%s",
         ifd < 0 ? errno_name(errno)
                 : res_errno(inotify_add_watch(ifd, longname, IN_MODIFY)));
    if (ifd >= 0)
        close(ifd);

    /* over-long mount point (short, valid source) and over-long umount target */
    emit("path.toolong.mount",  "%s", res_errno(mount("src", longname, "tmpfs", 0, NULL)));
    emit("path.toolong.umount", "%s", res_errno(umount2(longname, 0)));

    return 0;
}
