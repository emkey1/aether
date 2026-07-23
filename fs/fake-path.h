#ifndef FS_FAKE_PATH_H
#define FS_FAKE_PATH_H

// fakefs stores guest files on the host filesystem under their guest names,
// but the host filesystem (APFS on iOS/macOS) is case-insensitive while the
// guest expects case sensitivity. Guest names differing only in ASCII case
// would collide in the same host directory: installing ncurses-terminfo in an
// Alpine guest silently lost /usr/share/terminfo/{a,e,l,...} because their
// uppercase twins {A,E,L,...} already existed as host directories.
//
// So guest names are escaped before they touch the host filesystem:
//
//     'A'..'Z'  ->  '%' followed by the lowercase letter  ("Foo" -> "%foo")
//     '%'       ->  "%%"
//
// Everything else (including '/', so whole paths can be escaped in one pass)
// is stored as-is. The escaped form contains no ASCII uppercase, so any two
// distinct guest names map to host names that also differ under
// case-insensitive comparison. The mapping is reversible; readdir() results
// are decoded back to guest names with fake_path_from_host().
//
// The metadata DB always stores unescaped guest paths; only the host
// filesystem sees the escaped form. Existing roots are converted by the
// version-4 migration in fs/fake-migrate.c, and fakefs_import() writes new
// roots directly in escaped form.
//
// Known limitation: only ASCII case is escaped. Non-ASCII names that differ
// only in Unicode case or normalization (e.g. e-acute NFC vs NFD) can still
// collide on APFS.
//
// This header must stay dependency-free: it is included from ish core code,
// the host-side fakefsify tools, the iOS FileProvider extension, and the
// Linux kernel port (linux/fakefs.c), which has no libc headers.

#define FAKE_PATH_ESCAPE_CHAR '%'

// Escape a guest path (or single name) into its host on-disk form. Writes at
// most bufsize bytes including the NUL terminator. Returns buf, or a null
// pointer if the escaped form doesn't fit (the caller should fail with
// ENAMETOOLONG). Escaping at most doubles the length.
static inline char *fake_path_to_host(const char *path, char *buf, unsigned long bufsize) {
    unsigned long i = 0;
    for (; *path != '\0'; path++) {
        char c = *path;
        if (c >= 'A' && c <= 'Z') {
            if (i + 2 >= bufsize)
                return 0;
            buf[i++] = FAKE_PATH_ESCAPE_CHAR;
            buf[i++] = c + ('a' - 'A');
        } else if (c == FAKE_PATH_ESCAPE_CHAR) {
            if (i + 2 >= bufsize)
                return 0;
            buf[i++] = FAKE_PATH_ESCAPE_CHAR;
            buf[i++] = FAKE_PATH_ESCAPE_CHAR;
        } else {
            if (i + 1 >= bufsize)
                return 0;
            buf[i++] = c;
        }
    }
    buf[i] = '\0';
    return buf;
}

// Decode a host on-disk path (or single name) back to the guest form, in
// place (decoding never grows the string). Escape sequences not produced by
// fake_path_to_host (e.g. a stray '%' in a file the user dropped into the
// data directory by hand) are kept literally.
static inline void fake_path_from_host(char *path) {
    char *in = path, *out = path;
    while (*in != '\0') {
        char c = *in++;
        if (c == FAKE_PATH_ESCAPE_CHAR) {
            if (*in >= 'a' && *in <= 'z') {
                *out++ = *in++ - ('a' - 'A');
                continue;
            }
            if (*in == FAKE_PATH_ESCAPE_CHAR) {
                *out++ = FAKE_PATH_ESCAPE_CHAR;
                in++;
                continue;
            }
        }
        *out++ = c;
    }
    *out = '\0';
}

// True if the host on-disk form of this guest path differs from the path
// itself (i.e. it contains bytes fake_path_to_host would escape).
static inline int fake_path_needs_escape(const char *path) {
    for (; *path != '\0'; path++) {
        if ((*path >= 'A' && *path <= 'Z') || *path == FAKE_PATH_ESCAPE_CHAR)
            return 1;
    }
    return 0;
}

#endif
