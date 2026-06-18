/*
 * fs_rename_link.c — rename / link / symlink / unlink / mkdir / rmdir + AT_*
 * and renameat2 flags.
 *
 * Candidate iSH divergences:
 *   - rmdir of a symlink-to-dir must be ENOTDIR; iSH followed the final symlink.
 *   - unlink of a directory must be EISDIR; iSH leaked the host errno (EPERM on
 *     Darwin/iOS hosts).
 *   - renameat2(RENAME_NOREPLACE) is supported by Linux; iSH rejected every
 *     non-zero flag with EINVAL.
 */
#include "fs_common.h"

#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE (1 << 0)
#endif
#ifndef RENAME_EXCHANGE
#define RENAME_EXCHANGE (1 << 1)
#endif

static long nlink_of(const char *p) {
    struct stat s;
    return lstat(p, &s) == 0 ? (long) s.st_nlink : -1;
}
static int same_inode(const char *a, const char *b) {
    struct stat sa, sb;
    if (lstat(a, &sa) || lstat(b, &sb)) return -1;
    return sa.st_ino == sb.st_ino && sa.st_dev == sb.st_dev;
}
static const char *exists_kind(const char *p) {
    struct stat s;
    if (lstat(p, &s) < 0) return errno_name(errno);
    if (S_ISDIR(s.st_mode)) return "DIR";
    if (S_ISLNK(s.st_mode)) return "LNK";
    if (S_ISREG(s.st_mode)) return "REG";
    return "OTHER";
}

