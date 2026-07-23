#ifndef FS_FAKE_PATH_H
#define FS_FAKE_PATH_H

// fakefs stores guest files on the host filesystem under their guest names,
// but the host filesystem (APFS on iOS/macOS) is case-insensitive and
// normalization-insensitive while the guest expects byte-exact names. Guest
// names that APFS considers equal would collide in the same host directory:
//
//  - ASCII case: installing ncurses-terminfo in an Alpine guest silently
//    lost /usr/share/terminfo/{a,e,l,...} because their uppercase twins
//    {A,E,L,...} already existed as host directories.
//  - Unicode case folding: "Ф" and "ф" are the same file to APFS.
//  - Unicode normalization: NFC "é" (C3 A9) and NFD "é" (65 CC 81) are
//    canonically equivalent and collide, and some codepoints are ignored
//    outright by the folding tables.
//
// So guest names are escaped before they touch the host filesystem:
//
//     'A'..'Z'     ->  '%' followed by the lowercase letter ("Foo" -> "%foo")
//     '%'          ->  "%%"
//     byte >= 0x80 ->  '%' followed by two characters encoding the low 7
//                      bits: '0'+(bits>>4) (giving '0'..'7') then the low
//                      nibble as a lowercase hex digit. 0xC3 -> "%43".
//
// The three forms are unambiguous on decode: after '%', a lowercase letter
// means an uppercase original, a '%' means a literal '%', and a digit '0'-'7'
// starts a two-character high-byte escape. Everything else (lowercase ASCII,
// digits, ASCII punctuation and control bytes, and '/') passes through, so
// whole paths can be escaped in one pass. ASCII other than 'A'-'Z' is inert
// under both APFS case folding and normalization, and the escaped output
// contains nothing else, so any two distinct guest names map to host names
// that APFS also keeps distinct. The mapping is reversible; readdir() results
// are decoded back to guest names with fake_path_from_host().
//
// The metadata DB always stores unescaped guest paths; only the host
// filesystem sees the escaped form. Existing roots are converted by the
// version-4/5 migrations in fs/fake-migrate.c (v4 escaped only ASCII case;
// v5 rewrites those roots to this full form), and fakefs_import() writes new
// roots directly in escaped form.
//
// Tradeoff: escaping inflates names (3 bytes per non-ASCII byte, so up to
// 3x), so very long non-ASCII names can exceed the host's 255-byte name
// limit and fail with ENAMETOOLONG (a ~250-byte all-CJK guest name needs
// ~750 host bytes). That clean error replaces silent collision/corruption.
//
// This header must stay dependency-free: it is included from ish core code,
// the host-side fakefsify tools, the iOS FileProvider extension, and the
// Linux kernel port (linux/fakefs.c), which has no libc headers.

#define FAKE_PATH_ESCAPE_CHAR '%'

// Escape a guest path (or single name) into its host on-disk form. Writes at
// most bufsize bytes including the NUL terminator. Returns buf, or a null
// pointer if the escaped form doesn't fit (the caller should fail with
// ENAMETOOLONG). Escaping at most triples the length.
static inline char *fake_path_to_host(const char *path, char *buf, unsigned long bufsize) {
    unsigned long i = 0;
    for (; *path != '\0'; path++) {
        unsigned char c = (unsigned char) *path;
        if (c >= 'A' && c <= 'Z') {
            if (i + 2 >= bufsize)
                return 0;
            buf[i++] = FAKE_PATH_ESCAPE_CHAR;
            buf[i++] = (char) (c + ('a' - 'A'));
        } else if (c == FAKE_PATH_ESCAPE_CHAR) {
            if (i + 2 >= bufsize)
                return 0;
            buf[i++] = FAKE_PATH_ESCAPE_CHAR;
            buf[i++] = FAKE_PATH_ESCAPE_CHAR;
        } else if (c >= 0x80) {
            unsigned char low7 = c & 0x7f;
            if (i + 3 >= bufsize)
                return 0;
            buf[i++] = FAKE_PATH_ESCAPE_CHAR;
            buf[i++] = (char) ('0' + (low7 >> 4));
            buf[i++] = "0123456789abcdef"[low7 & 0xf];
        } else {
            if (i + 1 >= bufsize)
                return 0;
            buf[i++] = (char) c;
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
            if (*in >= '0' && *in <= '7') {
                char hex = in[1];
                int nibble = -1;
                if (hex >= '0' && hex <= '9')
                    nibble = hex - '0';
                else if (hex >= 'a' && hex <= 'f')
                    nibble = hex - 'a' + 10;
                if (nibble >= 0) {
                    *out++ = (char) (0x80 | ((*in - '0') << 4) | nibble);
                    in += 2;
                    continue;
                }
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
        unsigned char c = (unsigned char) *path;
        if ((c >= 'A' && c <= 'Z') || c == FAKE_PATH_ESCAPE_CHAR || c >= 0x80)
            return 1;
    }
    return 0;
}

#endif
