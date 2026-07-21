#ifndef FS_FAKEFSIFY_H
#define FS_FAKEFSIFY_H
#include <stdbool.h>

struct fakefsify_error {
    int line;
    enum {
        ERR_ARCHIVE,
        ERR_SQLITE,
        ERR_POSIX,
        ERR_CANCELLED,
    } type;
    int code;
    char *message;
};

struct progress {
    void *cookie;
    void (*callback)(void *cookie, double progress, const char *message, bool *cancel_out);
};

// Pin the process LC_CTYPE to a UTF-8 locale so libarchive can convert UTF-8
// path/linkpath entries (e.g. Debian/Devuan tarballs) instead of failing the
// read. Idempotent; safe to call repeatedly.
void fakefs_ensure_utf8_locale(void);
bool fakefs_import(const char *archive_path, const char *fs, struct fakefsify_error *err_out, struct progress progress);
// Create a brand-new, empty fakefs at `fs` (data/ + meta.db) containing only
// a root directory inode (mode 01777, /tmp-style, so every guest uid can
// create entries). Fails if `fs` already exists. Used for the shared
// /AOK/fakefs filesystem, or any create-empty-filesystem flow that doesn't
// start from an archive.
bool fakefs_init_empty(const char *fs, struct fakefsify_error *err_out);
bool fakefs_export(const char *fs, const char *archive_path, struct fakefsify_error *err_out, struct progress progress);

#endif