int main(int argc, char **argv) {
    fs_init(argc, argv, "renamelink");

    /* ---- rename ---- */
    fs_mkfile("r_src", "data");
    emit("ren.basic", "%s", res_errno(rename("r_src", "r_dst")));
    emit("ren.src_gone", "%s", exists_kind("r_src"));        /* ENOENT */
    emit("ren.dst_there", "%s", exists_kind("r_dst"));       /* REG */
    emit("ren.noent", "%s", res_errno(rename("nope", "x")));  /* ENOENT */

    /* overwrite an existing file */
    fs_mkfile("o_a", "a"); fs_mkfile("o_b", "b");
    emit("ren.overwrite", "%s", res_errno(rename("o_a", "o_b")));  /* OK */

    /* file onto dir, dir onto file, dir onto nonempty dir */
    fs_mkfile("f1", "x"); mkdir("dq", 0755);
    emit("ren.file_onto_dir", "%s", res_errno(rename("f1", "dq")));   /* EISDIR */
    mkdir("d2", 0755); fs_mkfile("f2", "y");
    emit("ren.dir_onto_file", "%s", res_errno(rename("d2", "f2")));   /* ENOTDIR */
    mkdir("de", 0755); mkdir("dn", 0755); fs_mkfile("dn/keep", "z");
    emit("ren.dir_onto_nonempty", "%s", res_errno(rename("de", "dn"))); /* ENOTEMPTY or EEXIST */
    /* rename onto itself is a no-op success */
    fs_mkfile("self", "s");
    emit("ren.self", "%s", res_errno(rename("self", "self")));        /* OK */
    emit("ren.self_there", "%s", exists_kind("self"));               /* REG */
    /* newpath is a subdir of oldpath */
    mkdir("sub", 0755);
    emit("ren.into_subdir", "%s", res_errno(rename("sub", "sub/inner"))); /* EINVAL */

    /* ---- renameat2 flags ---- */
    fs_mkfile("nr_a", "a"); fs_mkfile("nr_b", "b");
    long rr = syscall(SYS_renameat2, AT_FDCWD, "nr_a", AT_FDCWD, "nr_b", RENAME_NOREPLACE);
    emit("ren2.noreplace.exist", "%s", res_errno(rr));   /* EEXIST */
    fs_mkfile("nr_c", "c");
    rr = syscall(SYS_renameat2, AT_FDCWD, "nr_c", AT_FDCWD, "nr_new", RENAME_NOREPLACE);
    emit("ren2.noreplace.new", "%s", res_errno(rr));     /* OK */
    emit("ren2.noreplace.moved", "%s", exists_kind("nr_new"));  /* REG */

    /* ---- link ---- */
    fs_mkfile("l_src", "data");
    emit("link.basic", "%s", res_errno(link("l_src", "l_dst")));   /* OK */
    emit_i("link.nlink", nlink_of("l_src"));                       /* 2 */
    emit_i("link.same_inode", same_inode("l_src", "l_dst"));       /* 1 */
    emit("link.exist", "%s", res_errno(link("l_src", "l_dst")));   /* EEXIST */
    emit("link.noent", "%s", res_errno(link("nope", "x")));        /* ENOENT */
    mkdir("ld", 0755);
    emit("link.dir", "%s", res_errno(link("ld", "ld2")));          /* EPERM */

    /* ---- symlink ---- */
    emit("sym.basic", "%s", res_errno(symlink("whatever", "s_link")));  /* OK */
    emit("sym.exist", "%s", res_errno(symlink("x", "s_link")));         /* EEXIST */
    emit("sym.kind", "%s", exists_kind("s_link"));                      /* LNK */

    /* ---- unlink ---- */
    fs_mkfile("u_f", "x");
    emit("unlink.file", "%s", res_errno(unlink("u_f")));      /* OK */
    emit("unlink.noent", "%s", res_errno(unlink("nope")));    /* ENOENT */
    mkdir("u_d", 0755);
    emit("unlink.dir", "%s", res_errno(unlink("u_d")));       /* EISDIR */
    /* unlink removes the symlink itself, not its target */
    fs_mkfile("u_tgt", "t"); symlink("u_tgt", "u_sl");
    emit("unlink.symlink", "%s", res_errno(unlink("u_sl")));  /* OK */
    emit("unlink.tgt_kept", "%s", exists_kind("u_tgt"));      /* REG */

    /* ---- mkdir ---- */
    emit("mkdir.basic", "%s", res_errno(mkdir("m_d", 0755)));  /* OK */
    emit_i("mkdir.nlink", nlink_of("m_d"));                    /* 2 */
    emit("mkdir.exist", "%s", res_errno(mkdir("m_d", 0755)));  /* EEXIST */
    fs_mkfile("m_f", "x");
    emit("mkdir.exist_file", "%s", res_errno(mkdir("m_f", 0755))); /* EEXIST */
    emit("mkdir.noent", "%s", res_errno(mkdir("nope/sub", 0755))); /* ENOENT */
    /* mkdir over a symlink is EEXIST: the final component is never followed,
     * for both a valid target and a dangling one */
    symlink("m_d", "m_sl");
    emit("mkdir.over_symlink", "%s", res_errno(mkdir("m_sl", 0755)));      /* EEXIST */
    symlink("m_nowhere", "m_dsl");
    emit("mkdir.over_dangling", "%s", res_errno(mkdir("m_dsl", 0755)));    /* EEXIST */

    /* ---- rmdir ---- */
    mkdir("rd", 0755);
    emit("rmdir.basic", "%s", res_errno(rmdir("rd")));        /* OK */
    emit("rmdir.noent", "%s", res_errno(rmdir("nope")));      /* ENOENT */
    mkdir("rne", 0755); fs_mkfile("rne/x", "z");
    emit("rmdir.nonempty", "%s", res_errno(rmdir("rne")));    /* ENOTEMPTY */
    fs_mkfile("rf", "x");
    emit("rmdir.onfile", "%s", res_errno(rmdir("rf")));       /* ENOTDIR */
    /* rmdir must NOT follow a final symlink-to-dir */
    mkdir("rtgt", 0755); symlink("rtgt", "rsl");
    emit("rmdir.symlink", "%s", res_errno(rmdir("rsl")));     /* ENOTDIR */
    emit("rmdir.symtgt_kept", "%s", exists_kind("rtgt"));     /* DIR (not removed) */

    /* ---- unlinkat AT_REMOVEDIR ---- */
    mkdir("ar_d", 0755);
    emit("unlinkat.removedir", "%s", res_errno(unlinkat(AT_FDCWD, "ar_d", AT_REMOVEDIR))); /* OK */
    fs_mkfile("ar_f", "x");
    emit("unlinkat.removedir_file", "%s", res_errno(unlinkat(AT_FDCWD, "ar_f", AT_REMOVEDIR))); /* ENOTDIR */

    return 0;
}
