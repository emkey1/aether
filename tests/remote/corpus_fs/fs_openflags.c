/*
 * fs_openflags.c — open(2) flag semantics + F_GETFL reporting.
 *
 * Candidate iSH divergence: F_GETFL must report only the access mode and file
 * STATUS flags; the creation flags (O_CREAT/O_EXCL/O_NOCTTY/O_TRUNC) and
 * O_CLOEXEC are stripped by Linux after open. iSH stored the raw open flags and
 * returned them verbatim. We emit per-bit booleans (O_LARGEFILE differs by arch,
 * so never compare the raw value).
 */
#include "fs_common.h"

#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif

static int getfl(int fd) { return fcntl(fd, F_GETFL); }

int main(int argc, char **argv) {
    fs_init(argc, argv, "openflags");

    fs_mkfile("exist", "data");
    mkdir("dir", 0755);

    /* ---- creation flags ---- */
    emit("of.excl.exist",  "%s", res_errno(open("exist", O_WRONLY | O_CREAT | O_EXCL, 0644)));   /* EEXIST */
    int fd = open("newx", O_WRONLY | O_CREAT | O_EXCL, 0644);
    emit("of.excl.new",    "%s", res_errno(fd));                                                 /* OK */
    if (fd >= 0) close(fd);
    emit("of.creat.exist", "%s", res_errno(open("exist", O_WRONLY | O_CREAT, 0644)));            /* OK */
    emit("of.noent",       "%s", res_errno(open("nope", O_RDONLY)));                             /* ENOENT */

    /* ---- O_DIRECTORY / EISDIR ---- */
    emit("of.directory.onfile", "%s", res_errno(open("exist", O_RDONLY | O_DIRECTORY)));  /* ENOTDIR */
    emit("of.directory.ondir",  "%s", res_errno(open("dir", O_RDONLY | O_DIRECTORY)));    /* OK */
    emit("of.dir.wronly",       "%s", res_errno(open("dir", O_WRONLY)));                  /* EISDIR */
    emit("of.dir.rdwr",         "%s", res_errno(open("dir", O_RDWR)));                    /* EISDIR */
    emit("of.dir.rdonly",       "%s", res_errno(open("dir", O_RDONLY)));                  /* OK */

    /* O_RDWR|O_WRONLY (access mode 3) is POSIX-undefined: Linux accepts it
     * (opens with neither read nor write), iSH rejects it with EINVAL. Not
     * asserted -- this is application error territory, not a conformance bug. */

    /* ---- O_TRUNC truncates ---- */
    fs_mkfile("tr", "AAAA");
    fd = open("tr", O_WRONLY | O_TRUNC);
    if (fd >= 0) {
        struct stat st; fstat(fd, &st);
        emit_i("of.trunc.size", (long) st.st_size);   /* 0 */
        close(fd);
    }
    /* O_TRUNC | O_RDONLY still truncates on Linux */
    fs_mkfile("tr2", "BBBB");
    fd = open("tr2", O_RDONLY | O_TRUNC);
    if (fd >= 0) { struct stat st; fstat(fd, &st); emit_i("of.trunc.rdonly.size", (long) st.st_size); close(fd); }

    /* ---- O_APPEND forces writes to EOF regardless of seek ---- */
    fs_mkfile("ap", "AAAA");
    fd = open("ap", O_WRONLY | O_APPEND);
    if (fd >= 0) {
        lseek(fd, 0, SEEK_SET);
        ssize_t w = write(fd, "BB", 2); (void) w;
        struct stat st; fstat(fd, &st);
        emit_i("of.append.size", (long) st.st_size);   /* 6 */
        close(fd);
    }

    /* ---- F_GETFL: which bits survive ---- */
    fd = open("gf", O_RDONLY | O_CREAT | O_EXCL | O_TRUNC | O_NOCTTY | O_CLOEXEC, 0644);
    if (fd >= 0) {
        int fl = getfl(fd);
        emit_i("of.getfl.accmode",   fl & O_ACCMODE);
        emit_i("of.getfl.has_creat", !!(fl & O_CREAT));
        emit_i("of.getfl.has_excl",  !!(fl & O_EXCL));
        emit_i("of.getfl.has_trunc", !!(fl & O_TRUNC));
        emit_i("of.getfl.has_noctty",!!(fl & O_NOCTTY));
        emit_i("of.getfl.has_cloexec",!!(fl & O_CLOEXEC));
        emit_i("of.getfl.has_append",!!(fl & O_APPEND));
        close(fd);
    }

    /* status flags ARE reported */
    fd = open("gf2", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) {
        int fl = getfl(fd);
        emit_i("of.getfl.wr.accmode",  fl & O_ACCMODE);   /* O_WRONLY */
        emit_i("of.getfl.wr.append",   !!(fl & O_APPEND)); /* 1 */
        close(fd);
    }

    /* F_SETFL toggles O_NONBLOCK/O_APPEND; F_GETFL reflects it */
    fd = open("gf3", O_WRONLY | O_CREAT, 0644);
    if (fd >= 0) {
        fcntl(fd, F_SETFL, O_NONBLOCK | O_APPEND);
        int fl = getfl(fd);
        emit_i("of.setfl.nonblock", !!(fl & O_NONBLOCK));
        emit_i("of.setfl.append",   !!(fl & O_APPEND));
        close(fd);
    }

    return 0;
}
